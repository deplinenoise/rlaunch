#ifndef __AMIGA__
#error "This is an Amiga source file"
#endif

#include "amigafs.h"
#include "util.h"
#include "rlnet.h"
#include "peer.h"
#include "protocol.h"

#define BSTR_LEN(str) (((const rl_uint8 *)str)[0])
#define BSTR_PTR(str) (((const char *)str)+1)

#define BCPL_CAST(ptr_type, expression) ((ptr_type *) (((LONG)expression) << 2))

#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dos.h>
#include <exec/execbase.h>

#define RL_AMIGA_PATH_MAX 108

#define HANDLE_FROM_LOCK(lock) ((rl_client_handle_t*) (lock)->fl_Key)

static LONG translate_error_code(rl_uint32 error_code);
static const char* get_packet_type_name(const struct DosPacket* packet);
static void construct_bstr(char *start, LONG max_size, const char *input);

static const char *rl_client_handle_type_name(rl_client_handle_type_t type)
{
	switch (type)
	{
		case RL_HANDLE_FILE: return "file";
		case RL_HANDLE_DIR: return "directory";
		case RL_HANDLE_DEVICE: return "device";
		default:
							   return "<bogus>";
	}
}

/*
 * Given an input path and an optional parent directory, compute the
 * corresponding absolute path on the server.
 */
static void normalize_object_path(
	rl_amigafs_t *fs,
	char *buffer,
	size_t buffer_size,
	struct FileLock *dir_lock,
	const void *object_name_bstr)
{
	char item_path[RL_AMIGA_PATH_MAX];
	char full_path[RL_AMIGA_PATH_MAX];
	rl_client_handle_t *handle;

	/*
	 * Establish a node to start "locating" from--if we don't have a lock,
	 * start from the root
	 */
	if (!dir_lock)
		handle = &fs->root_handle;
	else
		handle = HANDLE_FROM_LOCK(dir_lock);

	rl_memcpy(item_path, BSTR_PTR(object_name_bstr), BSTR_LEN(object_name_bstr));
	item_path[BSTR_LEN(object_name_bstr)] = '\0';

	/*
	 * If the name is absolute (starts with a device name followed by a
	 * colon, or a lone colon) we override the start location to be the root.
	 */
	{
		const char *colon_pos = rl_strchr(item_path, ':');
		if (colon_pos)
		{
			rl_memmove(item_path, colon_pos+1, rl_strlen(colon_pos+1)+1);
			handle = &fs->root_handle;
		}
	}

	if (&fs->root_handle != handle)
	{
		RL_LOG_DEBUG(("normalize: path '%s' relative to parent '%s'", item_path, handle->path));
		rl_format_msg(buffer, buffer_size, "%s/%s", handle->path, item_path);
	}
	else
	{
		rl_format_msg(buffer, buffer_size, "%s", item_path);
	}
}

static void dump_pending_ops(rl_amigafs_t *fs)
{
	int index = 0;
	rl_pending_operation_t *op;

	if (!(RL_DEBUG & rl_log_bits))
		return;

	rl_log_message("Pending ops against %s: ", fs->peer->ident);

	for (op = fs->pending; op; op = op->next, ++index)
	{
		rl_log_message("%d: [%u: %s] ", index, op->request_seqno, rl_msg_name(op->expected_answer_type));
	}
}

static rl_pending_operation_t *alloc_pending(rl_amigafs_t *self, struct DosPacket *packet, rl_msg_kind_t expected_answer_type, rl_completion_callback_fn_t callback)
{
	rl_pending_operation_t *op;

	op = (rl_pending_operation_t *)
		RL_ALLOC_TYPED_ZERO(rl_pending_operation_t);

	if (!op)
		return NULL;

	op->request_seqno = self->seqno++;
	op->next = self->pending;
	op->input_packet = packet;
	op->expected_answer_type = expected_answer_type;
	op->callback = callback;

	self->pending = op;

	dump_pending_ops(self);
	return op;
}

static void construct_bstr(char *start, LONG max_size, const char *input)
{
	size_t len = rl_strlen(input);

	/* Reserve space for the size byte, and the trailing null termination for
	 * certain DOS BSTRs */
	max_size -= 2;

	if (len > max_size)
		len = max_size;

	*start++ = (char) len;

	while(len--)
	{
		*start++ = *input++;
	}

	*start = '\0';
}

static void unlink_pending(rl_amigafs_t *self, rl_pending_operation_t *target)
{
	rl_pending_operation_t *op = self->pending, *previous = NULL;

	RL_LOG_DEBUG(("Unlinking pending operation %u (%s)", target->request_seqno, rl_msg_name(target->expected_answer_type)));

	while (op)
	{
		if (target == op)
		{
			if (previous)
				previous->next = target->next;
			else
				self->pending = target->next;
			break;
		}
		else
		{
			previous = op;
			op = op->next;
		}
	}

	RL_FREE_TYPED(rl_pending_operation_t, target);
	dump_pending_ops(self);
}


/*
 * Mount a volume with the specified device name and map all handler messages
 * to the specified port.
 */
static struct DeviceList *mount_volume(const char *name, struct MsgPort *port)
{
   struct DeviceList *volume;
   struct DosList *dlist;

   if(name == NULL || port == NULL) return NULL;

   while(NULL == (dlist = AttemptLockDosList(LDF_VOLUMES|LDF_WRITE)))
   {
	   /* Can't lock the DOS list.  Wait a second and try again. */
	   Delay(50);
   }

   volume = (struct DeviceList *) FindDosEntry(dlist, (char*) name, LDF_VOLUMES);

   UnLockDosList(LDF_VOLUMES|LDF_WRITE);

   if(volume || !(volume = (struct DeviceList *)MakeDosEntry((char*) name, DLT_VOLUME)))
   {
	   return NULL;
   }

   volume->dl_VolumeDate.ds_Days	= 0L;
   volume->dl_VolumeDate.ds_Minute	= 0L;
   volume->dl_VolumeDate.ds_Tick	= 0L;
   volume->dl_Lock					= 0L;
   volume->dl_Task					= port;
   volume->dl_DiskType				= ID_DOS_DISK;

   while(NULL == (dlist = AttemptLockDosList(LDF_VOLUMES|LDF_WRITE)))
   {
	   /* Oops, can't lock DOS list.  Wait 1 second and retry. */
	   Delay(50);
   }

   AddDosEntry((struct DosList *)volume);
   UnLockDosList(LDF_VOLUMES|LDF_WRITE);
   return volume;
}

static int unmount_volume(struct DeviceList *volume)
{
	/* no volume, or locked; can't unmount */
	if(volume == NULL || volume->dl_Lock != 0)
		return 1;

	RemDosEntry((struct DosList *)volume);
	FreeDosEntry((struct DosList *)volume);
	return 0;
}

static int
reply_to_packet(rl_amigafs_t *self, struct DosPacket *packet)
{
	struct MsgPort* reply_port;
	RL_LOG_DEBUG(("OUT: %s Res1=%08x Res2=%08x", get_packet_type_name(packet), packet->dp_Res1, packet->dp_Res2));
	reply_port = packet->dp_Port;
	packet->dp_Port = self->device_port;
	PutMsg(reply_port, packet->dp_Link);
	return 0;
}

struct FileLock* rl_amigafs_alloc_root_lock(rl_amigafs_t *self, long mode)
{
	struct FileLock *lock = NULL;

   	if (NULL == (lock = RL_ALLOC_TYPED_ZERO(struct FileLock)))
		goto error;

	RL_LOG_DEBUG(("Allocated lock %p for root", lock));

	lock->fl_Access = mode;
	lock->fl_Key = (LONG) &self->root_handle;
	lock->fl_Task = self->device_port;
	lock->fl_Volume = MKBADDR(self->device_list);
	return lock;

error:
	if (lock)
		RL_FREE_TYPED(struct FileLock, lock);

	return NULL;
}

