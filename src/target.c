
#include "util.h"
#include "peer.h"
#include "protocol.h"
#include "rlnet.h"
#include "socket_includes.h"
#include "version.h"

#if defined(RL_POSIX)
#include <sys/select.h>
#include <signal.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

#if defined(__VBCC__)
/* bump the stack size */
extern size_t __stack;
size_t __stack = 65536;
#endif

#if !defined(__AMIGA__)
typedef struct rl_amigafs_tag { char dummy; } rl_amigafs_t;
#endif

#if defined(__AMIGA__)
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dostags.h>

#include "amigafs.h"

#ifdef __VBCC__
#define bzero(p, len) rl_memset(p, 0, len)
#endif

#ifndef __VBCC__
struct ExecBase *SysBase = NULL;
#else
extern struct ExecBase *SysBase;
#endif
struct DosLibrary *DOSBase = NULL;
static struct MsgPort* g_process_msg_port = NULL;

#elif defined(WIN32)
static volatile long sigbreak_occured = 0;
# define SIGBREAKF_CTRL_C 1
# define WaitSelect(nfds,r,w,e,t,sigmask) (*sigmask = sigbreak_occured, select(nfds,r,w,e,t))

#elif defined(RL_POSIX)
static volatile long sigbreak_occured = 0;
# define SIGBREAKF_CTRL_C 1
# define WaitSelect(nfds,r,w,e,t,sigmask) (*sigmask = 0, select(nfds,r,w,e,t))

#endif

static rl_socket_t add_peers_to_fd_set(fd_set *read_fds, fd_set *write_fds, peer_t *head)
{
	rl_socket_t max = 0;
	while (head)
	{
		const rl_socket_t this_fd = head->fd;

		FD_SET(this_fd, read_fds);

		if (head->update_result & PEER_STATUS_NEED_OUTPUT)
			FD_SET(this_fd, write_fds);

		if (this_fd > max)
			max = this_fd;
		head = head->next;
	}
	return max;
}

#if defined(__AMIGA__)
typedef struct launch_msg_tag
{
	struct Message msg_base;
	const char* device_name;
	char command_path[128];
	char input_path[64];
	char output_path[64];
	char arguments[256];
	int peer_index;
	struct FileLock *root_lock;
	LONG result_code;
} launch_msg_t;

static __saveds ULONG cmd_launcher(void)
{
	launch_msg_t *launch_msg;
	BPTR ihandle = 0, ohandle = 0;
	struct DosLibrary *DOSBase = 0;
	struct ExecBase *SysBase;
	char cmdline_with_args[512];

	struct TagItem system_tags[] = {
		{ NP_CurrentDir,			0 }, /* filled in below */
		{ SYS_Input,				0 },
		{ SYS_Output,				0 },
		{ SYS_Asynch,				FALSE },
		{ SYS_UserShell,			TRUE },
		{ NP_CloseInput,			FALSE },
		{ NP_CloseOutput,			FALSE },
		{ TAG_DONE, 0 }
	};

	SysBase = *((struct ExecBase**) 4);

	if (!(DOSBase = (struct DosLibrary *) OpenLibrary("dos.library", 37)))
		goto leave;

	/* Wait for the launch message to arrive. */
	{
		struct Process* this_proc = (struct Process *) FindTask(NULL);
		WaitPort(&this_proc->pr_MsgPort);
		launch_msg = (launch_msg_t*) GetMsg(&this_proc->pr_MsgPort);
	}

	/* Open input and output file handles */
	ihandle = Open(launch_msg->input_path, MODE_OLDFILE);
	ohandle = Open(launch_msg->output_path, MODE_NEWFILE);

	/* Populate the relevant tags with handle data */
	system_tags[0].ti_Data = (Tag) MKBADDR(launch_msg->root_lock);
	system_tags[1].ti_Data = (Tag) ihandle;
	system_tags[2].ti_Data = (Tag) ohandle;

	/* Format "<cmd> <args>" or "<cmd>" */
	if (launch_msg->arguments[0])
		rl_format_msg(cmdline_with_args, sizeof(cmdline_with_args), "%s %s",
					  launch_msg->command_path, launch_msg->arguments);
	else
		rl_format_msg(cmdline_with_args, sizeof(cmdline_with_args), "%s",
					  launch_msg->command_path);

	if (0 != SystemTagList(cmdline_with_args, &system_tags[0]))
	{
		/* If SystemTagList() fails we have to clean up the current directory
		 * lock manually */
		UnLock(MKBADDR(launch_msg->root_lock));
		launch_msg->result_code = 1;
	}
	else
	{
		launch_msg->result_code = 0;
	}

	RL_LOG_DEBUG(("[thread] command %s completed with code ",
				  launch_msg->command_path, launch_msg->result_code));

	/* The handles have not been closed, so clean them up now. */
	if (ihandle) Close(ihandle);
	if (ohandle) Close(ohandle);

	/* Reply to parent with result of the execution. */
	ReplyMsg((struct Message*) launch_msg);

leave:
	if (DOSBase)
		CloseLibrary((struct Library*) DOSBase);
	return 0;
}

