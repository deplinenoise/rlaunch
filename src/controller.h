#ifndef RL_CONTROLLER_H
#define RL_CONTROLLER_H

typedef enum controller_state_tag
{
	CONTROLLER_INITIAL,
	CONTROLLER_WAIT_EXECUTABLE_LAUNCHED,
	CONTROLLER_FILE_SERVING,
	CONTROLLER_ERROR
} controller_state_t;

enum {
	/* Max number of simultaneous files open. */
	RL_MAX_FILE_HANDLES = 16
};

typedef struct rl_filehandle_tag
{
#if defined(WIN32)
	void* handle;
	void* find_handle;
#elif defined(RL_POSIX)
	int handle;
	void *dir_handle; /* DIR* */
#endif

	char native_path[260];
	int type;
	unsigned int size;
} rl_filehandle_t;

typedef struct rl_controller_tag
{
	controller_state_t state;

	/* File server state */
	rl_filehandle_t root_handle;
	rl_filehandle_t vinput_handle;
	rl_filehandle_t voutput_handle;
	rl_filehandle_t handles[RL_MAX_FILE_HANDLES];

	/* Startup options */
	const char* executable;
	const char *arguments[16];
	int arg_count;
} rl_controller_t;

struct peer_tag;
union rl_msg_tag;

/* file_server.c */
int rl_file_serve(struct peer_tag *peer, const union rl_msg_tag *msg);

#endif