static struct FileLock *allocate_lock(
		rl_amigafs_t *fs,
		rl_client_handle_type_t type,
		rl_uint32 handle_id,
		LONG access,
		const char *name,
		rl_uint32 size)
{
	struct FileLock *lock = NULL;
	rl_client_handle_t *handle = NULL;

   	if (NULL == (lock = RL_ALLOC_TYPED_ZERO(struct FileLock)))
		goto error;

   	if (NULL == (handle = RL_ALLOC_TYPED_ZERO(rl_client_handle_t)))
		goto error;

	handle->handle_id = handle_id;
	handle->type = type;
	handle->size_lo = size;
	rl_string_copy(sizeof(handle->path), handle->path, name);

	RL_LOG_DEBUG(("Allocated lock %p for handle id %d (%p) type %s", lock, handle_id, handle, rl_client_handle_type_name(type)));

	lock->fl_Access = access;
	lock->fl_Key = (LONG) handle;
	lock->fl_Task = fs->device_port;
	lock->fl_Volume = MKBADDR(fs->device_list);
	return lock;

error:
	if (lock)
		RL_FREE_TYPED(struct FileLock, lock);
	if (handle)
		RL_FREE_TYPED(rl_client_handle_t, lock);

	return NULL;
}

void rl_amigafs_free_lock(rl_amigafs_t *fs, struct FileLock *lock)
{
	rl_client_handle_t *handle;
	rl_msg_t msg;

	RL_ASSERT(lock);

	handle = HANDLE_FROM_LOCK(lock);

	RL_ASSERT(handle);

	/* Don't free the device handle (it lives inside the amigafs struct). */
	if (RL_HANDLE_DEVICE != handle->type)
	{
		/* Clean up the server-side handle. */
		RL_MSG_INIT(msg, RL_MSG_CLOSE_HANDLE_REQUEST);
		msg.close_handle_request.hdr_sequence_num = fs->seqno++;
		msg.close_handle_request.handle = handle->handle_id;
		RL_LOG_DEBUG(("transmitting close request for handle %d", handle->handle_id));
		if (0 != peer_transmit_message(fs->peer, &msg))
			RL_LOG_WARNING(("Couldn't transmit close handle request for id %d", handle->handle_id));
		RL_FREE_TYPED(rl_client_handle_t, handle);
	}

	RL_FREE_TYPED(struct FileLock, lock);
}

#define HANDLER_RANGE_1_FIRST (0)
#define HANDLER_RANGE_1_LAST (34)

enum handler_flags
{
	BP1	= 0x0001,
	BP2	= 0x0002,
	BP3	= 0x0004,
	BP4	= 0x0008
};

typedef void (*packet_handler_fn)(rl_amigafs_t *fs, struct DosPacket *packet);

typedef struct lookup_entry_t
{
	/* Pointer to a function that will handle this message, or NULL if it is
	 * unsupported. */
	packet_handler_fn function;

	/* Indicates what arguments are BCPL pointer and must be adjusted with a
	 * two-bit left shift--this is done once before invoking the handlers
	 * rather than being sprinkled in the handlers so that sanity can be
	 * preserved.
	 */
	int flags;
} lookup_entry_t;

/* Handler functions. */

static void action_die				(rl_amigafs_t *fs, struct DosPacket *packet);
static void action_current_volume	(rl_amigafs_t *fs, struct DosPacket *packet);
static void action_locate_object	(rl_amigafs_t *fs, struct DosPacket *packet);
static void action_free_lock		(rl_amigafs_t *fs, struct DosPacket *packet);
static void action_delete_object	(rl_amigafs_t *fs, struct DosPacket *packet);
static void action_copy_dir			(rl_amigafs_t *fs, struct DosPacket *packet);
static void action_examine_object	(rl_amigafs_t *fs, struct DosPacket *packet);
static void action_examine_next		(rl_amigafs_t *fs, struct DosPacket *packet);
static void action_disk_info		(rl_amigafs_t *fs, struct DosPacket *packet);
static void action_info				(rl_amigafs_t *fs, struct DosPacket *packet);
static void action_parent			(rl_amigafs_t *fs, struct DosPacket *packet);
static void action_inhibit			(rl_amigafs_t *fs, struct DosPacket *packet);

static void action_rename_disk		(rl_amigafs_t *fs, struct DosPacket *packet);
static void action_rename_object	(rl_amigafs_t *fs, struct DosPacket *packet);
static void action_set_protect		(rl_amigafs_t *fs, struct DosPacket *packet);
static void action_create_dir		(rl_amigafs_t *fs, struct DosPacket *packet);
static void action_flush			(rl_amigafs_t *fs, struct DosPacket *packet); 
static void action_set_comment		(rl_amigafs_t *fs, struct DosPacket *packet);
static void action_set_file_date	(rl_amigafs_t *fs, struct DosPacket *packet);

static void action_findinput		(rl_amigafs_t *fs, struct DosPacket *packet);

static const lookup_entry_t packet_handlers_range_1[(HANDLER_RANGE_1_LAST - HANDLER_RANGE_1_FIRST) + 1] =
{
   { NULL,					0  | 0	| 0  | 0   }, /*  0 - ACTION_NIL		   */
   { NULL,					0  | 0	| 0  | 0   }, /*  1 - Unknown			   */
   { NULL,					BP1| BP2| BP3| 0   }, /*  2 - ACTION_GET_BLOCK	   */
   { NULL,					0  | BP2| BP3| 0   }, /*  3 - Unknown			   */
   { NULL,					BP1| BP2| BP3| 0   }, /*  4 - ACTION_SET_MAP	   */
   { action_die,			0  | 0	| 0  | 0   }, /*  5 - ACTION_DIE		   */
   { NULL,					0  | 0	| 0  | 0   }, /*  6 - ACTION_EVENT		   */
   { action_current_volume,	BP1| 0	| 0  | 0   }, /*  7 - ACTION_CURRENT_VOLUME*/
   { action_locate_object,	BP1| BP2| 0  | 0   }, /*  8 - ACTION_LOCATE_OBJECT */
   { action_rename_disk,	BP1| BP2| 0  | 0   }, /*  9 - ACTION_RENAME_DISK   */
   { NULL,					0  | 0	| 0  | 0   }, /* 10 - Unknown			   */
   { NULL,					0  | 0	| 0  | 0   }, /* 11 - Unknown			   */
   { NULL,					0  | 0	| 0  | 0   }, /* 12 - Unknown			   */
   { NULL,					0  | 0	| 0  | 0   }, /* 13 - Unknown			   */
   { NULL,					0  | 0	| 0  | 0   }, /* 14 - Unknown			   */
   { action_free_lock,		BP1| 0	| 0  | 0   }, /* 15 - ACTION_FREE_LOCK	   */
   { action_delete_object,	BP1| BP2| 0  | 0   }, /* 16 - ACTION_DELETE_OBJECT */
   { action_rename_object,	BP1| BP2| BP3| BP4 }, /* 17 - ACTION_RENAME_OBJECT */
   { NULL,					0  | 0	| 0  | 0   }, /* 18 - ACTION_MORE_CACHE    */
   { action_copy_dir,		BP1| 0	| 0  | 0   }, /* 19 - ACTION_COPY_DIR	   */
   { NULL,					0  | 0	| 0  | 0   }, /* 20 - ACTION_WAIT_CHAR	   */
   { action_set_protect,	0  | BP2| BP3| 0   }, /* 21 - ACTION_SET_PROTECT   */
   { action_create_dir,		BP1| BP2| 0  | 0   }, /* 22 - ACTION_CREATE_DIR    */
   { action_examine_object,	BP1| BP2| 0  | 0   }, /* 23 - ACTION_EXAMINE_OBJECT*/
   { action_examine_next,	BP1| BP2| 0  | 0   }, /* 24 - ACTION_EXAMINE_NEXT  */
   { action_disk_info,		BP1| 0	| 0  | 0   }, /* 25 - ACTION_DISK_INFO	   */
   { action_info,			BP1| BP2| 0  | 0   }, /* 26 - ACTION_INFO		   */
   { action_flush,			0  | 0	| 0  | 0   }, /* 27 - ACTION_FLUSH		   */
   { action_set_comment,	0  | BP2| BP3| BP4 }, /* 28 - ACTION_SET_COMMENT   */
   { action_parent,			BP1| 0	| 0  | 0   }, /* 29 - ACTION_PARENT		   */
   { NULL,					BP1| 0	| 0  | 0   }, /* 30 - ACTION_TIMER		   */
   { action_inhibit,		0  | 0	| 0  | 0   }, /* 31 - ACTION_INHIBIT	   */
   { NULL,					BP1| 0	| 0  | 0   }, /* 32 - ACTION_DISK_TYPE	   */
   { NULL,					0  | 0	| 0  | 0   }, /* 33 - ACTION_DISK_CHANGE   */
   { action_set_file_date,	0  | 0	| 0  | 0   }  /* 34 - ACTION_SET_FILE_DATE */
};

