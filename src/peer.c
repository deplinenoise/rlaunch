#include "util.h"
#include "peer.h"
#include "rlnet.h"
#include "socket_includes.h"
#include "version.h"

#include <stdio.h>

#ifdef WIN32
#include <windows.h>
#endif

#ifdef __AMIGA__
#include <exec/execbase.h>
extern struct ExecBase *SysBase;
#endif

#if defined(RL_POSIX)
#include <sys/utsname.h>
#endif

enum
{
	RL_PING_TIMEOUT = 30 /* seconds */
};

typedef enum peer_action_tag
{
	PEER_ACTION_RECEIVE_MESSAGE = 0,
	PEER_ACTION_TRANSMIT_MESSAGE,
	PEER_ACTION_RECEIVE_HANDSHAKE,
	PEER_ACTION_TRANSMIT_HANDSHAKE,
	PEER_ACTION_DISCONNECT,
	PEER_ACTION_UPDATE,
	PEER_ACTION_MAX
} peer_action_t;

typedef void (*peer_action_fn)(peer_t *self, const rl_msg_t *msg);

static int enqueue_output_message(peer_t *peer, const rl_msg_t *msg)
{
	rl_transport_buf_t *buf = NULL;

	if (NULL == (buf = rl_transport_alloc_buffer(&peer->transport)))
	{
		RL_LOG_WARNING(("enqueue %s failed: couldn't allocate buffer space", rl_msg_name(rl_msg_kind_of(msg))));
		goto err_cleanup;
	}

	if (0 != rl_encode_msg(msg, buf->buffer, (int) buf->buffer_size, &buf->used_size))
	{
		RL_LOG_WARNING(("enqueue %s failed: couldn't encode message", rl_msg_name(rl_msg_kind_of(msg))));
		goto err_cleanup;
	}

	if (RL_PACKET & rl_log_bits)
		rl_dump_buffer(buf->buffer, buf->used_size);

	buf->userdata = peer;

	if (0 != rl_transport_add_output_message(&peer->transport, buf))
	{
		RL_LOG_WARNING(("enqueue %s failed: transport didn't want more messages", rl_msg_name(rl_msg_kind_of(msg))));
		goto err_cleanup;
	}

	return 0;

err_cleanup:
	if (buf)
		rl_transport_free_buffer(&peer->transport, buf);
	return -1;
}

static void invoke_action(peer_t *peer, peer_action_t action, const rl_msg_t *arg);

static const char *peer_state_name(peer_state_t s)
{
	switch (s)
	{
	case PEER_INITIAL: return "initial";
	case PEER_WAIT_HANDSHAKE: return "wait-handshake";
	case PEER_CONNECTED: return "connected";
	case PEER_ERROR: return "error";
	case PEER_DISCONNECTED: return "disconnected";
	default: return "illegal";
	}
}

static const char *peer_action_name(peer_action_t a)
{
	switch (a)
	{
	case PEER_ACTION_RECEIVE_MESSAGE: return "receive_message";
	case PEER_ACTION_TRANSMIT_MESSAGE: return "transmit_message";
	case PEER_ACTION_RECEIVE_HANDSHAKE: return "receive_handshake";
	case PEER_ACTION_TRANSMIT_HANDSHAKE: return "transmit_handshake";
	case PEER_ACTION_DISCONNECT: return "disconnect";
	case PEER_ACTION_UPDATE: return "update";
	default: return "<unknown>";
	}
}

static void peer_set_state(peer_t* self, peer_state_t new_state)
{
	if (self->state != new_state)
	{
		RL_LOG_INFO(("%s[%s]: => %s", self->ident, peer_state_name(self->state), peer_state_name(new_state)));
		self->state = new_state;

		if (PEER_CONNECTED == new_state)
		{
			(*self->callbacks.on_connected)(self);
		}
	}
}

static void on_transmit_handshake(peer_t *self, const rl_msg_t *param);
static void on_receive_handshake(peer_t *self, const rl_msg_t *param);
static void on_transmit_message(peer_t *self, const rl_msg_t *param);
static void on_receive_message(peer_t *self, const rl_msg_t *param);
static void on_disconnect(peer_t *self, const rl_msg_t *param);

