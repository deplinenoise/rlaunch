

#include <dos/dos.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

static int start(void);

int __saveds begin(void)
{
	return start();
}

#define BSTR_LENGTH(s) ( s ? ((int) (* (char*) s)) : 0 )
#define BSTR_VALUE(s) ( s ? ((const char*) s) + 1 : "" )

#ifndef NDEBUG
#define TRACE(expr) do { log_message expr ; } while(0)
static void log_message(const char *fmt, ...)
{
	char buffer[512];
	va_list args;

	if (!DOSBase)
		return;

	va_start(args, fmt);
	vsprintf(buffer, fmt, args);
	va_end(args);

	FPuts(Output(), buffer);
	FPutC(Output(), '\n');
	Flush(Output());
}
#else
#define TRACE(expr) do { } while(0)
#endif

struct DosLibrary *DOSBase = NULL;

static void construct_bstr(char *start, LONG max_size, const char *input)
{
	LONG len = strlen(input);

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

	*start++ = '\0';
}


typedef enum fs_node_type_t {
	FS_NODE_DIRECTORY,
	FS_NODE_FILE
} fs_node_type_t;

typedef struct fs_node_t
{
	fs_node_type_t type;
	struct fs_node_t *parent;
	char name[32];
} fs_node_t;

#define FS_NODE_FROM_LOCK(lock) ((fs_node_t*) (lock)->fl_Key)

typedef struct filesys_t
{
	int num_locks;
	struct MsgPort *message_port;
	struct DeviceList* device_list;
	fs_node_t *root_node;
} filesys_t;

static struct FileLock *allocate_lock(filesys_t *fs, fs_node_t *node, LONG access)
{
	struct FileLock *result = (struct FileLock*) AllocMem(sizeof(struct FileLock), MEMF_PUBLIC | MEMF_CLEAR);
	if (!result)
		return NULL;
	TRACE(("Allocated lock %lx for fs_node %lx", result, node));
	result->fl_Access = access;
	result->fl_Key = (LONG) node;
	result->fl_Task = fs->message_port;
	result->fl_Volume = MKBADDR(fs->device_list);
	return result;
}

static fs_node_t blueprint[] = {
	{ FS_NODE_DIRECTORY, NULL, "<root>" },
	{ FS_NODE_FILE, NULL, "foo" },
	{ FS_NODE_FILE, NULL, "bar" },
	{ FS_NODE_DIRECTORY, NULL, "baz" },
	{ FS_NODE_FILE, NULL, "qux" },
};

static fs_node_t *fabricate_fs_nodes(void)
{
	blueprint[1].parent = &blueprint[0];
	blueprint[2].parent = &blueprint[0];
	blueprint[3].parent = &blueprint[0];
	blueprint[4].parent = &blueprint[3];
	return &blueprint[0];
}

#define HANDLER_RANGE_1_FIRST (0)
#define HANDLER_RANGE_1_LAST (34)

#define BP1 1
#define BP2 2
#define BP3 4
#define BP4 8

typedef void (*packet_handler_fn)(filesys_t *fs, struct DosPacket *packet);

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

/* Handler functions implemented here. */

static void action_die				(filesys_t *fs, struct DosPacket *packet);
static void action_current_volume	(filesys_t *fs, struct DosPacket *packet);
static void action_locate_object	(filesys_t *fs, struct DosPacket *packet);
static void action_free_lock		(filesys_t *fs, struct DosPacket *packet);
static void action_delete_object	(filesys_t *fs, struct DosPacket *packet);
static void action_copy_dir			(filesys_t *fs, struct DosPacket *packet);
static void action_examine_object	(filesys_t *fs, struct DosPacket *packet);
static void action_examine_next		(filesys_t *fs, struct DosPacket *packet);
static void action_disk_info		(filesys_t *fs, struct DosPacket *packet);
static void action_info				(filesys_t *fs, struct DosPacket *packet);
static void action_parent			(filesys_t *fs, struct DosPacket *packet);
static void action_inhibit			(filesys_t *fs, struct DosPacket *packet);

