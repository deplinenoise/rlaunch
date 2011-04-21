
#include "config.h"
#include "util.h"
#include "peer.h"
#include "rlnet.h"
#include "socket_includes.h"
#include "controller.h"
#include "version.h"

#include <stdio.h>
#include <string.h>

#ifdef RL_POSIX
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#endif

static int on_connected(peer_t *peer)
{
	char arguments[256];
	rl_msg_t msg;
	rl_msg_launch_executable_request_t *req = &msg.launch_executable_request;
	rl_controller_t *self = (rl_controller_t*) peer->userdata;

	RL_MSG_INIT(msg, RL_MSG_LAUNCH_EXECUTABLE_REQUEST);

	/* Format arguments string. */
	{
		char* p = arguments;
		char* p_max = arguments + sizeof(arguments) - 1;
		int i=0;
		for (i=0; i<self->arg_count && p < p_max; ++i)
		{
			RL_ASSERT(self->arguments[i]);
			p += rl_format_msg(p, p_max - p, i > 0 ? " %s" : "%s", self->arguments[i]);
		}
		*p = '\0';
	}

	req->path = self->executable;
	req->arguments = arguments;
	if (0 == peer_transmit_message(peer, &msg))
	{
		self->state = CONTROLLER_WAIT_EXECUTABLE_LAUNCHED;
		return 0;
	}
	else
	{
		self->state = CONTROLLER_ERROR;
		return 1;
	}
}

static int on_message_received(peer_t *peer, const rl_msg_t *msg)
{
	rl_controller_t *self = (rl_controller_t*) peer->userdata;
	switch (self->state)
	{
		case CONTROLLER_FILE_SERVING:
		case CONTROLLER_WAIT_EXECUTABLE_LAUNCHED:
		{
			if (RL_MSG_LAUNCH_EXECUTABLE_ANSWER == rl_msg_kind_of(msg))
			{
				self->state = CONTROLLER_FILE_SERVING;
			}
			else if (RL_MSG_EXECUTABLE_DONE_REQUEST == rl_msg_kind_of(msg))
			{
				RL_LOG_INFO(("executable completed with rc=%d", msg->executable_done_request.result_code));
				return 1;
			}
			else
			{
				rl_file_serve(peer, msg);
			}
			break;
		}

		default:
		{
			self->state = CONTROLLER_ERROR;
			break;
		}
	}
	RL_LOG_NETWORK(("on_message_received"));
	return 0;
}

static const peer_callbacks_t controller_callbacks = { on_message_received, on_connected };

static peer_t *connect_to_target(const char* machine, const char* port)
{
	rl_socket_t sock = INVALID_SOCKET;
	peer_t *this_peer = NULL;
	struct addrinfo hint;
	struct addrinfo *address_list = NULL, *addrp;

	rl_memset(&hint, 0, sizeof(hint));
	hint.ai_family = AF_INET;
	hint.ai_socktype = SOCK_STREAM;
	hint.ai_protocol = IPPROTO_TCP;

	if (0 != getaddrinfo(machine, port, &hint, &address_list))
	{
		RL_LOG_CONSOLE(("can't resolve %s:%s\n", machine, port));
		goto cleanup;
	}

	sock = (rl_socket_t) socket(PF_INET, SOCK_STREAM, 0);
	if (INVALID_SOCKET == sock)
	{
		RL_LOG_CONSOLE(("couldn't create socket\n"));
		goto cleanup;
	}

	for (addrp = address_list; addrp; addrp = addrp->ai_next)
	{
		if (0 == connect(sock, addrp->ai_addr, (int) addrp->ai_addrlen))
		{
			if (NULL == (this_peer = RL_ALLOC_TYPED_ZERO(peer_t)))
			{
				RL_LOG_CONSOLE(("out of memory allocating peer"));
				goto cleanup;
			}

			if (0 != peer_init(this_peer, sock, addrp->ai_addr, &controller_callbacks, PEER_INIT_CONTROLLER, NULL))
			{
				RL_LOG_CONSOLE(("Failed to init peer"));
				RL_FREE_TYPED(peer_t, this_peer);
				this_peer = NULL;
				goto cleanup;
			}
		}
	}

	if (!this_peer)
		RL_LOG_CONSOLE(("couldn't connect to any of the addresses for %s:%s, giving up\n", machine, port));

cleanup:
	if (NULL == this_peer && INVALID_SOCKET != sock)
		CloseSocket(sock);

	if (address_list)
		freeaddrinfo(address_list);

	return this_peer;
}

static const char * const usage_string =
"\nrlaunch controller v" RLAUNCH_VERSION
"\n" RLAUNCH_LICENSE
"\n\nA networked programming testing and development solution for the Amiga.\n"
"\n"
"Usage:\n"
" rl-controller [-fsroot <r>] [-port <#>] [-log <..>] <host> <exe_path> [args]\n"
"\n"
"Arguments:\n"
"  <host>         Hostname to connect to (mandatory)\n"
"\n"
"  <exe_path>     Path to executable relative to fsroot, with forward\n"
"                 slashes. Absolute Amiga paths can also be used to run\n"
"                 remote programs, e.g. c:info(mandatory)\n"
"\n"
"  [args]         Optional arguments to pass to Amiga executable.\n"
"\n"
"Options:\n"
"  -fsroot        Specifies the file serving directory. The executable\n"
"                 must live in this directory as well. (default: cwd)\n"
"\n"
"  -port          The TCP port to connect to (default: 7001)\n"
"\n"
"  -log           Specifies log levels (default: 'c')\n"
"                 0: disable everything    a: everything\n"
"                 d: debug channel         i: info channel\n"
"                 w: warning channel       p: network packet channel\n"
"                 c: console channel\n";

