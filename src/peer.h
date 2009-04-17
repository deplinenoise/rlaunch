#ifndef RLAUNCH_PEER_H
#define RLAUNCH_PEER_H

#include "util.h"
#include "protocol.h"
#include "transport.h"

struct sockaddr;
union rl_msg_tag;

#include <time.h>

/*
 * States and transitions:
 *
 * Controller entry sequence:
 * INITIAL -> SEND_HANDSHAKE [for controller]
 * INITIAL -> PEER_ERROR [can't send handshake]
 * SEND_HANDSHAKE -> WAIT_HANDSHAKE
 * WAIT_HANDSHAKE -> CONNECTED [handshake OK]
 * WAIT_HANDSHAKE -> ERROR [handshake not received or timeout]
 *
 * Target entry sequence
 * INITIAL -> WAIT_HANDSHAKE [for target]
 *
 * Common paths:
 * WAIT_HANDSHAKE -> CONNECTED [on OK handshake received]
 * WAIT_HANDSHAKE -> ERROR [on invalid handshake received]
 *
 * CONNECTED -> CONNECTED			[on successful transmit]
 * CONNECTED -> ERROR			[on unsuccessful transmit]
 *
 * CONNECTED -> CONNECTED			[on successful receive and delivery]
 * CONNECTED -> ERROR			[on unsuccessful receive and delivery]
 *
 * ERROR -> DISCONNECT [implicit]
 */
typedef enum peer_state_t
{
	PEER_INITIAL,			/* transient */
	PEER_WAIT_HANDSHAKE,
	PEER_CONNECTED,
	PEER_ERROR,				/* transient */
	PEER_DISCONNECTED,
	PEER_STATE_MAX
} peer_state_t;

struct peer_tag;

typedef struct peer_callbacks_tag
{
	int (*on_message)(struct peer_tag *peer, const union rl_msg_tag *msg);
	int (*on_connected)(struct peer_tag *peer);
} peer_callbacks_t;

typedef enum peer_init_mode_tag
{
	PEER_INIT_CONTROLLER,
	PEER_INIT_TARGET
} peer_init_mode_t;

typedef struct peer_tag
{
	/* instrusively stored pointer for external linked list support */
	struct peer_tag		*next;

	/* the state we're currently in */
	peer_state_t		state;

	/* socket fd associated with this peer */
	rl_socket_t			fd;

	/* human readable name for this peer */
	char				ident[32];

	/* peer index (used to index device names on the Amiga side) */
	int					peer_index;

	/* transport buffer that stores incomplete messages and queues
	 * output messages to be written
	 */
	rl_transport_t		transport;
	
	/* callbacks and their user data */
	void				*userdata;
	peer_callbacks_t	callbacks;

	/* result from update */
	int update_result;

	peer_init_mode_t	init_mode;

	time_t				last_activity;
	int					ping_on_wire;
} peer_t;

int peer_init(	peer_t *peer,
				rl_socket_t fd,
				const struct sockaddr* address,
				const peer_callbacks_t* callbacks,
				peer_init_mode_t init_mode,
				void* userdata);

void peer_destroy(peer_t *peer);

enum
{
	PEER_STATUS_NEED_OUTPUT			= 1 << 0,
	PEER_STATUS_REMOVE_ME			= 1 << 1
};

int peer_update(peer_t* peer, int can_read, int can_write);

int peer_transmit_message(peer_t* self, const union rl_msg_tag *msg);

#endif