static void action_rename_disk		(filesys_t *fs, struct DosPacket *packet);
static void action_rename_object	(filesys_t *fs, struct DosPacket *packet);
static void action_set_protect		(filesys_t *fs, struct DosPacket *packet);
static void action_create_dir		(filesys_t *fs, struct DosPacket *packet);
static void action_flush			(filesys_t *fs, struct DosPacket *packet);
static void action_set_comment		(filesys_t *fs, struct DosPacket *packet);
static void action_set_file_date	(filesys_t *fs, struct DosPacket *packet);

static void action_findinput		(filesys_t *fs, struct DosPacket *packet);

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

static char temp_bstr[257];

static const char *debug_extract_bstr(BSTR str)
{
	int len, i;
	char* data = (char*) str;

	if (!data)
	{
		temp_bstr[0] = 0;
		return temp_bstr;
	}

	len = *data;
	for (i = 0; i< len; ++i)
	{
		temp_bstr[i] = data[i+1];
	}

	temp_bstr[i] = '\0';
	return temp_bstr;
}

int unmount_volume(struct DeviceList *volume)
{
	/* no volume, or locked; can't unmount */
	if(volume == NULL || volume->dl_Lock != NULL)
		return 1;

	RemDosEntry((struct DosList *)volume);
	FreeDosEntry((struct DosList *)volume);
	return 0;
}

