#ifndef RL_SOCKET_TYPES_H
#define RL_SOCKET_TYPES_H

#include <stddef.h>

#ifdef __AMIGA__
#include <exec/types.h>
#endif

/* Establish a type for sockets. WinSock uses SOCKET, POSIX uses int, and the
 * Amiga bsdsocket.library uses LONG. Interestingly, SOCKET is a typedef to
 * HANDLE on Win32, which means it will be 64 bits wide. */

#if defined(__AMIGA__)
typedef ULONG rl_socket_t;
typedef LONG socklen_t;
#elif defined(WIN32)
typedef size_t rl_socket_t;
#elif defined(RL_POSIX)
typedef int rl_socket_t;
#endif

/* Use INVALID_SOCKET as an idiom even on non-Win32 */

#if !defined(WIN32)
#define INVALID_SOCKET ((rl_socket_t)-1)
#endif /* !WIN32 */


#endif
