
#include "util.h"
#include "peer.h"
#include "protocol.h"
#include "rlnet.h"
#include "socket_includes.h"

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
struct ExecBase *SysBase = 0;
#else
extern struct ExecBase *SysBase;
#endif
struct DosLibrary *DOSBase = 0;

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
struct launch_msg_tag
{
	char path[128];
	struct FileLock *root_lock;
} launch_msg;

static void cmd_launcher(void)
{
	struct TagItem system_tags[] = {
		{ NP_CurrentDir, (Tag) NULL }, /* filled in below */
		{ SYS_Input, (Tag) NULL },
		{ SYS_Output, (Tag) NULL },
		{ SYS_Asynch, TRUE },
		{ SYS_UserShell, TRUE },
		{ TAG_DONE, 0 }
	};

	system_tags[0].ti_Data = (Tag) MKBADDR(launch_msg.root_lock);

	if (0 != SystemTagList(launch_msg.path, &system_tags[0]))
		UnLock(MKBADDR(launch_msg.root_lock));
}

static void async_spawn(peer_t *peer, const char *cmd)
{
	rl_amigafs_t * const fs = (rl_amigafs_t *) peer->userdata;

	struct TagItem launch_tags[] =
	{
		NP_Entry,				(ULONG)cmd_launcher,
		NP_StackSize,			8000,
		NP_Name,				(ULONG)"Process Launcher",
		NP_Cli,					TRUE,
		NP_Input,				(Tag) NULL,
		NP_Output,				(Tag) NULL,
		NP_CloseInput,			FALSE,
		NP_CloseOutput,			FALSE,
		TAG_DONE,				0
	};

	struct Process *launcher_proc;

	rl_format_msg(launch_msg.path, sizeof(launch_msg.path), "%s", cmd);
	launch_msg.root_lock = rl_amigafs_alloc_root_lock(fs, SHARED_LOCK);

	if (!(launcher_proc = CreateNewProc(&launch_tags[0])))
	{
		rl_amigafs_free_lock(fs, launch_msg.root_lock);
		RL_LOG_WARNING(("Couldn't kick launcher thread"));
		return;
	}
}
#endif


static int on_launch_executable_request(peer_t *peer, const rl_msg_t *msg)
{
	rl_msg_t answer;

#if defined(__AMIGA__)
	char exe_path[108];

	/* Start the executable in the network device root */
	rl_format_msg(exe_path, sizeof(exe_path), "TBL0:%s", msg->launch_executable_request.path);

	RL_LOG_INFO(("launch executable: '%s'", exe_path));
	async_spawn(peer, exe_path);

#endif

	RL_MSG_INIT(answer, RL_MSG_LAUNCH_EXECUTABLE_ANSWER);
	answer.launch_executable_answer.hdr_in_reply_to = msg->launch_executable_request.hdr_sequence_num;
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
	if (0 != rl_amigafs_init(amifs, peer, "TBL0"))
	{
		peer_destroy(peer);
		RL_LOG_WARNING(("couldn't init amiga fs"));
		goto error_cleanup;
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

	rl_log_message("rl-controller v0.95 (c) 2009 Andreas Fredriksson, TBL Technologies");

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

	common_main(bind_address, bind_port);

cleanup:
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