static void on_transmit_handshake(peer_t *self, const rl_msg_t *param_unused_)
{
	char platform_version[128];

#if defined(WIN32)
	char node_name[64];
	DWORD node_name_size;
	OSVERSIONINFOA version_info;
#elif defined(RL_POSIX)
	struct utsname uname_data;
#endif

	rl_msg_t msg;
	rl_msg_handshake_request_t *request;

	request = &msg.handshake_request;
	request->hdr_type = RL_MSG_HANDSHAKE_REQUEST;
	request->hdr_flags = 0;
	request->hdr_sequence_num = 0;
	request->version_major = RLAUNCH_VER_MAJOR;
	request->version_minor = RLAUNCH_VER_MINOR;

#ifdef __AMIGA__
	request->platform_name = "AmigaOS";
	request->node_name = "unknown";

	rl_format_msg(platform_version, sizeof(platform_version), "Kickstart V%d",
			(int) SysBase->LibNode.lib_Version);
	request->platform_version = platform_version;

#elif defined(WIN32)
	request->platform_name = "Microsoft Windows";

	node_name_size = sizeof(node_name);
	if (!GetComputerNameA(node_name, &node_name_size))
		rl_format_msg(node_name, sizeof(node_name), "unknown");

	request->node_name = node_name;

	rl_memset(&version_info, 0, sizeof(version_info));
	version_info.dwOSVersionInfoSize = sizeof(version_info);
	if (GetVersionExA(&version_info))
	{
		rl_format_msg(platform_version, sizeof(platform_version), "%d.%d.%d %s",
				(int) version_info.dwMajorVersion,
				(int) version_info.dwMinorVersion,
				(int) version_info.dwBuildNumber,
				version_info.szCSDVersion);
	}
	else
		rl_format_msg(platform_version, sizeof(platform_version), "unknown version");

	request->platform_version = platform_version;

#elif defined(RL_POSIX)
	if (0 == uname(&uname_data))
	{
		request->platform_name = uname_data.sysname;
		rl_format_msg(platform_version, sizeof(platform_version), "%s %s (%s)",
				uname_data.version, uname_data.release, uname_data.machine);
	}
	else
	{
		rl_format_msg(uname_data.nodename, sizeof(uname_data.nodename), "unknown");
		rl_format_msg(platform_version, sizeof(platform_version), "unknown");
	}

	request->node_name = uname_data.nodename;
	request->platform_version = platform_version;
#endif

	/* TODO: not used right now */
	request->password_hash = "****";

	if (0 != enqueue_output_message(self, &msg))
	{
		peer_set_state(self, PEER_ERROR);
	}
	else
	{
		peer_set_state(self, PEER_WAIT_HANDSHAKE);
	}
}

static void on_receive_handshake(peer_t *self, const rl_msg_t *param)
{
	RL_LOG_INFO(("%s: peer is %s running rlaunch v%d.%d on %s (%s)",
				self->ident,
				param->handshake_request.node_name,
				param->handshake_request.version_major,
				param->handshake_request.version_minor,
				param->handshake_request.platform_name,
				param->handshake_request.platform_version));

	/* Require exactly the same version */
	if (param->handshake_request.version_major == RLAUNCH_VER_MAJOR &&
		param->handshake_request.version_major == RLAUNCH_VER_MAJOR)
	{
		if (PEER_INIT_TARGET == self->init_mode)
		{
			invoke_action(self, PEER_ACTION_TRANSMIT_HANDSHAKE, NULL);
		}
		peer_set_state(self, PEER_CONNECTED);
	}
	else
	{
		RL_LOG_CONSOLE(("disconnection peer %s with unsupported version %d.%d (local version " RLAUNCH_VERSION ")",
						param->handshake_request.node_name,
						param->handshake_request.version_major,
						param->handshake_request.version_minor));
		peer_set_state(self, PEER_ERROR);
	}
}

static void on_transmit_message(peer_t *self, const rl_msg_t *param)
{
	if ((RL_NETWORK & rl_log_bits) && param)
	{
		char desc[256];
		rl_describe_msg(param, desc, sizeof(desc));
		RL_LOG_NETWORK(("%s: on_transmit_message: %s", self->ident, desc));
	}

	if (0 != enqueue_output_message(self, param))
	{
		peer_set_state(self, PEER_ERROR);
	}
}

