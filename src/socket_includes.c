#include "config.h"
#include "socket_includes.h"
#include "util.h"

#if defined(RL_AMIGA)
#include <proto/exec.h>
#include <proto/dos.h>
struct Library *SocketBase = 0;
#endif /* RL_AMIGA */

#if defined(RL_POSIX)
#include <fcntl.h>
#endif

#if defined(RL_WIN32)
static WSADATA s_wsa_data;
static int s_winsock_initialized = 0;
#endif /* RL_WIN32 */

int rl_init_socket(void)
{
#if defined(RL_WIN32)
	return WSAStartup(MAKEWORD(2, 0), &s_wsa_data);
#elif defined(RL_AMIGA)
	if ((SocketBase = OpenLibrary("bsdsocket.library", 4)) == NULL)
	{
		if (DOSBase)
			FPuts(Output(), "cannot open bsdsocket.library (version 4 or later)\n");
		return 1;
	}
	else
		return 0;
#else
	return 0;
#endif
}

void rl_fini_socket(void)
{
#if defined(RL_WIN32)
	WSACleanup();
#elif defined(RL_AMIGA)
	if (SocketBase)
		CloseLibrary(SocketBase);
#endif
}

int rl_configure_socket_blocking(rl_socket_t s, int should_block)
{
#if defined(RL_WIN32)
	u_long value = should_block ? 0 : 1;
	return ioctlsocket(s, FIONBIO, &value);
#elif defined(RL_AMIGA)
	unsigned long value = should_block ? 0 : 1;
	return IoctlSocket(s, FIONBIO, (char*) &value);
#else
	const int flags = fcntl(s, F_GETFL);
	if (should_block)
		return fcntl(s, flags & ~(O_NONBLOCK));
	else
		return fcntl(s, flags | (O_NONBLOCK));
#endif
}


