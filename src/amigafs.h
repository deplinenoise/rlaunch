#ifndef RLAUNCH_AMIGAFS_H
#define RLAUNCH_AMIGAFS_H

#ifndef __AMIGA__
#error "This is an amiga source file."
#endif

#include "util.h"
#include "rlnet.h"

struct peer_tag;
struct rl_pending_operation_tag;
struct rl_amigafs_tag;

/* dos and exec types */
struct MsgPort;
struct DosPacket;
struct DeviceList;

/* Limit the fun to 64 character path segments. This is per individual path
 * component. */
#define RL_FSCLIENT_MAX_PATH (64)

typedef enum rl_client_handle_type_tag
{
	RL_HANDLE_FILE,
	RL_HANDLE_DIR,
	RL_HANDLE_DEVICE
} rl_client_handle_type_t;

enum rl_client_handle_flags_tag
{
	RL_CLIENT_FLAG_FILE_ENUM_IN_PROGRESS = 1
};

typedef struct rl_client_handle_tag
{
	rl_uint32 handle_id;
	rl_client_handle_type_t type;
	rl_uint32 offset_lo;
	rl_uint32 offset_hi;
	char node_name[32];
	rl_uint32 size_lo;
	rl_uint32 flags;
	/* FIXME: Add size_hi, perhaps. */
	struct rl_client_handle_tag *next;
} rl_client_handle_t;

typedef struct rl_pending_read_tag
{
	char *destination;
	rl_uint32 bytes_read;
} rl_pending_read_t;

typedef struct rl_pending_write_tag
{
	const void *source;
	size_t length;
} rl_pending_write_t;

typedef struct rl_pending_stat_tag
{
	const void *source;
	size_t length;
} rl_pending_stat_t;

typedef void (*rl_completion_callback_fn_t)
	(struct rl_amigafs_tag *fs,
	 struct rl_pending_operation_tag *op,
	 const rl_msg_t *message);

typedef struct rl_pending_operation_tag
{
	/* The sequence number for the request associated with this operation. */
	rl_uint32 request_seqno;

	/* The next pending operation in the chain. */
	struct rl_pending_operation_tag *next;

	/* The original packet that started this operation. */
	struct DosPacket *input_packet;

	/* The type of answer we're expecting back. */
	rl_msg_kind_t expected_answer_type;

	/* Completion function to call when the answer is received. */
	rl_completion_callback_fn_t callback;

	union
	{
		rl_pending_stat_t stat;
		rl_pending_read_t read;
		rl_pending_write_t write;
	} detail;

} rl_pending_operation_t;

typedef struct rl_amigafs_tag
{
	/* The peer this file system belongs to. */
	struct peer_tag					*peer;

	/* The running sequence number attached to outgoing requests. */
	rl_uint32						seqno;

	/* The message port we're serving the file system on. */
	struct MsgPort					*device_port;

	/* The node in the DOS lists for this device. */
	struct DeviceList				*device_list;

	/* Pending operations on this device that are blocked waiting for answers
	 * to network messages. */
	rl_pending_operation_t			*pending;

	/* Our root handle for the device. */
	rl_client_handle_t				root_handle;
} rl_amigafs_t;

int rl_amigafs_init(rl_amigafs_t *self, struct peer_tag *peer, const char *device_name);

void rl_amigafs_destroy(rl_amigafs_t *self);

int rl_amigafs_process_device_message(rl_amigafs_t *self);

int rl_amigafs_process_network_message(rl_amigafs_t *self, const rl_msg_t *msg);

#endif