static int pump_peer_state_machine(peer_t *peer)
{
	int peer_status = 0;
	int first_update = 1;

    do
	{
		int select_status;
		int max_fd = 0;
		struct timeval timeout;
        fd_set input_set, output_set;

		FD_ZERO(&input_set);
		FD_ZERO(&output_set);

		FD_SET(peer->fd, &input_set);
		if (first_update)
		{
			FD_SET(peer->fd, &output_set);
			first_update = 0;
		}
		max_fd = (int) (peer->fd + 1);

		if (PEER_STATUS_NEED_OUTPUT & peer_status)
		{
			FD_SET(peer->fd, &output_set);
		}

		timeout.tv_sec = 1;
		timeout.tv_usec = 0;

		select_status = select(max_fd, &input_set, &output_set, NULL, &timeout);
		if (-1 == select_status)
			return -1;

		peer_status = peer_update(peer, FD_ISSET(peer->fd, &input_set), FD_ISSET(peer->fd, &output_set));
	}
	while (0 == (PEER_STATUS_REMOVE_ME & peer_status));

	return 0;
}

int main(int argc, char** argv)
{
	int sockets_initialized = 0;
	peer_t *peer = NULL;
	const char* peer_hostname = NULL;
	const char* peer_port = "7001";
	const char *fsroot = "";
	rl_controller_t ctrl;

	memset(&ctrl, 0, sizeof(ctrl));

	/* parse the command line options */
	{
		int options_done = 0;
		int i;
		for (i = 1; i < argc; ++i)
		{
			const char *this_arg = argv[i];
			const char *next_arg = (i+1) < argc ? argv[i+1] : "";

			if (!options_done && 0 == strcmp("-fsroot", this_arg))
			{
				++i;
				fsroot = next_arg;
			}
			else if (!options_done && 0 == strcmp("-port", this_arg))
			{
				++i;
				peer_port = next_arg;
			}
			else if (!options_done && 0 == strcmp("-log", this_arg))
			{
				++i;
				rl_toggle_log_bits(next_arg);
			}
			else if (!peer_hostname)
			{
				peer_hostname = this_arg;
				options_done = 1;
			}
			else if (!ctrl.executable)
			{
				ctrl.executable = this_arg;
				options_done = 1;
			}
			else if (options_done && ctrl.arg_count < sizeof(ctrl.arguments)/sizeof(ctrl.arguments[0]))
			{
				RL_LOG_DEBUG(("arg%d = %s", ctrl.arg_count, this_arg));
				ctrl.arguments[ctrl.arg_count++] = this_arg;
			}
			else
			{
				RL_LOG_CONSOLE(("%s", usage_string));
				goto cleanup;
			}
		}
	}

	if (!peer_hostname || !ctrl.executable)
	{
		RL_LOG_CONSOLE(("%s", usage_string));
		goto cleanup;
	}

#ifdef RL_WIN32
	if (0 != rl_init_socket())
		goto cleanup;
	sockets_initialized = 1;
#endif

	ctrl.state = CONTROLLER_INITIAL;
	ctrl.root_handle.type = RL_NODE_TYPE_DIRECTORY;

	if (fsroot[0])
	{
		rl_string_copy(sizeof(ctrl.root_handle.native_path), ctrl.root_handle.native_path, fsroot);
	}
	else
	{
#ifdef RL_WIN32
		GetCurrentDirectoryA(sizeof(ctrl.root_handle.native_path), ctrl.root_handle.native_path);
#else
		getcwd(ctrl.root_handle.native_path, sizeof(ctrl.root_handle.native_path));
#endif
	}

	/* Set up virtual input/output files. */
	ctrl.vinput_handle.type = RL_NODE_TYPE_FILE;
	ctrl.voutput_handle.type = RL_NODE_TYPE_FILE;
	rl_string_copy(sizeof(ctrl.vinput_handle.native_path), ctrl.vinput_handle.native_path, "(virtual input)");
	rl_string_copy(sizeof(ctrl.voutput_handle.native_path), ctrl.voutput_handle.native_path, "(virtual output)");
#ifdef RL_WIN32
	ctrl.vinput_handle.handle = GetStdHandle(STD_INPUT_HANDLE);
	ctrl.voutput_handle.handle = GetStdHandle(STD_OUTPUT_HANDLE);
#else
	ctrl.vinput_handle.handle = 0;
	ctrl.voutput_handle.handle = 1;
#endif

	/* establish a connection */
	if (NULL == (peer = connect_to_target(peer_hostname, peer_port)))
		goto cleanup;

	peer->userdata = &ctrl;

	pump_peer_state_machine(peer);

cleanup:
	if (peer)
	{
		peer_destroy(peer);
		RL_FREE_TYPED(peer_t, peer);
	}
	if (sockets_initialized)
		rl_fini_socket();
	return 0;
}
