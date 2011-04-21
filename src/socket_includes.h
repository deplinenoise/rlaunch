#ifndef RL_SOCKET_INCLUDES_H
#define RL_SOCKET_INCLUDES_H

#include "config.h"
#include "socket_types.h"

#if defined(RL_AMIGA)
#include <proto/socket.h>
#include <sys/errno.h>
#include <sys/ioctl.h>
#endif


#if defined(RL_AMIGA) || defined(RL_POSIX)
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif

#if defined(RL_WIN32)
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#if defined(RL_POSIX)
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
#endif


#if defined(RL_AMIGA)
# define RL_LAST_SOCKET_ERROR Errno()

#elif defined(RL_WIN32)
#define CloseSocket closesocket
#define RL_LAST_SOCKET_ERROR WSAGetLastError()
#define EWOULDBLOCK WSAEWOULDBLOCK

#elif defined(RL_POSIX)
# define CloseSocket close
# define RL_LAST_SOCKET_ERROR errno
#endif


/*!
 * \brief Configure blocking on the specified socket.
 * \param s The socket to configure blocking on.
 * \param should_block If non-zero, the socket should be in blocking mode.
 * \return Non-zero on error. 
 */
int rl_configure_socket_blocking(rl_socket_t s, int should_block);

int rl_init_socket(void);

void rl_fini_socket(void);

#endif