static void action_is_filesystem(rl_amigafs_t *fs, struct DosPacket* packet)
{
	RL_LOG_DEBUG(("action_is_filesystem"));
	packet->dp_Res1 = DOSTRUE;
	packet->dp_Res2 = 0;
	reply_to_packet(fs, packet);
}


/*
 *	ACTION_FINDINPUT	Open(..., MODE_OLDFILE)
 *
 *	ARG1:	BPTR -	FileHandle to fill in
 *	ARG2:	LOCK -	Lock to directory that ARG3 is relative to
 *	ARG3:	BSTR -	Name of file to be opened (relative to ARG2)
 *
 *	RES1:	BOOL -	Success/Failure (DOSTRUE/DOSFALSE)
 *	RES2:	CODE -	Failure code if RES1 = DOSFALSE
 */
static void complete_findinput(rl_amigafs_t *fs, rl_pending_operation_t *op, const rl_msg_t *msg);

static void action_findinput(rl_amigafs_t *fs, struct DosPacket *packet)
{
	struct FileLock * const dir_lock =
		BCPL_CAST(struct FileLock, packet->dp_Arg2);

	rl_client_handle_t * const dir_handle = HANDLE_FROM_LOCK(dir_lock);

	const void *filename_bstr = BCPL_CAST(const void, packet->dp_Arg3);
	const char *filename_cstr = BSTR_PTR(filename_bstr);

	rl_pending_operation_t *pending_op = NULL;
	LONG error_code = 0;
	rl_msg_t msg;

    RL_LOG_DEBUG(("FINDINPUT: directory=\"%d\", name=\"%Q\"",
				dir_handle->handle_id, packet->dp_Arg3));

	/* Construct a pending open for the file. */
	pending_op = alloc_pending(fs, packet, RL_MSG_OPEN_HANDLE_ANSWER, complete_findinput);
	if (!pending_op)
	{
		error_code = ERROR_NO_FREE_STORE;
		goto error;
	}

	/* Skip leading DEVICE: header that is sometimes present */
	{
		const char* colon;
		if (NULL != (colon = rl_strchr(filename_cstr, ':')))
		{
			filename_cstr = colon+1;
			RL_LOG_DEBUG(("FINDINPUT: dropping device prefix"));
		}
	}

	RL_MSG_INIT(msg, RL_MSG_OPEN_HANDLE_REQUEST);
	msg.open_handle_request.hdr_sequence_num	= pending_op->request_seqno;
	msg.open_handle_request.path				= filename_cstr; /* FIXME: Are they always null-terminated? */
	msg.open_handle_request.mode				= RL_OPENFLAG_READ;
	if (0 != peer_transmit_message(fs->peer, &msg))
		goto error;

	return;

error:
	if (pending_op)
		unlink_pending(fs, pending_op);

	packet->dp_Res1 = DOSFALSE;
	packet->dp_Res2 = error_code;
	reply_to_packet(fs, packet);
}

static void complete_findinput(rl_amigafs_t *fs, rl_pending_operation_t *op, const rl_msg_t *msg)
{
	struct DosPacket * const packet = op->input_packet;
	struct FileHandle * const fh = BCPL_CAST(struct FileHandle, op->input_packet->dp_Arg1);
	const void *filename_bstr = BCPL_CAST(const void, packet->dp_Arg3);

	/* Make sure the client is getting a lock on a file. */
	if (RL_NODE_TYPE_FILE != msg->open_handle_answer.type)
	{
		packet->dp_Res1 = DOSFALSE;
		packet->dp_Res2 = ERROR_OBJECT_WRONG_TYPE;
	}
	else
	{
		struct FileLock *file_lock;

		file_lock = allocate_lock(fs,
				RL_HANDLE_FILE,
				msg->open_handle_answer.handle,
				SHARED_LOCK,
				BSTR_PTR(filename_bstr),
				msg->open_handle_answer.size);

		if (!file_lock)
		{
			packet->dp_Res1 = DOSFALSE;
			packet->dp_Res2 = ERROR_NO_FREE_STORE;
		}
		else
		{
			fh->fh_Type = fs->device_port;
			fh->fh_Arg1 = (LONG) file_lock;
		}
	}

	/* If we failed, clean up the server-side handle. */
	if (DOSFALSE == packet->dp_Res1)
	{
		rl_msg_t close_msg;
		RL_MSG_INIT(close_msg, RL_MSG_CLOSE_HANDLE_REQUEST);
		close_msg.close_handle_request.hdr_sequence_num = fs->seqno++;
		close_msg.close_handle_request.handle = msg->open_handle_answer.handle;
		peer_transmit_message(fs->peer, &close_msg);
	}

	reply_to_packet(fs, packet);
	unlink_pending(fs, op);
}

/*
 *	ACTION_EXAMINE_OBJECT	Examine(...)
 *
 *	ARG1:	LOCK -	Lock on object to examine
 *	ARG2:	BPTR -	FileInfoBlock to fill in
 *
 *	RES1:	BOOL -	Success/Failure (DOSTRUE/DOSFALSE)
 *	RES2:	CODE -	Failure code if RES1 = DOSFALSE
 */
static void action_examine_object(rl_amigafs_t *fs, struct DosPacket *packet)
{
	struct FileLock *lock = BCPL_CAST(struct FileLock, packet->dp_Arg1);
	struct FileInfoBlock *fib = BCPL_CAST(struct FileInfoBlock, packet->dp_Arg2);
	rl_client_handle_t *handle = NULL;

	RL_LOG_DEBUG(("EXAMINE_OBJECT Lock=%p fib=%p", lock, fib));

	if (lock)
		handle = HANDLE_FROM_LOCK(lock);

	if (!handle)
		handle = &fs->root_handle;

	if (&fs->root_handle == handle)
	{
		rl_memset(fib, 0, sizeof(*fib));
		fib->fib_DiskKey = 0L;
		fib->fib_EntryType = fib->fib_DirEntryType = ST_ROOT;
		rl_memcpy(fib->fib_FileName, BADDR(fs->device_list->dl_Name), BSTR_LEN(BADDR(fs->device_list->dl_Name))+1);
		fib->fib_Protection = 0;
		fib->fib_Size = 0;
		fib->fib_NumBlocks = 0;
		fib->fib_Date = fs->device_list->dl_VolumeDate;
		construct_bstr(fib->fib_Comment, sizeof(fib->fib_Comment), "This is a remote launch device");
		packet->dp_Res1 = DOSTRUE;
		packet->dp_Res2 = 0;
	}
	else
	{
		rl_memset(fib, 0, sizeof(*fib));
		fib->fib_DiskKey = 0L;
		fib->fib_DirEntryType = RL_HANDLE_FILE == handle->type ? -1 : 1;
		construct_bstr(fib->fib_FileName, sizeof(fib->fib_FileName), handle->path);
		/* Set protection bits for regular files. These set bits in the
		 * protection mask indicate forbidden actions, not caps. Really weird.
		 * */
		fib->fib_Protection = FIBF_WRITE | FIBF_DELETE /* simulate R/O FS */;
		fib->fib_Size = handle->size_lo;
		fib->fib_NumBlocks = handle->size_lo;
		fib->fib_Comment[0] = '\0';
		packet->dp_Res1 = DOSTRUE;
		packet->dp_Res2 = 0;
	}

	reply_to_packet(fs, packet);
}

/*
 *	ACTION_EXAMINE_NEXT	ExNext(...)
 *
 *	ARG1:	LOCK -	Lock on directory being examined
 *	ARG2:	BPTR -	FileInfoBlock to fill in
 *
 *	RES1:	Success/Failure (DOSTRUE/DOSFALSE)
 *	RES2:	Failure code if RES1 = DOSFALSE
 */