static void action_is_filesystem(filesys_t *fs, struct DosPacket* packet)
{
	TRACE(("action_is_filesystem"));
	packet->dp_Res1 = DOSTRUE;
	packet->dp_Res2 = 0;
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
static void action_findinput(filesys_t *fs, struct DosPacket *packet)
{
	struct FileHandle *out_handle = (struct FileHandle*) packet->dp_Arg1;
	struct FileLock *dir_lock = (struct FileLock *)packet->dp_Arg2;
	const BSTR object_name = packet->dp_Arg3;
	const int object_name_length = BSTR_LENGTH(object_name);
	const char *object_name_str = object_name_length ?
		((const char*)(object_name+1)) : (const char *) "";

    TRACE(("FINDINPUT: directory=%lx, name=\"%s\"", dir_lock, object_name_str));

	packet->dp_Res1 = DOSFALSE;
	packet->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
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
static void action_examine_object(filesys_t *fs, struct DosPacket *packet)
{
	struct FileLock *lock = (struct FileLock*) packet->dp_Arg1;
	struct FileInfoBlock *fib = (struct FileInfoBlock*) packet->dp_Arg2;
	struct fs_node_t *node = NULL;

	TRACE(("EXAMINE_OBJECT Lock=%lx fib=%lx", packet->dp_Arg1, packet->dp_Arg2));

	if (lock)
		node = FS_NODE_FROM_LOCK(lock);

	if (!node)
		node = fs->root_node;

	if (node == fs->root_node)
	{
		TRACE(("Examining root node"));
		memset(fib, 0, sizeof(*fib));
		fib->fib_DiskKey = 0L;
		fib->fib_EntryType = fib->fib_DirEntryType = ST_ROOT;
		memcpy(fib->fib_FileName, BADDR(fs->device_list->dl_Name), BSTR_LENGTH(BADDR(fs->device_list->dl_Name))+1);
		fib->fib_Protection = 0;
		fib->fib_Size = 0;
		fib->fib_NumBlocks = 0;
		fib->fib_Date = fs->device_list->dl_VolumeDate;
		construct_bstr(fib->fib_Comment, sizeof(fib->fib_Comment), "Torsk Force");
		packet->dp_Res1 = DOSTRUE;
		packet->dp_Res2 = 0;
	}
	else
	{
		packet->dp_Res1 = DOSFALSE;
		packet->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
	}
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
static void action_examine_next(filesys_t *fs, struct DosPacket *packet)
{
	TRACE(("EXAMINE_NEXT Lock=%lx", packet->dp_Arg1));
	packet->dp_Res1 = DOSFALSE;
	packet->dp_Res2 = ERROR_NO_MORE_ENTRIES;
}

/* Helper function to populate a InfoData struct from the specified fs. */
static void fill_in_infodata(filesys_t *fs, struct InfoData *info)
{
	info->id_NumSoftErrors = 0;
	info->id_UnitNumber = 1;
	info->id_DiskState = ID_VALIDATED;
	info->id_NumBlocks = 1000;
	info->id_NumBlocksUsed = 500;
	info->id_BytesPerBlock = 1;
	info->id_DiskType = ID_FFS_DISK;
	info->id_VolumeNode = MKBADDR(fs->device_list);
	info->id_InUse = 0;
}

/*	ACTION_DISK_INFO	Info(...)
 *	ARG1:	BPTR -	Pointer to an InfoData structure to fill in
 *	RES1:	BOOL -	Success/Failure (DOSTRUE/DOSFALSE)
 */
static void action_disk_info(filesys_t *fs, struct DosPacket* packet)
{
	TRACE(("ACTION_DISK_INFO"));
	fill_in_infodata(fs, (struct InfoData*) packet->dp_Arg1);
	packet->dp_Res1 = DOSTRUE;
	packet->dp_Res2 = 0;
}

/*
 *	ACTION_INFO	<sendpkt only>
 *
 *	ARG1:	LOCK -	Lock on volume
 *	ARG2:	BPTR -	Pointer to an InfoData structure to fill in
 *
 *	RES1:	BOOL -	Success/Failure (DOSTRUE/DOSFALSE)
 */
static void action_info(filesys_t *fs, struct DosPacket *packet)
{
	struct FileLock *lock = (struct FileLock*) packet->dp_Arg1;

	TRACE(("ACTION_INFO lock=%lx \"%s\"", lock, FS_NODE_FROM_LOCK(lock)->name));

	if (NULL == lock || lock->fl_Volume != MKBADDR(fs->device_list))
	{
		TRACE(("--> failed, invalid lock"));
		packet->dp_Res1 = DOSFALSE;
		packet->dp_Res2 = ERROR_ACTION_NOT_KNOWN;
	}
	else
	{
		fill_in_infodata(fs, (struct InfoData*) packet->dp_Arg2);
		packet->dp_Res1 = DOSTRUE;
		packet->dp_Res2 = 0;
	}
}

static int strip_absolute_device(const char **path, const char *device_name, int path_length)
{
	const char *p;

	/*
	 * Throw away everything up to the first colon. Not sure if this is
	 * entirely correct.
	 */

	p = strchr(path, ':');

	if (!p)
		return 0;

	*path = p;
	return 1;
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
static void action_locate_object(filesys_t *fs, struct DosPacket* packet)
{
	struct FileLock *dir_lock = (struct FileLock *)packet->dp_Arg1;
	const BSTR object_name = packet->dp_Arg2;
	const int object_name_length = BSTR_LENGTH(object_name);
	const char *object_name_str = object_name_length ?
		((const char*)(object_name+1)) : (const char *) "";
	const LONG mode = packet->dp_Arg3;
	struct FileLock *result_lock = NULL;

	fs_node_t *node;
	size_t 
	const char *path = object_name_str;

    TRACE(("LOCATE_OBJECT: directory=%s, name=\"%s\" mode=%ld (%s)",
				dir_lock ? FS_NODE_FROM_LOCK(dir_lock)->name : "<null>",
				debug_extract_bstr(object_name),
				mode,
				mode == -1 ? "SHARED_LOCK/ACCESS_READ" : "EXCLUSIVE_LOCK/ACCESS_WRITE"));

	/*
	 * Establish a node to start "locating" from--if we don't have a lock,
	 * start from the root
	 */
	if (!dir_lock)
		node = fs->root_node;
	else
		node = FS_NODE_FROM_LOCK(dir_lock);

	/*
	 * If the name is absolute (starts with a device name followed by a
	 * colon, or a lone colon) we override the start location to be the root.
	 */
	if (strip_absolute_device(fs, &path))
	{
		node = fs->root_node;
	}

	/* Traverse the remaining path elements and walk the path structure */
	while (*path)
	{
		const char *next = strchr(path, '/');
		size_t token_length;

		if (!next)
			break;

		token_length = (size_t) (next - path);

		if (token_length == 0)
			break;

		path = next+1;
	}


	if (result_lock)
	{
		TRACE(("Returning lock: %lx for node \"%s\"", result_lock, FS_NODE_FROM_LOCK(result_lock)->name));
		packet->dp_Res1 = MKBADDR(result_lock);
		packet->dp_Res2 = 0;
		++fs->num_locks;
	}
	else
	{
		TRACE(("Returning ERROR_OBJECT_NOT_FOUND"));
		packet->dp_Res1 = DOSFALSE;
		packet->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
	}
}

/*
 *	ACTION_FREE_LOCK	UnLock(...)
 *	ARG1:	LOCK -	Lock to free
 *	RES1:	BOOL -	DOSTRUE
 */
static void action_free_lock(filesys_t *fs, struct DosPacket* packet)
{
	struct FileLock *lock = (struct FileLock*) packet->dp_Arg1;

	if (lock)
	{
		TRACE(("ACTION_FREE_LOCK: lock: %lx (%s) Node=%lx", lock, FS_NODE_FROM_LOCK(lock)->name, lock->fl_Key));
		FreeMem(lock, sizeof(struct FileLock));

		--fs->num_locks;
		packet->dp_Res1 = DOSTRUE;
		packet->dp_Res2 = 0;
	}
	else
	{
		TRACE(("ACTION_FREE_LOCK w/ null lock?!"));
		packet->dp_Res1 = DOSFALSE;
		packet->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
	}
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
static void action_set_protect(filesys_t *fs, struct DosPacket *packet)
{
	TRACE(("SET_PROTECT Lock=%lx Name=\"%s\" Bits=%ld",
				packet->dp_Arg2,
				debug_extract_bstr(packet->dp_Arg3),
				packet->dp_Arg4));
	packet->dp_Res1 = DOSFALSE;
	packet->dp_Res2 = ERROR_WRITE_PROTECTED;
}

/*
 *	ACTION_COPY_DIR		DupLock(...)
 *
 *	ARG1:	LOCK -	Lock to duplicate
 *
 *	RES1:	LOCK -	Duplicated lock or 0 to indicate failure
 *	RES2:	CODE -	Failure code if RES1 = 0
 */
static void action_copy_dir(filesys_t *fs, struct DosPacket *packet)
{
	struct FileLock *lock = (struct FileLock*) packet->dp_Arg1;
	if (lock)
	{
		TRACE(("ACTION_COPY_DIR for %lx \"%s\"", lock, FS_NODE_FROM_LOCK(lock)->name));
		packet->dp_Res1 = MKBADDR(allocate_lock(fs, FS_NODE_FROM_LOCK(lock), lock->fl_Access));
		if (!packet->dp_Res1)
		{
			packet->dp_Res2 = ERROR_NO_FREE_STORE;
		}
		else
		{
			packet->dp_Res2 = 0;
			TRACE(("Resulting lock is %lx", packet->dp_Res1 << 2));
		}
	}
	else
	{
		TRACE(("ACTION_COPY_DIR with null lock?!"));
		packet->dp_Res1 = 0;
		packet->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
	}
}

/*
 *	ACTION_PARENT	Parent(...)
 *
 *	ARG1:	LOCK -	Lock on object to get the parent of
 *
 *	RES1:	LOCK -	Parent lock
 *	RES2:	Failure code if RES1 = 0
 */
static void action_parent(filesys_t *fs, struct DosPacket *packet)
{
	struct FileLock *lock = (struct FileLock*) packet->dp_Arg1;
	struct fs_node_t *node = lock->fl_Key ? (struct fs_node_t*) lock->fl_Key : NULL;

	TRACE(("ACTION_PARENT for lock %lx (%s)", lock, FS_NODE_FROM_LOCK(lock)->name));

	if (node == fs->root_node)
	{
		TRACE(("[NULL parent for the root]"));
		packet->dp_Res1 = 0;
		packet->dp_Res2 = 0;
	}
	else if (NULL == node)
	{
		TRACE(("[error; NULL node]"));
		packet->dp_Res1 = DOSFALSE;
		packet->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
	}
	else
	{
		struct FileLock *result = allocate_lock(fs, node->parent, lock->fl_Access);
		if (result)
		{
			packet->dp_Res1 = MKBADDR(result);
			packet->dp_Res2 = 0;
		}
		else
		{
			packet->dp_Res1 = DOSFALSE;
			packet->dp_Res2 = ERROR_OBJECT_NOT_FOUND;
		}
	}
}

const char* get_packet_type_name(const struct DosPacket* packet)
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

static void respond_to_packet(filesys_t *fs, struct DosPacket* packet)
{
	packet_handler_fn handler = NULL;
	int flags = 0;
	const register LONG packet_type = packet->dp_Type;

	TRACE(("IN: type=%s Arg1=%lx Arg2=%lx Arg3=%lx Arg4=%lx Arg5=%lx",
				get_packet_type_name(packet),
				packet->dp_Arg1,
				packet->dp_Arg2,
				packet->dp_Arg3,
				packet->dp_Arg4,
				packet->dp_Arg5));

	/* handle the most critical cases first */
	if (ACTION_READ == packet_type)
	{
		handler = NULL /* action_read */;
		flags = 0;
	}
	/* handle common packets in the continous low range via a lookup table */
	else if (packet_type >= HANDLER_RANGE_1_FIRST && packet_type <= HANDLER_RANGE_1_LAST)
	{
		const lookup_entry_t* entry = &packet_handlers_range_1[packet_type];
		handler = entry->function;
		flags = entry->flags;
	}
	/* handle later extension packets with a switch--why didn't they order them
	 * continually going forward in 2.0? */
	else
	{
		switch (packet_type)
		{
		case ACTION_IS_FILESYSTEM:
			handler = action_is_filesystem;
			flags = 0;
			break;

		case ACTION_FINDINPUT:
			handler = action_findinput;
			flags = BP1 | BP2 | BP3;
			break;


		default:
			break;
		}
	}

	if (!handler)
	{
		TRACE(("don't know how to handle %s", get_packet_type_name(packet)));
		packet->dp_Res1 = DOSFALSE;
		packet->dp_Res2 = ERROR_ACTION_NOT_KNOWN;
		return;
	}
	
	if (flags & BP1) packet->dp_Arg1 <<= 2;
	if (flags & BP2) packet->dp_Arg2 <<= 2;
	if (flags & BP3) packet->dp_Arg3 <<= 2;
	if (flags & BP4) packet->dp_Arg4 <<= 2;

	(*handler)(fs, packet);

	if (flags & BP1) packet->dp_Arg1 >>= 2;
	if (flags & BP2) packet->dp_Arg2 >>= 2;
	if (flags & BP3) packet->dp_Arg3 >>= 2;
	if (flags & BP4) packet->dp_Arg4 >>= 2;

	TRACE(("OUT: Res1=%08lx Res2=%08lx", packet->dp_Res1, packet->dp_Res2));
}

static int start(void)
{
	struct MsgPort* device_port = NULL;
	struct DeviceList* device_list = NULL;

	filesys_t my_filesys;

	if (NULL == (DOSBase = (struct DosLibrary*) OpenLibrary("dos.library", 37)))
		goto cleanup;

	TRACE(("creating message port"));
	if (NULL == (device_port = CreateMsgPort()))
	{
		TRACE(("creating message port failed"));
		goto cleanup;
	}

	TRACE(("creating volume"));
	if (NULL == (device_list = mount_volume("FOO", device_port)))
	{
		TRACE(("creating volume failed"));
		goto cleanup;
	}

	TRACE(("Volume w/ name \"%s\" mounted", BSTR_VALUE(BADDR(device_list->dl_Name))));

	my_filesys.device_list = device_list;
	my_filesys.message_port = device_port;
	my_filesys.num_locks = 0;
	my_filesys.root_node = fabricate_fs_nodes();

	TRACE(("entering event loop"));
	for (;;)
	{
		struct Message* msg;
		ULONG wait_result;

		wait_result = Wait((1 << device_port->mp_SigBit) | SIGBREAKF_CTRL_C);

		if (SIGBREAKF_CTRL_C & wait_result)
			break;

		while (msg = GetMsg(device_port))
		{
			struct MsgPort* reply_port;
			struct DosPacket *packet = (struct DosPacket *) msg->mn_Node.ln_Name;

			respond_to_packet(&my_filesys, packet);

			reply_port = packet->dp_Port;
			packet->dp_Port = device_port;
			PutMsg(reply_port, packet->dp_Link);
		}
	}

cleanup:
	TRACE(("cleaning up"));
	if (device_list)
		unmount_volume(device_list);

	if (device_port)
		DeleteMsgPort(device_port);

	/*
	if (my_filesys.root_node)
		fs_node_free(my_filesys.root_node);
		*/

	if (DOSBase)
		CloseLibrary((struct Library*)DOSBase);

	return RETURN_OK;
}