static int async_spawn(peer_t *peer, const char *cmd, const char *arguments)
{
	char device_name[32];
	struct Process *launcher_proc = NULL;
	rl_amigafs_t * const fs = (rl_amigafs_t *) peer->userdata;
	launch_msg_t * const launch_msg = RL_ALLOC_TYPED_ZERO(launch_msg_t);

	struct TagItem launch_tags[] =
	{
		{ NP_Output,			0 },
		{ NP_Entry,				(ULONG)cmd_launcher },
		{ NP_StackSize,			8000 },
		{ NP_Name,				(ULONG)"Process Launcher" },
		{ NP_Cli,				TRUE },
		{ NP_CloseInput,		FALSE },
		{ NP_CloseOutput,		FALSE },
		{ TAG_DONE,				0 }
	};

	if (!launch_msg)
		goto cleanup;

	/* Store peer index rather than peer pointer, as the peer might disconnect
	 * while the command is running. */
	launch_msg->peer_index = peer->peer_index;

	launch_tags[0].ti_Data = (Tag) Output();

	/* Produce e.g. "TBL2" */
	rl_format_msg(device_name, sizeof(device_name), RLAUNCH_BASE_DEVICE_NAME "%d", peer->peer_index);

	/* Start the executable in the network device root unless it is an absolute
	 * path, in which case we blindly just use that. This is useful to run e.g.
	 * c:list and other stuff as a simple remote shell.
	 */
	if (NULL == rl_strchr(cmd, ':'))
		rl_format_msg(launch_msg->command_path, sizeof(launch_msg->command_path),
					  "%s:%s", device_name, cmd);
	else
		rl_string_copy(sizeof(launch_msg->command_path), launch_msg->command_path, cmd);

	/* Copy argument string */
	rl_string_copy(sizeof(launch_msg->arguments), launch_msg->arguments, arguments);

	RL_LOG_INFO(("launch cmd string: %s", launch_msg->command_path));
	RL_LOG_INFO(("launch cmd args: %s", launch_msg->arguments));

	/* Produce e.g. "TBL2:+virtual-input+ */
	rl_format_msg(launch_msg->input_path, sizeof(launch_msg->input_path),
				  "%s:%s", device_name, RLAUNCH_VIRTUAL_INPUT_FILE);
	RL_LOG_INFO(("launch input: %s", launch_msg->input_path));

	/* Produce e.g. "TBL2:+virtual-output+ */
	rl_format_msg(launch_msg->output_path, sizeof(launch_msg->output_path),
				  "%s:%s", device_name, RLAUNCH_VIRTUAL_OUTPUT_FILE);
	RL_LOG_INFO(("launch output: %s", launch_msg->output_path));

	/* Allocate a root lock structure as if opened by Open() on the device. The
	 * launcher process will take ownership of the volume lock and use that as
	 * the current directory of the spawned executable. This could indeed have
	 * been done by the thread through Open(), but we save some time and just
	 * allocate the lock here.
	 */
	launch_msg->root_lock = rl_amigafs_alloc_root_lock(fs, SHARED_LOCK);

	if (!(launcher_proc = CreateNewProcTagList(launch_tags)))
	{
		RL_LOG_WARNING(("Couldn't kick launcher thread"));
		goto cleanup;
	}

	/* Sent the launch message to the thread which will take ownership of it. */
	launch_msg->msg_base.mn_ReplyPort = g_process_msg_port;
	launch_msg->msg_base.mn_Length = sizeof(launch_msg_t);
	PutMsg(&launcher_proc->pr_MsgPort, (struct Message*) launch_msg);
	return 0;

cleanup:
	if (launch_msg)
	{
		if (launch_msg->root_lock)
		{
			rl_amigafs_free_lock(fs, launch_msg->root_lock);
		}
		RL_FREE_TYPED(launch_msg_t, launch_msg);
	}
	return 1;
}
#endif


