
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
#endif

static int on_connected(peer_t *peer)
{
	rl_msg_t msg;
	rl_msg_launch_executable_request_t *req = &msg.launch_executable_request;
	rl_controller_t *self = (rl_controller_t*) peer->userdata;

	RL_MSG_INIT(msg, RL_MSG_LAUNCH_EXECUTABLE_REQUEST);

	req->path = self->executable;
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
"USAGE:\n"
" rl-controller [-fsroot <directory>] [-port <port#>] [-log [a0][dniwc..]] <hostname> <path/to/executable>\n"
"\n"
"  <hostname>             Hostname to connect to (mandatory)\n"
"  <path/to/executable>   Path to executable relative to fsroot, with forward slashes. (mandatory)\n"
"\n"
"  Optional parameters:\n"
"  -fsroot                Specifies the file serving directory. The executable\n"
"                         must live in this directory as well. (default: cwd)\n"
"  -port                  The TCP port to connect to on the remote target (default: 7001)\n"
"  -log                   Specifies log levels (default: 'c')\n"
"                         0: disable everything\n"
"                         a: enable everything\n"
"                         d: enable debug channel\n"
"                         i: enable info channel\n"
"                         w: enable warning channel\n"
"                         p: enable network packet channel\n"
"                         c: enable console channel\n";

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
		int i;
		for (i = 1; i < argc; ++i)
		{
			const char *this_arg = argv[i];
			const char *next_arg = (i+1) < argc ? argv[i+1] : "";

			if (0 == strcmp("-fsroot", this_arg))
			{
				++i;
				fsroot = next_arg;
			}
			else if (0 == strcmp("-port", this_arg))
			{
				++i;
				peer_port = next_arg;
			}
			else if (0 == strcmp("-log", this_arg))
			{
				++i;
				rl_toggle_log_bits(next_arg);
			}
			else if (!peer_hostname)
			{
				peer_hostname = this_arg;
			}
			else if (!ctrl.executable)
			{
				ctrl.executable = this_arg;
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

#ifdef WIN32
	if (0 != rl_init_socket())
		goto cleanup;
	sockets_initialized = 1;
#endif

	ctrl.state = CONTROLLER_INITIAL;
	ctrl.root_handle.type = RL_NODE_TYPE_DIRECTORY;
	rl_string_copy(sizeof(ctrl.root_handle.native_path), ctrl.root_handle.native_path, fsroot);

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