static void on_receive_message(peer_t *self, const rl_msg_t *msg)
{
	self->last_activity = rl_time(NULL);

	if (RL_NETWORK & rl_log_bits)
	{
		char desc[256];
		rl_describe_msg(msg, desc, sizeof(desc));
		RL_LOG_NETWORK(("%s: on_receive_message: %s", self->ident, desc));
	}

	if (RL_MSG_PING_REQUEST == msg->ping_request.hdr_type)
	{
		rl_msg_t answer;
		answer.ping_answer.hdr_type = RL_MSG_PING_ANSWER;
		answer.ping_answer.hdr_flags = 0;
		answer.ping_answer.hdr_in_reply_to = msg->ping_request.hdr_sequence_num;
		invoke_action(self, PEER_ACTION_TRANSMIT_MESSAGE, &answer);
	}
	else if (RL_MSG_PING_ANSWER == msg->ping_answer.hdr_type)
	{
		self->ping_on_wire = 0;
	}
	else
	{
		if (0 != (*self->callbacks.on_message)(self, msg))
		{
			peer_set_state(self, PEER_ERROR);
		}
	}
}

static void on_disconnect(peer_t *self, const rl_msg_t *param)
{
	peer_set_state(self, PEER_DISCONNECTED);
}

static void action_nop(peer_t *self, const rl_msg_t *param)
{
}

static void on_update_connected(peer_t *self, const rl_msg_t *param)
{
	const time_t time_diff = rl_time(NULL) - self->last_activity;

	/* have the target wait a bit longer so that the pings don't overlap
	 * exactly in time, creating redundant traffic */
	const time_t threshold = (PEER_INIT_TARGET == self->init_mode ? RL_PING_TIMEOUT : (RL_PING_TIMEOUT + 1));

	if (time_diff > threshold && !self->ping_on_wire)
	{
		rl_msg_t ping;
		ping.ping_request.hdr_type = RL_MSG_PING_REQUEST;
		ping.ping_request.hdr_flags = 0;
		ping.ping_request.hdr_sequence_num = 0;
		invoke_action(self, PEER_ACTION_TRANSMIT_MESSAGE, &ping);
		self->ping_on_wire = 1;
	}
	else if (self->ping_on_wire && time_diff > (threshold << 1))
	{
		RL_LOG_WARNING(("%s[%s]: timeout on wire ping", self->ident, peer_state_name(self->state)));
		peer_set_state(self, PEER_ERROR);
	}
}

static const peer_action_fn state_actions[PEER_STATE_MAX][PEER_ACTION_MAX] =
{
	/* PEER_INITIAL */
	{ NULL, NULL, NULL, on_transmit_handshake, NULL, action_nop },
	/* PEER_WAIT_HANDSHAKE */
	{ NULL, NULL, on_receive_handshake, on_transmit_handshake, on_disconnect, action_nop },
	/* PEER_CONNECTED */
	{ on_receive_message, on_transmit_message, NULL, NULL, on_disconnect, on_update_connected },
	/* PEER_ERROR */
	{ NULL, NULL, NULL, NULL, on_disconnect, action_nop },
	/* PEER_DISCONNECTED */
	{ NULL, NULL, NULL, NULL, action_nop, action_nop }
};

static void invoke_action(peer_t *peer, peer_action_t action, const rl_msg_t *arg)
{
	peer_action_fn fn = state_actions[peer->state][action];

	/* don't spam about updates in the log */
	if (action != PEER_ACTION_UPDATE)
	{
		RL_LOG_DEBUG(("%s[%s]: <%s> (%s)",
					peer->ident,
					peer_state_name(peer->state),
					peer_action_name(action),
					(arg ? rl_msg_name(rl_msg_kind_of(arg)) : "N/A")));
	}

	if (fn)
	{
		(*fn)(peer, arg);
	}
	else
	{
		RL_LOG_WARNING(("%s[%s]: action %s not defined",
					peer->ident,
					peer_state_name(peer->state),
					peer_action_name(action)));
		peer_set_state(peer, PEER_ERROR);
	}
}

static int peer_peek_incoming(rl_transport_t *t, const char	*buf_, size_t len)
{
	const rl_uint8 *buf = (const rl_uint8 *) buf_;
	rl_uint16 size = 0;

	if (len < 4)
		return 4;

	size = (((rl_uint16)buf[2]) << 8) | (buf[3]);

	return (int) size;
}