static void complete_examine_next(rl_amigafs_t *fs, rl_pending_operation_t *op, const rl_msg_t *msg);
static void action_examine_next(rl_amigafs_t *fs, struct DosPacket *packet)
{
	struct FileLock *lock = BCPL_CAST(struct FileLock, packet->dp_Arg1);
	rl_client_handle_t *handle = HANDLE_FROM_LOCK(lock);
	rl_msg_t msg;
	rl_pending_operation_t *pending_op = NULL;
	LONG error_code;

	pending_op = alloc_pending(fs, packet, RL_MSG_FIND_NEXT_FILE_ANSWER, complete_examine_next);
	if (!pending_op)
	{
		error_code = ERROR_NO_FREE_STORE;
		goto error;
	}

	RL_MSG_INIT(msg, RL_MSG_FIND_NEXT_FILE_REQUEST);
	msg.find_next_file_request.hdr_sequence_num	= pending_op->request_seqno;
	msg.find_next_file_request.handle = HANDLE_FROM_LOCK(lock)->handle_id;
	msg.find_next_file_request.reset =
		(handle->flags & RL_CLIENT_FLAG_FILE_ENUM_IN_PROGRESS) ? 0 : 1;

	if (0 != peer_transmit_message(fs->peer, &msg))
	{
		error_code = ERROR_DEVICE_NOT_MOUNTED;
		goto error;
	}

	handle->flags |= RL_CLIENT_FLAG_FILE_ENUM_IN_PROGRESS;
	return;

error:
	if (pending_op)
		unlink_pending(fs, pending_op);

	packet->dp_Res1 = DOSFALSE;
	packet->dp_Res2 = error_code;
	reply_to_packet(fs, packet);
}

static void complete_examine_next(rl_amigafs_t *fs, rl_pending_operation_t *op, const rl_msg_t *msg)
{
	struct DosPacket * const packet = op->input_packet;
	struct FileLock * const lock = BCPL_CAST(struct FileLock, packet->dp_Arg1);
	struct FileInfoBlock * const fib = BCPL_CAST(struct FileInfoBlock, packet->dp_Arg2);
	rl_client_handle_t *handle = HANDLE_FROM_LOCK(lock);
	const rl_msg_find_next_file_answer_t * const answer = &msg->find_next_file_answer;
	
	if (answer->end_of_sequence)
	{
		handle->flags &= ~(RL_CLIENT_FLAG_FILE_ENUM_IN_PROGRESS);
		packet->dp_Res1 = DOSFALSE;
		packet->dp_Res2 = ERROR_NO_MORE_ENTRIES;
	}
	else
	{
		rl_memset(fib, 0, sizeof(*fib));
		fib->fib_DiskKey = 0L;
		fib->fib_DirEntryType = RL_NODE_TYPE_DIRECTORY == answer->type ? -1 : 1;
		fib->fib_EntryType = fib->fib_EntryType; /* FIXME: Is this right? */
		construct_bstr(fib->fib_FileName, sizeof(fib->fib_FileName), answer->name);
		/* Set protection bits for regular files. These set bits in the
		 * protection mask indicate forbidden actions, not caps. Really weird.
		 * */
		fib->fib_Protection = FIBF_WRITE | FIBF_DELETE /* simulate R/O FS */;
		fib->fib_Size = answer->size;
		fib->fib_NumBlocks = answer->size;
		fib->fib_Comment[0] = '\0';
		packet->dp_Res1 = DOSTRUE;
		packet->dp_Res2 = 0;
	}

	reply_to_packet(fs, packet);
	unlink_pending(fs, op);
}

/* Helper function to populate a InfoData struct from the specified fs. */
static void fill_in_infodata(rl_amigafs_t *fs, struct InfoData *info)
{
	RL_LOG_DEBUG(("Populating infodata @ %p - %p", info, ((LONG)info) + sizeof(*info)));

	info->id_NumSoftErrors = 0;
	info->id_UnitNumber = 1;
	info->id_DiskState = ID_VALIDATED;
	info->id_NumBlocks = 1000;
	info->id_NumBlocksUsed = 500;
	info->id_BytesPerBlock = 1;
	info->id_DiskType = ID_FFS_DISK;
	info->id_VolumeNode = MKBADDR(fs->device_list);
	info->id_InUse = 0;

	RL_LOG_DEBUG(("id_VolumeNode as BPTR = %08x", info->id_VolumeNode));
}

/*	ACTION_DISK_INFO	Info(...)
 *	ARG1:	BPTR -	Pointer to an InfoData structure to fill in
 *	RES1:	BOOL -	Success/Failure (DOSTRUE/DOSFALSE)
 */
static void action_disk_info(rl_amigafs_t *fs, struct DosPacket* packet)
{
	RL_LOG_DEBUG(("ACTION_DISK_INFO"));
	fill_in_infodata(fs, BCPL_CAST(struct InfoData, packet->dp_Arg1));
	packet->dp_Res1 = DOSTRUE;
	packet->dp_Res2 = 0;
	reply_to_packet(fs, packet);
}

/*
 *	ACTION_INFO	<sendpkt only>
 *
 *	ARG1:	LOCK -	Lock on volume
 *	ARG2:	BPTR -	Pointer to an InfoData structure to fill in
 *
 *	RES1:	BOOL -	Success/Failure (DOSTRUE/DOSFALSE)
 */
static void action_info(rl_amigafs_t *fs, struct DosPacket *packet)
{
	struct FileLock *lock = BCPL_CAST(struct FileLock, packet->dp_Arg1);

	RL_LOG_DEBUG(("ACTION_INFO %p lock_id=%d", lock, lock ? HANDLE_FROM_LOCK(lock)->handle_id : -1));

	if (NULL == lock || lock->fl_Volume != MKBADDR(fs->device_list))
	{
		RL_LOG_DEBUG(("--> failed, invalid lock"));
		packet->dp_Res1 = DOSFALSE;
		packet->dp_Res2 = ERROR_INVALID_LOCK;
	}
	else if (HANDLE_FROM_LOCK(lock) == &fs->root_handle)
	{
		fill_in_infodata(fs, BCPL_CAST(struct InfoData, packet->dp_Arg2));
		packet->dp_Res1 = DOSTRUE;
		packet->dp_Res2 = 0;
	}
	else
	{
		packet->dp_Res1 = DOSFALSE;
		packet->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
	}

	reply_to_packet(fs, packet);
}

static void action_flush(rl_amigafs_t *fs, struct DosPacket *packet)
{
	/* sure thing */
	packet->dp_Res1 = DOSTRUE;
	packet->dp_Res2 = 0;
	reply_to_packet(fs, packet);
}

/*
 *	ACTION_LOCATE_OBJECT	Lock(...)
 *
 *	ARG1:	LOCK -	Lock on directory to which ARG2 is relative
 *	ARG2:	BSTR -	Name (possibly with a path) of object to lock
 *	ARG3:	LONG -	Mode: ACCESS_READ/SHARED_LOCK or
 *			ACCESS_WRITE/EXCLUSIVE_LOCK
 *
 *	RES1:	LOCK -	Lock on requested object or 0 to indicate failure
 *	RES2:	CODE -	Failure code if RES1 = 0
 */
static void complete_locate_object(rl_amigafs_t *fs, rl_pending_operation_t *op, const rl_msg_t *msg);

