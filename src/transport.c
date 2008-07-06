#include "transport.h"
#include "util.h"
#include "socket_includes.h"

#define RL_TRANSPORT_MAX_POOLED_BUFFERS (4)

static int rl_iobuf_init(rl_iobuf_t *buf, size_t size)
{
	buf->base_address = (char*) rl_alloc_sized(size);
	buf->end_address = buf->base_address + size;
	buf->write_cursor = buf->base_address;
	buf->read_cursor = buf->base_address;
	buf->valid = buf->base_address ? 1 : 0;
	return buf->base_address ? 0 : 1;
}

static void rl_iobuf_destroy(rl_iobuf_t *buf)
{
	if (buf->valid)
		rl_free_sized(buf->base_address, buf->end_address - buf->base_address);
	buf->valid = 0;
}

int
rl_transport_init(rl_transport_t *t, const rl_transport_callbacks_t *callbacks, size_t buffer_size, void *userdata)
{
	rl_memset(t, 0, sizeof(rl_transport_t));

	RL_ASSERT(callbacks);
	RL_ASSERT(callbacks->peek_incoming);
	RL_ASSERT(callbacks->deliver_incoming);

	if (0 != rl_iobuf_init(&t->inbuf, buffer_size))
		goto cleanup;

	t->callbacks = callbacks;
	t->userdata = userdata;
	return 0;

cleanup:
	rl_transport_destroy(t);
	return 1;
}

void
rl_transport_destroy(rl_transport_t *t)
{
	rl_transport_buf_t *msg;
	
	msg = t->out_queue;
	while (msg)
	{
		rl_transport_buf_t *next = msg->next;
		rl_transport_free_buffer(t, msg);
		msg = next;
	}

	msg = t->next_free_buffer;
	while (msg)
	{
		rl_transport_buf_t *next = msg->next;
		rl_free_sized(msg, msg->total_size);
		msg = next;
	}

	rl_iobuf_destroy(&t->inbuf);
}

int
rl_transport_update(rl_transport_t *t)
{
	int status = 0;

	if (t->error)
		return RL_TRANSPORT_ERROR;
	if (t->disconnect)
		return RL_TRANSPORT_DISCONNECTED;

	while (t->inbuf.read_cursor < t->inbuf.write_cursor)
	{
		int msg_size;
		const ptrdiff_t avail_size = t->inbuf.write_cursor - t->inbuf.read_cursor;

		msg_size = (*t->callbacks->peek_incoming)(t, t->inbuf.read_cursor, avail_size);

		/* error? */
		if (msg_size <= 0)
		{
			if (msg_size < 0)
			{
				t->error = 1;
				return RL_TRANSPORT_ERROR;
			}
			break;
		}
		else if (msg_size > (t->inbuf.end_address - t->inbuf.base_address))
		{
			RL_LOG_WARNING(("message size %d will never fit in buffer", msg_size));
			t->error = 1;
			return RL_TRANSPORT_ERROR;
		}

		/* enough data available? */
		if (avail_size < msg_size)
			break;

		/* deliver the message */
		if (0 != (*t->callbacks->deliver_incoming)(t, t->inbuf.read_cursor, (size_t) msg_size))
		{
			t->error = 1;
			return RL_TRANSPORT_ERROR;
		}

		/* advance buffer */
		t->inbuf.read_cursor += msg_size;
	}

	if (t->inbuf.read_cursor != t->inbuf.base_address)
	{
		/* move back remaining (partial) data to the front of the buffer */
		size_t trim = t->inbuf.write_cursor - t->inbuf.read_cursor;
		if (trim > 0)
			rl_memmove(t->inbuf.base_address, t->inbuf.read_cursor, trim);
		t->inbuf.read_cursor = t->inbuf.base_address;
		t->inbuf.write_cursor = t->inbuf.read_cursor + trim;
	}

	if (t->out_queue)
		status |= RL_TRANSPORT_NEED_OUTPUT;

	return status;
}

void
rl_transport_on_input_arrived(rl_transport_t *t, rl_socket_t sock)
{
	/* read as much as possible */
	int read_result;
	const int max_buffer_left = (int) (t->inbuf.end_address - t->inbuf.write_cursor);
   
	read_result = recv(sock, t->inbuf.write_cursor, max_buffer_left, 0 /* flags */);

	RL_LOG_DEBUG(("read %d bytes (avail space pre:%d post:%d)",
				read_result, max_buffer_left, max_buffer_left - read_result));

	if (0 == read_result)
	{
		t->disconnect = 1;
	}
	else if (-1 == read_result)
	{
		if (RL_LAST_SOCKET_ERROR != EWOULDBLOCK)
			t->error = 1;	
	}
	else
	{
		t->inbuf.write_cursor += read_result;
	}
}

void
rl_transport_on_output_possible(rl_transport_t *t, rl_socket_t sock)
{
	while (t->out_queue)
	{
		rl_transport_buf_t *msg = t->out_queue;
		int write_rc = 0;

		write_rc = send(sock, (const char *)msg->buffer + (msg->used_size - msg->remaining), (int) msg->remaining, /* flags*/ 0);
		RL_LOG_DEBUG(("wrote %d bytes", write_rc));

		if (write_rc < 0)
		{
			if (EWOULDBLOCK != RL_LAST_SOCKET_ERROR)
				t->error = 1;
			break;
		}

		if (0 == write_rc)
			break;

		msg->remaining -= write_rc;

		if (0 == msg->remaining)
		{
			t->out_queue = msg->next;
			rl_transport_free_buffer(t, msg);
		}
	}
}

int
rl_transport_add_output_message(rl_transport_t *self, rl_transport_buf_t *buf)
{
	buf->remaining = buf->used_size;

	if (!self->out_queue)
	{
		self->out_queue = buf;
		self->out_tail = buf;
	}
	else
	{
		self->out_tail->next = buf;
		self->out_tail = buf;
	}
	return 0;
}

rl_transport_buf_t *
rl_transport_alloc_buffer(rl_transport_t *self)
{
	rl_transport_buf_t *result;

	if (self->num_free_buffers)
	{
		RL_ASSERT(self->next_free_buffer);

		--self->num_free_buffers;

		result = self->next_free_buffer;
		self->next_free_buffer = result->next;
		result->next = NULL;
		return result;
	}
	else
	{
		const size_t buffer_size = 8192;
		const size_t total_size = sizeof(rl_transport_buf_t) + buffer_size;

		result = (rl_transport_buf_t *) rl_alloc_sized_and_clear(total_size);

		if (!result)
			return NULL;

		result->total_size = total_size;
		result->buffer_size = buffer_size;
		return result;
	}
}

void
rl_transport_free_buffer(rl_transport_t *self, rl_transport_buf_t *buf)
{
	if (self->num_free_buffers >= RL_TRANSPORT_MAX_POOLED_BUFFERS)
	{
		rl_free_sized(buf, buf->total_size);
	}
	else
	{
		buf->next = self->next_free_buffer;
		self->next_free_buffer = buf;
		++self->num_free_buffers;
	}
}
