#ifndef RLAUNCH_TRANSPORT_H
#define RLAUNCH_TRANSPORT_H

#include "util.h"
#include "socket_types.h"

typedef struct rl_iobuf_tag
{
	char *base_address;
	char *read_cursor;
	char *write_cursor;
	char *end_address;
	int valid;
} rl_iobuf_t;

typedef enum rl_message_delivery_result_tag
{
	RL_MESSAGE_INCOMPLETE,
	RL_MESSAGE_DELIVERED,
	RL_MESSAGE_GARBAGE
} rl_message_delivery_result_t;

struct rl_transport_tag;

typedef struct rl_transport_buf_tag
{
	/* total size of structure */
	size_t total_size;

	/* opaque userdata pointer (used by the peer) */
	void *userdata;

	/* buffer bytes used for the message when it was encoded or decoded */
	size_t used_size;

	/* bytes remaining to write */
	size_t remaining;

	/* next buffer, valid when parked in transport */
	struct rl_transport_buf_tag *next;

	/* size of the buffer space */
	size_t buffer_size;

	/* (variable) buffer--allocated immediately in the structure */
	rl_uint8 buffer[1];
} rl_transport_buf_t;

typedef struct rl_transport_callbacks_tag
{
	int (*peek_incoming)
		(struct rl_transport_tag		*transport,
		 const char						*buffer,
		 size_t							len);

	int (*deliver_incoming)
		(struct rl_transport_tag		*transport,
		 char							*buffer,
		 size_t							len);
} rl_transport_callbacks_t;

typedef struct rl_transport_tag
{
	const rl_transport_callbacks_t		*callbacks;
	rl_iobuf_t							inbuf;
	void								*userdata;
	rl_transport_buf_t					*out_queue;
	rl_transport_buf_t					*out_tail;

	int									num_free_buffers;
	rl_transport_buf_t					*next_free_buffer;

	int									error;
	int									disconnect;
} rl_transport_t;

int
rl_transport_init
	(rl_transport_t *t,
	 const rl_transport_callbacks_t *callbacks,
	 size_t buffer_size,
	 void *userdata);

void
rl_transport_destroy(rl_transport_t *t);

enum {
	RL_TRANSPORT_NEED_OUTPUT = 1 << 0,
	RL_TRANSPORT_DISCONNECTED = 1 << 14,
	RL_TRANSPORT_ERROR = 1 << 15
};

int
rl_transport_update(rl_transport_t *t);

void
rl_transport_on_input_arrived(rl_transport_t *t, rl_socket_t sock);

/*
 * Write as much as possible from the output queue to the specified socket.
 */
void
rl_transport_on_output_possible(rl_transport_t *t, rl_socket_t sock);

int
rl_transport_add_output_message(rl_transport_t *t, rl_transport_buf_t *buffer);

rl_transport_buf_t *
rl_transport_alloc_buffer(rl_transport_t *t);

void
rl_transport_free_buffer(rl_transport_t *t, rl_transport_buf_t *buf);

#endif
