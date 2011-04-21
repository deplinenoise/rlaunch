#ifndef RLAUNCH_UTIL_H
#define RLAUNCH_UTIL_H

#include "config.h"

#include <stddef.h>

/*
 * Come up with something that will pass for "inline" on the current compiler
 */
#if defined(_MSC_VER)
# define INLINE __forceinline
#elif defined(SASC)
# define INLINE inline
#elif defined(__VBCC__)
# define INLINE
#elif defined(__GNUC__)
# define INLINE __inline__
#else
# error need inline keyword for this compiler
#endif

/*
 * Static assertion macro.
 */
#define RL_STATIC_ASSERT(name, expr) \
	struct static_assertion_failed_##name { char test[ (expr) ? 1 : -1 ]; }

/*
 * Set up some fixed-size typedefs.
 */
#if defined(RL_AMIGA)
typedef unsigned char		rl_uint8;
typedef unsigned short		rl_uint16;
typedef unsigned int		rl_uint32;
#elif defined(RL_WIN32)
typedef unsigned char		rl_uint8;
typedef unsigned short		rl_uint16;
typedef unsigned int		rl_uint32;
#elif defined(RL_POSIX)
#include <stdint.h>
typedef uint8_t				rl_uint8;
typedef uint16_t			rl_uint16;
typedef uint32_t			rl_uint32;
#else
#error "Don't know what platform this is."
#endif

/* Make sure our typedefs are the size they should be */
RL_STATIC_ASSERT(rl_uint8_is_size_1, sizeof(rl_uint8) == 1);
RL_STATIC_ASSERT(rl_uint16_is_size_2, sizeof(rl_uint16) == 2);
RL_STATIC_ASSERT(rl_uint32_is_size_4, sizeof(rl_uint32) == 4);

typedef struct rl_net_array_tag {
	const rl_uint8 *base;
	rl_uint32 length;
} rl_net_array_t;

/*
 * A simple, portable printf-like formatting engine implementation. The
 * resulting buffer is always null terminated.
 *
 * It supports the following specifications:
 *
 * %{align}{zero}{width}{type}
 *
 * align: if '-' is specified, the value is written left aligned within the field width
 * zero: if '0' is specified, the fill character (padding) is '0' instead of a space
 * width: the minumum number of characters to write; any gap is to be filled with the fill character
 *
 * The following formatting types are available:
 *
 * d: Format a signed base-10 integer
 * u: Format an unsigned base-10 integer
 * x: Format an unsigned hexadecimal (base-16) integer
 * b: Format an unsigned binary (base-2) integer
 * c: Format a single character
 * s: Format a string
 * p: Format a pointer value
 * B: Format a BCPL style-string (Amiga only)
 * Q: Format a BCPL style-string as used in AmigaDOS (address/4) (Amiga only)
 * %: Emit '%'
 */
size_t rl_format_msg(char *buffer, size_t buffer_size, const char *fmt, ...);

void rl_log_message(const char *fmt, ...);
void rl_dump_buffer(const void *ptr, size_t size);

enum rl_log_bit_tag
{
	RL_DEBUG		= 1,
	RL_NETWORK		= 2,
	RL_INFO			= 4,
	RL_WARNING		= 8,
	RL_CONSOLE		= 16,
	RL_PACKET		= 32,
	RL_ALL_LOG_BITS = RL_DEBUG | RL_NETWORK | RL_INFO | RL_WARNING | RL_CONSOLE | RL_PACKET
};

extern int rl_log_bits;

/* parse characters and flip the corresponding bits in rl_log_bits according to rl_log_bit_tag
   d - RL_DEBUG
   n - RL_NETWORK
   i - RL_INFO
   w - RL_WARNING
   c - RL_CONSOLE
   p - RL_PACKET
   -- also
   0 - nothing
   a - everything
 */
void rl_toggle_log_bits(const char *argument);

#if 1
#define RL_LOG(level, expr) do { if (rl_log_bits & level) rl_log_message expr ; } while (0)
#else
#define RL_LOG(level, expr) do { } while (0)
#endif