static int on_launch_executable_request(peer_t *peer, const rl_msg_t *msg)
{
	rl_msg_t answer;
	int spawn_result = 1;
	RL_LOG_INFO(("launch executable: '%s'", msg->launch_executable_request.path));

#if defined(__AMIGA__)
	spawn_result = async_spawn(peer, msg->launch_executable_request.path, msg->launch_executable_request.arguments);
#endif

	if (0 == spawn_result)
	{
		RL_MSG_INIT(answer, RL_MSG_LAUNCH_EXECUTABLE_ANSWER);
		answer.launch_executable_answer.hdr_in_reply_to = msg->launch_executable_request.hdr_sequence_num;
	}
	else
	{
		RL_MSG_INIT(answer, RL_MSG_ERROR_ANSWER);
		answer.error_answer.error_code = RL_NETERR_SPAWN_FAILURE;
	}
	return peer_transmit_message(peer, &answer);
}

static int on_message_received(peer_t *peer, const rl_msg_t *msg)
{
#ifdef __AMIGA__
	rl_amigafs_t *fs = (rl_amigafs_t *) peer->userdata;
#endif
	
	if (RL_MSG_LAUNCH_EXECUTABLE_REQUEST == rl_msg_kind_of(msg))
		return on_launch_executable_request(peer, msg);
#ifdef __AMIGA__
	else if (fs)
		return rl_amigafs_process_network_message(fs, msg);
#endif
	else
		return 1;
}

static int on_connected(struct peer_tag *peer)
{
	return 0;
}

static const peer_callbacks_t server_callbacks = { on_message_received, on_connected };

static peer_t *accept_peer(rl_socket_t server_fd)
{
	rl_socket_t peer_fd;
	struct sockaddr_in remote_addr;
	socklen_t remote_addr_len = sizeof(remote_addr);

	peer_t *peer = NULL;
	rl_amigafs_t *amifs = NULL;

	peer_fd = (rl_socket_t) accept(server_fd, (struct sockaddr*) &remote_addr, &remote_addr_len);
	if (INVALID_SOCKET == peer_fd)
	{
		RL_LOG_WARNING(("couldn't accept connection"));
		goto error_cleanup;
	}

	if (sizeof(struct sockaddr_in) != remote_addr_len || remote_addr.sin_family != AF_INET)
	{

		RL_LOG_WARNING(("incoming connection isn't TCP/IP"));
		goto error_cleanup;
	}

	if (NULL == (peer = RL_ALLOC_TYPED_ZERO(peer_t)))
	{
		RL_LOG_WARNING(("out of memory allocating peer_t"));
		goto error_cleanup;
	}

#if defined(__AMIGA__)
	if (NULL == (amifs = RL_ALLOC_TYPED_ZERO(rl_amigafs_t)))
	{
		RL_LOG_WARNING(("out of memory allocating rl_amigafs_t"));
		goto error_cleanup;
	}
#endif

   	if (0 != (peer_init(peer, peer_fd, (struct sockaddr*) &remote_addr, &server_callbacks, PEER_INIT_TARGET, amifs)))
	{
		RL_LOG_WARNING(("couldn't init peer"));
		goto error_cleanup;
	}

#if defined(__AMIGA__)
	{
		char device_name[32];
		rl_format_msg(device_name, sizeof(device_name), RLAUNCH_BASE_DEVICE_NAME "%d", peer->peer_index);

		if (0 != rl_amigafs_init(amifs, peer, device_name))
		{
			RL_LOG_WARNING(("couldn't init amiga fs %s for peer %s", device_name, peer->ident));
			peer_destroy(peer);
			goto error_cleanup;
		}
	}
#endif

	return peer;
	
error_cleanup:
	if (peer) RL_FREE_TYPED(peer_t, peer);
#if defined(__AMIGA__)
	if (amifs) RL_FREE_TYPED(rl_amigafs_t, amifs);
#endif

	if (INVALID_SOCKET != peer_fd)
		CloseSocket(peer_fd);

	return NULL;
}