static int peer_deliver_incoming(rl_transport_t *t, char *buf, size_t len)
{
	rl_msg_t msg;
	peer_t *peer;

	peer = (peer_t*) t->userdata;

	if (RL_PACKET & rl_log_bits)
		rl_dump_buffer(buf, len);

	if (0 != rl_decode_msg(buf, (int) len, &msg))
	{
		RL_LOG_WARNING(("%s: failed to decode incoming message", peer->ident));
		return 1;
	}

	if (RL_MSG_HANDSHAKE_REQUEST == rl_msg_kind_of(&msg))
		invoke_action(peer, PEER_ACTION_RECEIVE_HANDSHAKE, &msg);
	else
		invoke_action(peer, PEER_ACTION_RECEIVE_MESSAGE, &msg);
	return 0;
}

static const rl_transport_callbacks_t peer_transport_callbacks =
{
	peer_peek_incoming,
	peer_deliver_incoming
};

int peer_init(
		peer_t *self,
		rl_socket_t fd,
		const struct sockaddr* address,
		const peer_callbacks_t* cb,
		peer_init_mode_t init_mode,
		void* userdata)
{
	self->state = PEER_INITIAL;
	self->fd = fd;
	self->next = NULL;
	self->callbacks = *cb;
	self->userdata = userdata;
	self->update_result = 0;
	self->init_mode = init_mode;
	self->ping_on_wire = 0;
	self->last_activity = rl_time(NULL);

	RL_ASSERT(self->callbacks.on_message);
	RL_ASSERT(self->callbacks.on_connected);

	if (0 != rl_transport_init(&self->transport, &peer_transport_callbacks, 32768, self))
		return 1;

	if (AF_INET == address->sa_family)
	{
		char addr_buffer[64];
		const struct sockaddr_in* addr_in = (const struct sockaddr_in*) address;
#ifdef __AMIGA__
		rl_format_msg(addr_buffer, sizeof(addr_buffer),
				"%d.%d.%d.%d",
				(addr_in->sin_addr.s_addr >> 24) & 0xff,
				(addr_in->sin_addr.s_addr >> 16) & 0xff,
				(addr_in->sin_addr.s_addr >> 8) & 0xff,
				addr_in->sin_addr.s_addr & 0xff);
#else
		rl_format_msg(addr_buffer, sizeof(addr_buffer), "%s", inet_ntoa(addr_in->sin_addr));
#endif

		rl_format_msg(self->ident, sizeof(self->ident),
				"%s:%d", addr_buffer, (int) addr_in->sin_port);
	}
	else
	{
		rl_format_msg(self->ident, sizeof(self->ident), "<non-tcp/ip address>");
	}

	RL_LOG_DEBUG(("%s: init peer", self->ident));

	if (PEER_INIT_CONTROLLER == init_mode)
	{
		invoke_action(self, PEER_ACTION_TRANSMIT_HANDSHAKE, NULL);
	}
	else
	{
		peer_set_state(self, PEER_WAIT_HANDSHAKE);
	}
	return 0;
}

void peer_destroy(peer_t *self)
{
	RL_LOG_DEBUG(("%s: destroying", self->ident));
	CloseSocket(self->fd);
	rl_transport_destroy(&self->transport);
}

int peer_transmit_message(peer_t* self, const rl_msg_t *msg)
{
	invoke_action(self, PEER_ACTION_TRANSMIT_MESSAGE, msg);
	return 0;
}

int peer_update(peer_t *self, int can_read, int can_write)
{
	int transport_status;

	if (can_read)
		rl_transport_on_input_arrived(&self->transport, self->fd);

	if (can_write)
		rl_transport_on_output_possible(&self->transport, self->fd);

	transport_status = rl_transport_update(&self->transport);

	if (RL_TRANSPORT_ERROR & transport_status)
		invoke_action(self, PEER_ACTION_DISCONNECT, NULL);
	if (RL_TRANSPORT_DISCONNECTED & transport_status)
		invoke_action(self, PEER_ACTION_DISCONNECT, NULL);

	invoke_action(self, PEER_ACTION_UPDATE, NULL);

	/* Try to make some output progress if any of the actions have triggered a
	 * write */
	rl_transport_on_output_possible(&self->transport, self->fd);

	if (PEER_ERROR == self->state || PEER_DISCONNECTED == self->state)
		self->update_result = PEER_STATUS_REMOVE_ME;
	else if (RL_TRANSPORT_NEED_OUTPUT & transport_status)
		self->update_result = PEER_STATUS_NEED_OUTPUT;
	else
		self->update_result = 0;

	return self->update_result;
}

