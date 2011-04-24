#ifndef RLAUNCH_CONFIG_H
#define RLAUNCH_CONFIG_H

#if defined(__AMIGA__)
#define RL_AMIGA 1
#define BIG_ENDIAN 1
#elif defined(__APPLE__)
#define RL_POSIX 1
#define RL_APPLE 1
#elif defined(linux)
#define RL_POSIX 1
#define RL_LINUX 1
#elif defined(WIN32)
#define RL_WIN32 1
#endif


#endif