static void action_locate_object(rl_amigafs_t *fs, struct DosPacket* packet)
{
	LONG error_code = ERROR_OBJECT_NOT_FOUND;
	struct FileLock *dir_lock = BCPL_CAST(struct FileLock, packet->dp_Arg1);
	const void* object_name_bstr = BCPL_CAST(const void, packet->dp_Arg2);
	const LONG mode = packet->dp_Arg3;
	struct FileLock *result_lock = NULL;
	char full_path[RL_AMIGA_PATH_MAX];
	rl_client_handle_t *handle = NULL;
	rl_pending_operation_t *pending_op;
	rl_msg_t msg;

    RL_LOG_DEBUG(("LOCATE_OBJECT: directory=\"%d\", name=\"%Q\" mode=%d (%s)",
				dir_lock ? HANDLE_FROM_LOCK(dir_lock)->handle_id : -1,
				packet->dp_Arg2,
				(int) mode,
				mode == -1 ? "SHARED_LOCK/ACCESS_READ" : "EXCLUSIVE_LOCK/ACCESS_WRITE"));

	/* Clean up and normalize the path string. */
	normalize_object_path(fs, full_path, sizeof(full_path), dir_lock, object_name_bstr);
	RL_LOG_DEBUG(("Normalized lookup path: '%s'", full_path));

	/* If the client really wanted the root node, we can return that immediately. */
	if (0 == rl_strlen(full_path))
	{
		result_lock = rl_amigafs_alloc_root_lock(fs, mode);
		if (!result_lock)
			goto error;
		RL_LOG_DEBUG(("Returning lock: %p for handle id %d", result_lock, HANDLE_FROM_LOCK(result_lock)->handle_id));
		packet->dp_Res1 = MKBADDR(result_lock);
		packet->dp_Res2 = 0;
		reply_to_packet(fs, packet);
		return;
	}
	
	/* Construct a pending handle open request for the object */
	pending_op = alloc_pending(fs, packet, RL_MSG_OPEN_HANDLE_ANSWER, complete_locate_object);
	if (!pending_op)
	{
		error_code = ERROR_NO_FREE_STORE;
		goto error;
	}

	RL_MSG_INIT(msg, RL_MSG_OPEN_HANDLE_REQUEST);
	msg.open_handle_request.hdr_sequence_num	= pending_op->request_seqno;
	msg.open_handle_request.path				= &full_path[0];
	if (0 != peer_transmit_message(fs->peer, &msg))
	{
		error_code = ERROR_NOT_A_DOS_DISK;
		goto error;
	}

	return;

error:
	if (pending_op)
		unlink_pending(fs, pending_op);

	packet->dp_Res1 = DOSFALSE;
	packet->dp_Res2 = error_code;
	reply_to_packet(fs, packet);
}

static void complete_locate_object(rl_amigafs_t *fs, rl_pending_operation_t *op, const rl_msg_t *msg)
{
	struct FileLock *lock = NULL;
	struct DosPacket * const packet = op->input_packet;
	struct FileLock *dir_lock = BCPL_CAST(struct FileLock, packet->dp_Arg1);
	const void* object_name_bstr = BCPL_CAST(const void, packet->dp_Arg2);
	char full_path[RL_AMIGA_PATH_MAX];
	const rl_client_handle_type_t type =
		RL_NODE_TYPE_DIRECTORY == msg->open_handle_answer.type ? RL_HANDLE_DIR : RL_HANDLE_FILE;

	normalize_object_path(fs, full_path, sizeof(full_path), dir_lock, object_name_bstr);

	/* FIXME: The input path here is bogus. */
	if (NULL != (lock = allocate_lock(fs, type, msg->open_handle_answer.handle, 0, full_path, msg->open_handle_answer.size)))
	{
		op->input_packet->dp_Res1 = MKBADDR(lock);
		op->input_packet->dp_Res2 = 0;
	}
	else
	{
		op->input_packet->dp_Res1 = 0;
		op->input_packet->dp_Res2 = ERROR_NO_FREE_STORE;
	}

	reply_to_packet(fs, op->input_packet);
	unlink_pending(fs, op);
}


/*
 *	ACTION_FREE_LOCK	UnLock(...)
 *	ARG1:	LOCK -	Lock to free
 *	RES1:	BOOL -	DOSTRUE
 */
static void action_free_lock(rl_amigafs_t *fs, struct DosPacket* packet)
{
	struct FileLock *lock = BCPL_CAST(struct FileLock, packet->dp_Arg1);

	if (lock)
	{
		RL_LOG_DEBUG(("ACTION_FREE_LOCK: lock: %p (handle: %d) Node=%x", lock, HANDLE_FROM_LOCK(lock)->handle_id, (int)lock->fl_Key));
		rl_amigafs_free_lock(fs, lock);
		packet->dp_Res1 = DOSTRUE;
		packet->dp_Res2 = 0;
	}
	else
	{
		RL_LOG_DEBUG(("ACTION_FREE_LOCK w/ null lock?!"));
		packet->dp_Res1 = DOSFALSE;
		packet->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
	}
	reply_to_packet(fs, packet);
}

/*
 *	ACTION_END	Close()
 *
 *	ARG1:	ARG1 -	Lock (fh_Arg1 field of the file handle we're closing)
 *
 *	RES1:	BOOL -	Success/Failure (DOSTRUE/DOSFALSE)
 *	RES2:	CODE -	Failure code if RES1 = DOSFALSE
 */ 

static void action_end(rl_amigafs_t *fs, struct DosPacket *packet)
{
	struct FileLock * const lock = (struct FileLock *) packet->dp_Arg1;

	/* TODO: Should make the free be sync w/ the server so freeing has a bottom
	 * half continuation with the close_handle_answer. Otherwise we might run
	 * out of handles if opening/closing quicker on the client than on the
	 * server. */
	rl_amigafs_free_lock(fs, lock);

	packet->dp_Res1 = DOSTRUE;
	packet->dp_Res2 = 0;
	reply_to_packet(fs, packet);
}

/*
 *	ACTION_SET_PROTECT
 *
 *	ARG1:	ARG1 -	Unused
 *	ARG2:	LOCK -	Lock to which ARG3 is relative to
 *	ARG3:	BSTR -	bstring of the object name
 *	ARG4:   LONG -  Protection 32-bits
 *
 *	RES1:	BOOL -	DOSTRUE/DOSFALSE
 *	RES2:	CODE -	Failure code if RES1 = DOSFALSE
 */
static void action_set_protect(rl_amigafs_t *fs, struct DosPacket *packet)
{
	RL_LOG_DEBUG(("SET_PROTECT Lock=%p Name=\"%Q\" Bits=%d",
				packet->dp_Arg2,
				packet->dp_Arg3,
				(int) packet->dp_Arg4));
	packet->dp_Res1 = DOSFALSE;
	packet->dp_Res2 = ERROR_WRITE_PROTECTED;
	reply_to_packet(fs, packet);
}

/*
 *	ACTION_COPY_DIR		DupLock(...)
 *
 *	ARG1:	LOCK -	Lock to duplicate
 *
 *	RES1:	LOCK -	Duplicated lock or 0 to indicate failure
 *	RES2:	CODE -	Failure code if RES1 = 0
 */
static void action_copy_dir(rl_amigafs_t *fs, struct DosPacket *packet)
{
	struct FileLock *lock = BCPL_CAST(struct FileLock, packet->dp_Arg1);
	if (lock)
	{
		struct FileLock *copy;
		rl_client_handle_t *handle = HANDLE_FROM_LOCK(lock);

		if (RL_HANDLE_DEVICE == handle->type)
			copy = rl_amigafs_alloc_root_lock(fs, SHARED_LOCK);
		else
			copy = allocate_lock(fs, handle->type, handle->handle_id, SHARED_LOCK, handle->path, handle->size_lo);

		packet->dp_Res1 = MKBADDR(copy);

		if (!packet->dp_Res1)
		{
			packet->dp_Res2 = ERROR_NO_FREE_STORE;
		}
		else
		{
			packet->dp_Res2 = 0;
			RL_LOG_DEBUG(("Resulting lock is %p", copy));
		}
	}
	else
	{
		RL_LOG_WARNING(("ACTION_COPY_DIR with null lock?!"));
		packet->dp_Res1 = 0;
		packet->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
	}
	reply_to_packet(fs, packet);
}

/*
 *	ACTION_PARENT	Parent(...)
 *
 *	ARG1:	LOCK -	Lock on object to get the parent of
 *
 *	RES1:	LOCK -	Parent lock
 *	RES2:	Failure code if RES1 = 0
 */