static void serve(const rl_socket_t server_fd)
{
	fd_set read_fds, write_fds;
	peer_t *peers = NULL;

	for (;;)
	{
		rl_socket_t new_peer_socket = INVALID_SOCKET;
		int nfds, num_ready_fds;
		rl_socket_t max_peer_fd;
		struct timeval timeout;
		unsigned long signal_mask = SIGBREAKF_CTRL_C;
		peer_t *new_peer;

#if defined(__AMIGA__)
		/* Also wake on messages due to process termination */
		signal_mask |= 1 << g_process_msg_port->mp_SigBit;

		/* On the Amiga, add the signal bits for all file systems we're serving as well. */
		{
			peer_t *peer = peers;
			while (peer)
			{
				rl_amigafs_t *device = (rl_amigafs_t *) peer->userdata;
				signal_mask |= 1 << device->device_port->mp_SigBit;
				peer = peer->next;
			}
		}
#endif
		FD_ZERO(&read_fds);
		FD_ZERO(&write_fds);
		FD_SET(server_fd, &read_fds);

		max_peer_fd = add_peers_to_fd_set(&read_fds, &write_fds, peers);

		nfds = (int) (server_fd > max_peer_fd ? server_fd : max_peer_fd);

		timeout.tv_sec = 1;
		timeout.tv_usec = 0;

		num_ready_fds = WaitSelect(nfds+1, &read_fds, &write_fds, NULL, &timeout, &signal_mask);

#if defined(__AMIGA__)
		/* See if any file systems need attention. */
		{
			peer_t *peer = peers;
			while (peer)
			{
				rl_amigafs_t *amifs = (rl_amigafs_t *) peer->userdata;
				if (signal_mask & (1 << amifs->device_port->mp_SigBit))
				{
					rl_amigafs_process_device_message(amifs);
				}
				peer = peer->next;
			}
		}

		if (signal_mask & (1 << g_process_msg_port->mp_SigBit))
		{
			launch_msg_t *msg = (launch_msg_t*) GetMsg(g_process_msg_port);
			peer_t *peer = peers;

			RL_LOG_INFO(("%s launch completed; result %d", msg->command_path, msg->result_code));

			while (peer)
			{
				if (peer->peer_index == msg->peer_index)
				{
					rl_msg_t req;
					RL_MSG_INIT(req, RL_MSG_EXECUTABLE_DONE_REQUEST);
					req.executable_done_request.result_code = msg->result_code;
					peer_transmit_message(peer, &req);
					break;
				}
				peer = peer->next;
			}
			
			if (!peer)
			{
				RL_LOG_WARNING(("couldn't find peer to notify about completed exe launch %s", msg->command_path));
			}

			RL_FREE_TYPED(launch_msg_t, msg);
		}
#endif

		if (signal_mask & SIGBREAKF_CTRL_C)
		{
			RL_LOG_INFO(("breaking on ctrl+c"));
			break;
		}

		else if (-1 == num_ready_fds)
		{
			RL_LOG_WARNING(("select() failed"));
			break;
		}

		if (FD_ISSET(server_fd, &read_fds))
		{
			RL_LOG_INFO(("new connection available"));
			new_peer = accept_peer(server_fd);
			if (!new_peer)
				continue;
			new_peer->next = peers;
			peers = new_peer;
			new_peer_socket = new_peer->fd;
		}
	
		{
			peer_t* ci;
			peer_t *prev = NULL, *next = NULL;

			for (ci = peers; ci; )
			{
				const int can_read = (ci->fd == new_peer_socket) || FD_ISSET(ci->fd, &read_fds);
				const int can_write = (ci->fd == new_peer_socket) || FD_ISSET(ci->fd, &write_fds);
				int status;

				prev = next;
				next = ci->next;

				status = peer_update(ci, can_read, can_write);

				if (PEER_STATUS_REMOVE_ME & status)
				{
					if (prev)
						prev->next = next;
					else
						peers = next;

#if defined(__AMIGA__)
					if (ci->userdata)
						rl_amigafs_destroy((rl_amigafs_t *)ci->userdata);
#endif

					peer_destroy(ci);
					RL_FREE_TYPED(peer_t, ci);
				}

				ci = next;
			}
		}
	}

	{
		peer_t* ci;
		for (ci = peers; ci; )
		{
			peer_t *next = ci->next;

#if defined(__AMIGA__)
			if (ci->userdata)
				rl_amigafs_destroy((rl_amigafs_t *)ci->userdata);
#endif
			peer_destroy(ci);
			RL_FREE_TYPED(peer_t, ci);
			ci = next;
		}
	}
}

