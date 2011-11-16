#ifndef RLAUNCH_CONFIG_H
#define RLAUNCH_CONFIG_H

#if defined(__AMIGA__)
#define RL_AMIGA 1
#define BIG_ENDIAN 1
#define NATIVE_PATH_TERMINATOR '/'
#elif defined(__APPLE__)
#define RL_POSIX 1
#define RL_APPLE 1
#define NATIVE_PATH_TERMINATOR '/'
#elif defined(linux)
#define RL_POSIX 1
#define RL_LINUX 1
#define NATIVE_PATH_TERMINATOR '/'
#elif defined(WIN32)
#define RL_WIN32 1
#define NATIVE_PATH_TERMINATOR '\\'
#endif


#endif