static void action_parent(rl_amigafs_t *fs, struct DosPacket *packet)
{
	struct FileLock *lock = BCPL_CAST(struct FileLock, packet->dp_Arg1);
	struct FileLock *result_lock;
	rl_client_handle_t *handle = HANDLE_FROM_LOCK(lock);

	RL_LOG_DEBUG(("ACTION_PARENT for lock %p (%d)", lock, HANDLE_FROM_LOCK(lock)->handle_id));

	if (handle == &fs->root_handle || NULL == handle)
	{
		RL_LOG_DEBUG(("[The root handle (or null handle) doesn't have a parent]"));
		packet->dp_Res1 = DOSFALSE;
		packet->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
	}
	else
	{
		/* Take the parent handle's path and drop the last path component. */
		int len;
		char parent_path[108];

		rl_string_copy(sizeof(parent_path), parent_path, handle->path);
		len = (int) rl_strlen(parent_path);
		while (--len >= 0)
		{
			if (parent_path[len] == '/')
			{
				parent_path[len] = '\0';
				break;
			}
		}

		/* No slashes? Assume it's a file in the root directory and return the root. */
		if (len <= 0)
		{
			RL_LOG_DEBUG(("Returning root lock as parent of %s", handle->path));
			result_lock = rl_amigafs_alloc_root_lock(fs, SHARED_LOCK);
		}
		else
		{
			RL_LOG_DEBUG(("parent handle from '%s' to '%s'", handle->path, parent_path));
			result_lock = allocate_lock(fs, RL_HANDLE_DIR, ~0u, SHARED_LOCK, parent_path, 0);
		}

		if (result_lock)
		{
			packet->dp_Res1 = MKBADDR(result_lock);
			packet->dp_Res2 = 0;
		}
		else
		{
			packet->dp_Res1 = DOSFALSE;
			packet->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
		}
	}

	reply_to_packet(fs, packet);
}

/*
 *	ACTION_READ	Read(...)
 *
 *	ARG1:	ARG1 -	fh_Arg1 field of opened FileHandle
 *	ARG2:	APTR -	Buffer to put data into
 *	ARG3:	LONG -	Number of bytes to read
 *
 *	RES1:	LONG -	Number of bytes read. 0 indicates EOF. -1 indicates ERROR
 *	RES2:	CODE -	Failure code if RES1 = -1
 */
static void
complete_read(rl_amigafs_t *self, rl_pending_operation_t *op, const rl_msg_t *msg);

static int
transmit_read_request(peer_t *peer, rl_client_handle_t *handle, rl_pending_operation_t *op, rl_uint32 count);

static int
buffer_overlap(rl_client_handle_t *handle, struct DosPacket *packet, rl_uint32* offset, rl_uint32* count);

static void
action_read(rl_amigafs_t *self, struct DosPacket *packet)
{
	LONG error_code = ERROR_SEEK_ERROR; /* TODO: What to use for real read errors? */
	struct FileLock *lock = (struct FileLock *) packet->dp_Arg1;
	rl_client_handle_t *handle = HANDLE_FROM_LOCK(lock);
	rl_uint32 bytes_remaining = (rl_uint32) packet->dp_Arg3;

	rl_pending_operation_t *pending_op;

	RL_LOG_DEBUG(("action_read \"%s\", %d bytes", handle->path, (int) packet->dp_Arg3));

	/* See if we can satisfy some of the request from the read buffer. */
	{
		rl_uint32 offset, count;
		if (buffer_overlap(handle, packet, &offset, &count))
		{
			/* We can return (some) buffered data. */
			handle->offset_lo += count;
			bytes_remaining -= count;
			rl_memcpy((char*) packet->dp_Arg2, &handle->buffer[offset], count);

			/* Early out for request served entirely from buffer. */
			if (0 == bytes_remaining)
			{
				RL_LOG_DEBUG(("early out servicing %u bytes from buffer", count));
				packet->dp_Res1 = count;
				packet->dp_Res2 = 0;
				reply_to_packet(self, packet);
				return;
			}
		}
	}

	/* We have to round-trip to the server for more buffer data.
	 * Populate a pending op and queue it waiting for the network reply.
	 */
	pending_op = alloc_pending(self, packet, RL_MSG_READ_FILE_ANSWER, complete_read);
	if (!pending_op)
	{
		error_code = ERROR_NO_FREE_STORE;
		goto error;
	}

	{
		rl_uint32 bytes_read = (rl_uint32) packet->dp_Arg3 - bytes_remaining;
		pending_op->detail.read.destination = (char*) packet->dp_Arg2 + bytes_read;
	}

	if (0 != transmit_read_request(self->peer, handle, pending_op, bytes_remaining))
		goto error;

	return;

error:
	if (pending_op)
		unlink_pending(self, pending_op);

	packet->dp_Res1 = DOSFALSE;
	packet->dp_Res2 = error_code;
	reply_to_packet(self, packet);
}

static int
transmit_read_request(peer_t *peer, rl_client_handle_t *handle, rl_pending_operation_t *op, rl_uint32 count)
{
	rl_msg_t msg;
	RL_MSG_INIT(msg, RL_MSG_READ_FILE_REQUEST);
	msg.read_file_request.hdr_sequence_num	= op->request_seqno;
	msg.read_file_request.handle			= handle->handle_id;
	msg.read_file_request.offset_hi			= handle->offset_hi;
	msg.read_file_request.offset_lo			= handle->offset_lo;
	/* TODO: split into multiple packets on request side or in answer w/ continuations? */
	msg.read_file_request.length			= RL_MAX_MACRO(count, sizeof(handle->buffer));

	return peer_transmit_message(peer, &msg);
}

static int
buffer_overlap(rl_client_handle_t *handle, struct DosPacket *packet, rl_uint32* offset, rl_uint32* count)
{
	rl_uint32 lo = handle->buffer_start;
	rl_uint32 hi = handle->buffer_start + handle->buffer_len;
	rl_uint32 out_offset;
	rl_uint32 avail = 0;
	rl_uint32 bytes_to_read = (rl_uint32) packet->dp_Arg3;

	/* See if the buffer lies after the cursor */
	if (handle->offset_lo < lo)
		return 0;

	/* See if the buffer lies before the cursor */
	if (hi <= handle->offset_lo)
		return 0;

	/* There is overlap with the start position inside the buffer. */
	avail = hi - handle->offset_lo;
	*offset = out_offset = (handle->offset_lo - lo);
	*count = RL_MIN_MACRO(avail, bytes_to_read);
	return 1;
}

static rl_uint32
readop_bytes_read(rl_pending_operation_t *op, struct DosPacket *packet)
{
	/* Calculate how much we read through pointer subtraction: dp_Arg2 is
	 * the start of the caller-supplied buffer. */
	return (rl_uint8*) op->detail.read.destination - (rl_uint8*) packet->dp_Arg2;
}

static void
complete_read(rl_amigafs_t *self, rl_pending_operation_t *op, const rl_msg_t *msg) 
{
	register struct DosPacket * const packet = op->input_packet;
	struct FileLock *lock = (struct FileLock *) packet->dp_Arg1;
	rl_client_handle_t *handle = HANDLE_FROM_LOCK(lock);
	const rl_uint32 amount_read = msg->read_file_answer.data.length;
	rl_uint32 amount_left = (rl_uint32) packet->dp_Arg3 - readop_bytes_read(op, packet);
	rl_uint32 slice_amount;

	/* Move data from the packet's transfer buffer into the destination.
	 *
	 * The read can have been much greater than requested, so only copy the
	 * externally expected size. For example, when requesting a single byte a
	 * full buffer will be requested. One byte should go to the external
	 * buffer, and the rest should end up in the buffer space to be used for
	 * future reads. */
	slice_amount = RL_MIN_MACRO(amount_left, amount_read);
	rl_memcpy(op->detail.read.destination, msg->read_file_answer.data.base, slice_amount);

	/* Update the handle's virtual file position. TODO: 64-bit filepos. */
	handle->offset_lo += slice_amount;

	/* Move the byte cursor within the pending operation. Note that the op
	 * structure is reused between all reads required to fulfil a non-buffered
	 * read. */
	op->detail.read.destination += slice_amount;

	/* If we're done (all bytes read, or EOF), reply to the ACTION_READ and
	 * unlink the message. */
	if (packet->dp_Arg3 == readop_bytes_read(op, packet) || 0 == amount_read)
	{
		/* Any extra data we managed to read goes into the buffer. */
		handle->buffer_start = handle->offset_lo;
		handle->buffer_len = amount_read - slice_amount;
		rl_memcpy(handle->buffer, msg->read_file_answer.data.base + slice_amount, handle->buffer_len);
		RL_LOG_DEBUG(("Buffered %u bytes from offset %u", handle->buffer_len, handle->buffer_start));

		packet->dp_Res1 = readop_bytes_read(op, packet);
		packet->dp_Res2 = 0;
		RL_LOG_DEBUG(("Returning DOS result %d", packet->dp_Res1));
		reply_to_packet(self, packet);
		unlink_pending(self, op);
	}
	/* Otherwise, continue by leaving the read pending and issue more requests
	 * until we have filled the externally supplied buffer. AmigaDOS allows
	 * handlers to return partial reads, but in practice a lot of code is
	 * written that doesn't properly loop around Read(), so we have to do it
	 * for them or it will fail.
	 */
	else
	{
		/* Just grab the next sequence number and requeue the same operation */
		op->request_seqno = self->seqno++;

		if (0 != transmit_read_request(self->peer, handle, op, packet->dp_Arg3 - readop_bytes_read(op, packet)))
		{
			packet->dp_Res1 = 0;
			packet->dp_Res2 = ERROR_SEEK_ERROR;
			reply_to_packet(self, packet);
			unlink_pending(self, op);
		}
	}
}