#define RL_LOG_DEBUG(expr) RL_LOG(RL_DEBUG, expr)
#define RL_LOG_NETWORK(expr) RL_LOG(RL_NETWORK, expr)
#define RL_LOG_INFO(expr) RL_LOG(RL_INFO, expr)
#define RL_LOG_WARNING(expr) RL_LOG(RL_WARNING, expr)
#define RL_LOG_CONSOLE(expr) RL_LOG(RL_CONSOLE, expr)

/*
 * Utility macros
 */

#define RL_MIN_MACRO(a, b) ((a) < (b) ? (a) : (b))
#define RL_MAX_MACRO(a, b) ((a) > (b) ? (a) : (b))

/*
 * Dynamic memory allocation
 */

#define RL_ALLOC_TYPED(t)			((t*) rl_alloc_sized(sizeof(t)))
#define RL_ALLOC_TYPED_ZERO(t)		((t*) rl_alloc_sized_and_clear(sizeof(t)))
#define RL_FREE_TYPED(t, ptr)		(rl_free_sized(ptr, sizeof(t)))

int rl_init_alloc(void);
void rl_fini_alloc(void);

void *rl_alloc_sized(size_t sz);
void *rl_alloc_sized_and_clear(size_t sz);
void rl_free_sized(void *ptr, size_t sz);

/*
 * Endian support
 */

#ifndef BIG_ENDIAN
void byte_swap2(void *ptr);
void byte_swap4(void *ptr);
#endif

/*
 * C library selection (implementation)
 */

static INLINE char rl_isdigit(char ch)
{
	return ch >= '0' && ch <= '9';
}

static INLINE int rl_char_int_value(char ch)
{
	return (int) ch - (int) '0';
}

int rl_string_copy(size_t dest_size, char *dest, const char *source);

#if defined(NO_C_LIB)

size_t rl_strlen(const char* str);
void rl_memmove(void *dest, const void *src, size_t len);
void rl_memcpy(void *dest, const void *src, size_t len);
void rl_memset(void *dest, int value, size_t len);
long rl_time(const void* ignored);
void rl_abort(void);
const char* rl_strchr(const char *input, char ch);
int rl_strcmp(const char *lhs, const char* rhs);

#ifdef NDEBUG
#define RL_ASSERT(x) do { } while(0)
#else
#define RL_ASSERT(x) do { if (!(x)) { RL_LOG_CONSOLE(("%s:%d: assertion failed: %s", __FILE__, __LINE__, #x)); rl_abort(); } } while(0)
#endif

#else /* NO_C_LIB */

/* Use the C library */

#include <assert.h>
#include <string.h>
#define rl_strcmp strcmp
#define rl_strlen strlen
#define rl_memcpy memcpy
#define rl_memset memset
#define rl_memmove memmove
#define rl_time time
#define rl_strchr strchr
#define RL_ASSERT(x) assert(x)
#endif

/*
 * Data structures
 */

struct rl_dict_bucket_tag;

typedef int (*rl_compare_fn)(const void *lhs, const void *rhs);
typedef rl_uint32 (*rl_hash_fn)(const void *datum, size_t len);
typedef void (*rl_destructor_fn)(void *datum);

typedef struct rl_dict_tag
{
	struct rl_dict_bucket_tag *buckets;
	size_t num_buckets;
	size_t num_elements;
	rl_hash_fn hash_fn;
	rl_compare_fn compare_fn;
	rl_destructor_fn destructor_fn;
} rl_dict_t;

int
rl_dict_init(rl_dict_t *dict, size_t buckets);

void
rl_dict_destroy(rl_dict_t *dict);

/* String utilities */

typedef struct rl_strbuf_tag
{
	char *buffer;
	char *max;
} rl_strbuf_t;

void rl_strbuf_init(rl_strbuf_t* buf, char *buffer, size_t size);
int rl_strbuf_append(rl_strbuf_t* buf, const char *str);
int rl_strbuf_append_str_len(rl_strbuf_t* buf, const char *str, size_t len);

#endif