static void common_main(const char *bind_address, int bind_port)
{
	rl_socket_t listener_fd;
	struct sockaddr_in listen_address;

	RL_LOG_CONSOLE(("rlaunch target " RLAUNCH_VERSION));
   	RL_LOG_CONSOLE((RLAUNCH_LICENSE));

	RL_LOG_DEBUG(("common_main: bind_address:%s, bind_port:%d", bind_address, bind_port));

	listener_fd = socket(PF_INET, SOCK_STREAM, 0);

	{
		long value = 1;
		if (0 != setsockopt(listener_fd, SOL_SOCKET, SO_REUSEADDR, (const char*) &value, sizeof(value)))
		{
			RL_LOG_WARNING(("Couldn't enable SO_REUSEADDR"));
			goto cleanup;
		}
	}

	if (INVALID_SOCKET == listener_fd)
	{
		RL_LOG_CONSOLE(("Couldn't create main socket"));
		goto cleanup;
	}

	RL_LOG_DEBUG(("server socket created (fd=%d)", listener_fd));

	rl_memset(&listen_address, 0, sizeof(listen_address));
	listen_address.sin_family = AF_INET;
	listen_address.sin_port = htons((u_short)bind_port);
	listen_address.sin_addr.s_addr = inet_addr(bind_address);

	if (0 != bind(listener_fd, (struct sockaddr*)&listen_address, sizeof(listen_address)))
	{
		RL_LOG_CONSOLE(("bind() failed--is port %d busy?", bind_port));
		goto cleanup;
	}
	RL_LOG_DEBUG(("bind ok"));

	if (0 != listen(listener_fd, 2))
	{
		RL_LOG_CONSOLE(("listen() failed"));
		goto cleanup;
	}
	RL_LOG_DEBUG(("listen ok"));

	serve(listener_fd);
	RL_LOG_DEBUG(("server done"));

cleanup:
	if (INVALID_SOCKET != listener_fd)
		CloseSocket(listener_fd);
}

#if defined(WIN32)
static BOOL WINAPI ctrl_c_handler(DWORD ctrlType)
{
	if (CTRL_C_EVENT == ctrlType)
	{
		if (0 == sigbreak_occured)
			InterlockedIncrement(&sigbreak_occured);
		return TRUE;
	}
	else
		return FALSE;
}
#elif defined(RL_POSIX)
void ctrl_c_handler(int signum, siginfo_t *info, void *ignored)
{
	sigbreak_occured = SIGBREAKF_CTRL_C;
}
#endif

#ifdef __AMIGA__

int main(const char *args)
{
	int rv = RETURN_OK;
	struct Process *this_process;
	struct WBStartup *wbstartup_message = NULL;

	char bind_address[64] = "0.0.0.0";
	int bind_port = 7001;

	SysBase = *((struct ExecBase**) 4);

	this_process = (struct Process*) FindTask(NULL);

	if (!this_process->pr_CLI)
	{
		WaitPort(&(this_process->pr_MsgPort));
		wbstartup_message = (struct WBStartup*) GetMsg(&(this_process->pr_MsgPort));
	}

	if ((DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37)) == NULL)
		goto cleanup;

	if (this_process->pr_CLI)
	{
		LONG argument_values[3] = { 0l, 0l, 0l };
		struct RDArgs* args;

		if (NULL != (args = ReadArgs("ADDRESS,PORT/N,LOG", &argument_values[0], NULL)))
		{
			if (argument_values[0])
				rl_format_msg(bind_address, sizeof(bind_address), "%s", (const char*) argument_values[0]);
			if (argument_values[1])
				bind_port = *((LONG*)argument_values[1]);
			if (argument_values[2])
				rl_toggle_log_bits((const char *) argument_values[2]);
			FreeArgs(args);
		}
		else
		{
			PrintFault(IoErr(), "");
			goto panic;
		}
	}
	else
	{
		/* TODO: Implement workbench tooltip arguments */
	}

	if (wbstartup_message)
		RL_LOG_DEBUG(("started from workbench"));
	else
		RL_LOG_DEBUG(("started from CLI"));

	rl_init_socket();
	rl_init_alloc();

	/* Initialize message port for async spawn results */
	if (!(g_process_msg_port = CreateMsgPort()))
		goto cleanup;

	common_main(bind_address, bind_port);

cleanup:
	if (g_process_msg_port)
		DeleteMsgPort(g_process_msg_port);

	rl_fini_alloc();
	rl_fini_socket();

panic:
	if (DOSBase)
		CloseLibrary((struct Library*)DOSBase);

	if (wbstartup_message)
		ReplyMsg((struct Message*) wbstartup_message);

	return rv;
}
#else
int main(int argc, char** argv)
{
	int cleanup_alloc = 0;
	int cleanup_socket = 0;

#if defined(WIN32)
	SetConsoleCtrlHandler(ctrl_c_handler, TRUE);
#elif defined(RL_POSIX)
	{
		struct sigaction act;
		rl_memset(&act, 0, sizeof(act));
		act.sa_sigaction = ctrl_c_handler;
		sigaction(SIGINT, &act, NULL);
	}
#endif

	if (0 != rl_init_socket())
		goto cleanup;

	cleanup_socket = 1;

	if (0 != rl_init_alloc())
		goto cleanup;

	cleanup_alloc = 1;

	common_main("0.0.0.0", 7001);

cleanup:
	if (cleanup_alloc)
		rl_fini_alloc();
	if (cleanup_socket)
		rl_fini_socket();

	return 0;
}
#endif