/*
 *	ACTION_SEEK	Seek(...)
 *
 *	ARG1:	ARG1 -	fh_Arg1 field of opened FileHandle
 *	ARG2:	LONG -	Position or offset
 *	ARG3:	LONG -	Seek mode
 *
 *	RES1:	LONG -	Position before Seek() took place
 *	RES2:	CODE -	Failure code if RES1 = -1
 */
static void action_seek(rl_amigafs_t *self, struct DosPacket *packet)
{
	struct FileLock *lock = (struct FileLock *) packet->dp_Arg1;
	rl_client_handle_t *handle = HANDLE_FROM_LOCK(lock);
	LONG old_pos;
	LONG seek_amount;

	RL_LOG_DEBUG(("action_seek \"%s\", %d bytes rel %d", handle->path, (int) packet->dp_Arg2, (int) packet->dp_Arg3));

	old_pos = (LONG) handle->offset_lo;
	seek_amount = packet->dp_Arg2;

	switch (packet->dp_Arg3)
	{
	case OFFSET_BEGINNING:
		handle->offset_lo = seek_amount;
		break;
	case OFFSET_CURRENT:
		handle->offset_lo += seek_amount;
		break;
	case OFFSET_END:
		handle->offset_lo = ((LONG) handle->size_lo) + seek_amount;
		break;
	}

	/* assume 32-bit files */
	if (handle->offset_lo > handle->size_lo)
		handle->offset_lo = handle->size_lo;

	packet->dp_Res1 = old_pos;
	packet->dp_Res2 = 0;
	reply_to_packet(self, packet);
}

static const char* get_packet_type_name(const struct DosPacket* packet)
{
	switch (packet->dp_Type)
	{
	case ACTION_STARTUP: return "ACTION_STARTUP";
	case ACTION_GET_BLOCK: return "ACTION_GET_BLOCK";
	case ACTION_SET_MAP: return "ACTION_SET_MAP";
	case ACTION_DIE: return "ACTION_DIE";
	case ACTION_EVENT: return "ACTION_EVENT";
	case ACTION_CURRENT_VOLUME: return "ACTION_CURRENT_VOLUME";
	case ACTION_LOCATE_OBJECT: return "ACTION_LOCATE_OBJECT";
	case ACTION_RENAME_DISK: return "ACTION_RENAME_DISK";
	case ACTION_WRITE: return "ACTION_WRITE";
	case ACTION_READ: return "ACTION_READ";
	case ACTION_FREE_LOCK: return "ACTION_FREE_LOCK";
	case ACTION_DELETE_OBJECT: return "ACTION_DELETE_OBJECT";
	case ACTION_RENAME_OBJECT: return "ACTION_RENAME_OBJECT";
	case ACTION_MORE_CACHE: return "ACTION_MORE_CACHE";
	case ACTION_COPY_DIR: return "ACTION_COPY_DIR";
	case ACTION_WAIT_CHAR: return "ACTION_WAIT_CHAR";
	case ACTION_SET_PROTECT: return "ACTION_SET_PROTECT";
	case ACTION_CREATE_DIR: return "ACTION_CREATE_DIR";
	case ACTION_EXAMINE_OBJECT: return "ACTION_EXAMINE_OBJECT";
	case ACTION_EXAMINE_NEXT: return "ACTION_EXAMINE_NEXT";
	case ACTION_DISK_INFO: return "ACTION_DISK_INFO";
	case ACTION_INFO: return "ACTION_INFO";
	case ACTION_FLUSH: return "ACTION_FLUSH";
	case ACTION_SET_COMMENT: return "ACTION_SET_COMMENT";
	case ACTION_PARENT: return "ACTION_PARENT";
	case ACTION_TIMER: return "ACTION_TIMER";
	case ACTION_INHIBIT: return "ACTION_INHIBIT";
	case ACTION_DISK_TYPE: return "ACTION_DISK_TYPE";
	case ACTION_DISK_CHANGE: return "ACTION_DISK_CHANGE";
	case ACTION_SET_DATE: return "ACTION_SET_DATE";
	case ACTION_SCREEN_MODE: return "ACTION_SCREEN_MODE";
	case ACTION_READ_RETURN: return "ACTION_READ_RETURN";
	case ACTION_WRITE_RETURN: return "ACTION_WRITE_RETURN";
	case ACTION_SEEK: return "ACTION_SEEK";
	case ACTION_FINDUPDATE: return "ACTION_FINDUPDATE";
	case ACTION_FINDINPUT: return "ACTION_FINDINPUT";
	case ACTION_FINDOUTPUT: return "ACTION_FINDOUTPUT";
	case ACTION_END: return "ACTION_END";
	case ACTION_SET_FILE_SIZE: return "ACTION_SET_FILE_SIZE";
	case ACTION_WRITE_PROTECT: return "ACTION_WRITE_PROTECT";
	case ACTION_SAME_LOCK: return "ACTION_SAME_LOCK";
	case ACTION_CHANGE_SIGNAL: return "ACTION_CHANGE_SIGNAL";
	case ACTION_FORMAT: return "ACTION_FORMAT";
	case ACTION_MAKE_LINK: return "ACTION_MAKE_LINK";
	case ACTION_READ_LINK: return "ACTION_READ_LINK";
	case ACTION_FH_FROM_LOCK: return "ACTION_FH_FROM_LOCK";
	case ACTION_IS_FILESYSTEM: return "ACTION_IS_FILESYSTEM";
	case ACTION_CHANGE_MODE: return "ACTION_CHANGE_MODE";
	case ACTION_COPY_DIR_FH: return "ACTION_COPY_DIR_FH";
	case ACTION_PARENT_FH: return "ACTION_PARENT_FH";
	case ACTION_EXAMINE_ALL: return "ACTION_EXAMINE_ALL";
	case ACTION_EXAMINE_FH: return "ACTION_EXAMINE_FH";
	case ACTION_LOCK_RECORD: return "ACTION_LOCK_RECORD";
	case ACTION_FREE_RECORD: return "ACTION_FREE_RECORD";
	case ACTION_ADD_NOTIFY: return "ACTION_ADD_NOTIFY";
	case ACTION_REMOVE_NOTIFY: return "ACTION_REMOVE_NOTIFY";
	case ACTION_EXAMINE_ALL_END: return "ACTION_EXAMINE_ALL_END";
	case ACTION_SET_OWNER: return "ACTION_SET_OWNER";
	case ACTION_SERIALIZE_DISK: return "ACTION_SERIALIZE_DISK";
	default: return "<unknown>";
	}
}

static void process_fs_packet(rl_amigafs_t *self, struct DosPacket* packet)
{
	packet_handler_fn handler = NULL;
	register LONG packet_type = packet->dp_Type;

	RL_LOG_DEBUG(("IN: type=%s Arg1=%08x Arg2=%08x Arg3=%08x Arg4=%08x Arg5=%08x",
				get_packet_type_name(packet),
				packet->dp_Arg1,
				packet->dp_Arg2,
				packet->dp_Arg3,
				packet->dp_Arg4,
				packet->dp_Arg5));

	/* handle the most critical cases first */
	if (ACTION_READ == packet_type)
	{
		handler = action_read;
	}

	/* handle common packets in the continous low range via a lookup table */
	else if (packet_type >= HANDLER_RANGE_1_FIRST && packet_type <= HANDLER_RANGE_1_LAST)
	{
		const lookup_entry_t* entry = &packet_handlers_range_1[packet_type];
		handler = entry->function;
	}
	/* handle later extension packets with a switch--why didn't they order them
	 * continually?! */
	else
	{
		switch (packet_type)
		{
		case ACTION_IS_FILESYSTEM:
			handler = action_is_filesystem;
			break;

		case ACTION_FINDINPUT:
			handler = action_findinput;
			break;

			/*
		case ACTION_FINDOUTPUT:
			handler = action_findoutput;
			break;
			*/

		case ACTION_SEEK:
			handler = action_seek;
			break;

		case ACTION_END:
			handler = action_end;
			break;
		default:
			break;
		}
	}

	if (!handler)
	{
		RL_LOG_WARNING(("don't know how to handle %s", get_packet_type_name(packet)));
		packet->dp_Res1 = DOSFALSE;
		packet->dp_Res2 = ERROR_ACTION_NOT_KNOWN;
		reply_to_packet(self, packet);
		return;
	}
	
	(*handler)(self, packet);
}

int rl_amigafs_init(rl_amigafs_t *self, peer_t *peer, const char *device_name)
{
	RL_LOG_DEBUG(("rl_amigafs_init %p w/ device_name=\"%s\"", self, device_name));

	rl_memset(self, 0, sizeof(rl_amigafs_t));

	self->peer = peer;
	self->root_handle.type = RL_HANDLE_DEVICE;
	self->root_handle.handle_id = (rl_uint32) -1;
	rl_string_copy(sizeof(self->root_handle.path), self->root_handle.path, device_name);

	if (NULL == (self->device_port = CreateMsgPort()))
	{
		RL_LOG_DEBUG(("creating message port failed"));
		goto cleanup;
	}

	if (NULL == (self->device_list = mount_volume(device_name, self->device_port)))
	{
		RL_LOG_DEBUG(("creating volume failed"));
		goto cleanup;
	}

	RL_LOG_DEBUG(("%s: volume mounted", peer ? peer->ident : ""));
	return 0;

cleanup:
	rl_amigafs_destroy(self);
	return 1;
}

void rl_amigafs_destroy(rl_amigafs_t *self)
{
	RL_LOG_DEBUG(("rl_amigafs_destroy %p", self));

	if (self->device_list)
		unmount_volume(self->device_list);

	if (self->device_port)
		DeleteMsgPort(self->device_port);

	self->peer = 0;
}

int rl_amigafs_process_device_message(rl_amigafs_t *self)
{
	struct Message* msg;
	while (NULL != (msg = GetMsg(self->device_port)))
	{
		struct DosPacket *packet = (struct DosPacket *) msg->mn_Node.ln_Name;
		process_fs_packet(self, packet);
	}
	return 0;
}

static rl_pending_operation_t *
find_pending_op(rl_amigafs_t *self, const rl_msg_t *msg) 
{
	const rl_uint32 seqno = msg->handshake_request.hdr_sequence_num;
	rl_pending_operation_t *op = self->pending;

	while (op)
	{
		if (seqno == op->request_seqno)
			return op;
		else
			op = op->next;
	}

	return NULL;

}

static LONG translate_error_code(rl_uint32 error_code)
{
	switch (error_code)
	{
		case RL_NETERR_SUCCESS:
			return 0;

		case RL_NETERR_ACCESS_DENIED:
			return ERROR_OBJECT_IN_USE;

		case RL_NETERR_NOT_FOUND:
			return ERROR_OBJECT_NOT_FOUND;

		case RL_NETERR_NOT_A_FILE:
		case RL_NETERR_NOT_A_DIRECTORY:
			return ERROR_OBJECT_WRONG_TYPE;

		case RL_NETERR_IO_ERROR:
			/* TODO: This is probably going to break. */
			return ERROR_DISK_NOT_VALIDATED;

		case RL_NETERR_INVALID_VALUE:
			/* TODO: This is probably going to break. */
			return ERROR_OBJECT_WRONG_TYPE;

		default:
			return ERROR_DEVICE_NOT_MOUNTED;
	}
}

int rl_amigafs_process_network_message(rl_amigafs_t *self, const rl_msg_t *msg)
{
	const rl_msg_kind_t msg_kind = rl_msg_kind_of(msg);
	int status = 0;
	rl_pending_operation_t *pending_op = NULL;
	
	/* If there isn't any pending operation for this message, throw it away. */
	if (NULL == (pending_op = find_pending_op(self, msg)))
	{
		RL_LOG_DEBUG(("Couldn't find pending operation for message %s w/ seq no %u",
					rl_msg_name(msg_kind), msg->handshake_request.hdr_sequence_num));
		dump_pending_ops(self);
		return 1;
	}

	if (msg_kind == pending_op->expected_answer_type)
	{
		(*pending_op->callback)(self, pending_op, msg);
	}
	else if(msg_kind == RL_MSG_ERROR_ANSWER)
	{
		pending_op->input_packet->dp_Res1 = DOSFALSE;
		pending_op->input_packet->dp_Res2 = translate_error_code(msg->error_answer.error_code);
		reply_to_packet(self, pending_op->input_packet);
		unlink_pending(self, pending_op);
	}
	else
	{
		RL_LOG_DEBUG(("mismatched answer for sequence #%u: got %s but expected %s",
					pending_op->request_seqno,
					rl_msg_name(msg_kind),
					rl_msg_name(pending_op->expected_answer_type)));
		pending_op->input_packet->dp_Res1 = DOSFALSE;
		pending_op->input_packet->dp_Res2 = ERROR_DEVICE_NOT_MOUNTED;
		reply_to_packet(self, pending_op->input_packet);
		unlink_pending(self, pending_op);
		status = 1; /* terminate this connection */
	}

	return status;
}

static void action_die(rl_amigafs_t *self, struct DosPacket* packet) {}

/*
   ACTION_CURRENT_VOLUME

Purpose: identify the volume belonging to a FileHandle
Implements: used by AmigaDOS function ErrorReport(REPORT_STREAM)t
dp_Type - ACTION_CURRENT_VOLUME (7)
dp_Arg1 - fh->fh_Argl
dp_Res1 - BPTR to struct DeviceList
dp_Res2 - ULONG (Exec unit number)
*/
static void action_current_volume(rl_amigafs_t *self, struct DosPacket* packet)
{
	RL_LOG_DEBUG(("action_current_volume"));
	packet->dp_Res1 = MKBADDR(self->device_list);
	packet->dp_Res2 = 0;
	reply_to_packet(self, packet);
	return;
}

static void action_delete_object(rl_amigafs_t *self, struct DosPacket* packet)
{
	RL_LOG_DEBUG(("action_delete_object"));
	packet->dp_Res1 = DOSFALSE;
	packet->dp_Res2 = 0;
	reply_to_packet(self, packet);
	return;
}

static void action_inhibit(rl_amigafs_t *self, struct DosPacket* packet)
{
	RL_LOG_DEBUG(("action_inhibit"));
	packet->dp_Res1 = DOSFALSE;
	packet->dp_Res2 = 0;
	reply_to_packet(self, packet);
}

static void action_rename_disk(rl_amigafs_t *self, struct DosPacket* packet)
{
	RL_LOG_DEBUG(("action_rename_disk"));
	packet->dp_Res1 = DOSFALSE;
	packet->dp_Res2 = 0;
	reply_to_packet(self, packet);
}

static void action_rename_object(rl_amigafs_t *self, struct DosPacket* packet)
{
	RL_LOG_DEBUG(("action_rename_object"));
	packet->dp_Res1 = DOSFALSE;
	packet->dp_Res2 = 0;
	reply_to_packet(self, packet);
}

static void action_create_dir(rl_amigafs_t *self, struct DosPacket* packet)
{
	RL_LOG_DEBUG(("action_rename_object"));
	packet->dp_Res1 = DOSFALSE;
	packet->dp_Res2 = 0;
	reply_to_packet(self, packet);
}

/* static void action_flush(rl_amigafs_t *self, struct DosPacket* packet) {} */
static void action_set_comment(rl_amigafs_t *self, struct DosPacket* packet)
{
	RL_LOG_DEBUG(("action_set_comment"));
	packet->dp_Res1 = DOSFALSE;
	packet->dp_Res2 = 0;
	reply_to_packet(self, packet);
}

static void action_set_file_date(rl_amigafs_t *self, struct DosPacket* packet)
{
	RL_LOG_DEBUG(("action_set_file_date"));
	packet->dp_Res1 = DOSFALSE;
	packet->dp_Res2 = 0;
	reply_to_packet(self, packet);
}
