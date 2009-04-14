#include "util.h"

#include <stdarg.h>
#include <stddef.h>

#if defined(WIN32)
typedef __int64 ssize_t;
#elif defined(__AMIGA__)
typedef long ssize_t;
#endif

#ifndef NO_C_LIB
#include <stdio.h>
#include <stdlib.h>
#endif

#include "socket_includes.h"

#ifdef RL_POSIX
#include <unistd.h>
#include <fcntl.h>
#endif

#ifdef __AMIGA__
#include <proto/exec.h>
#include <proto/dos.h>
#include <exec/types.h>
#include <exec/memory.h>
#endif

typedef void (*format_write_func)(const char *start, size_t amount, void *state);

static void format_integer_signed(
		ssize_t value,
		int base,
		int falign_left,
		int fwidth,
		char fill,
		format_write_func wf,
		void *writer_state);

static void format_integer_unsigned(
		size_t value,
		int base,
		int falign_left,
		int fwidth,
		char fill,
		format_write_func wf,
		void *writer_state);

static void format_string(
		const char* value,
		size_t length,
		int falign_left,
		int fwidth,
		char fill,
		format_write_func wf,
		void *writer_state);

static void format_message(const char *format, va_list args, format_write_func wf, void *writer_state)
{
	const char *cursor = format, *fmt_pos = NULL;

	/* Scan forward looking for the format control character '%'. */
	while (NULL != (fmt_pos = rl_strchr(cursor, '%')))
	{
		char fill = ' ';
		int fwidth = 0;
		int falign_left = 0;

		/* Anything leading up to the percent sign can be output immediately as
		 * it is not being formatted. */
		(*wf)(cursor, (size_t) (fmt_pos - cursor), writer_state);

		/* Position the cursor right after the percent sign. */
		cursor = fmt_pos + 1;

		/* Parse the formatting code after the percent sign. */

		/* A minus immediately after the percent indicates that we're going to left align. */
		if ('-' == *cursor)
		{
			falign_left = 1;
			++cursor;
		}

		if ('0' == *cursor)
		{
			fill = '0';
			++cursor;
		}

		/* Numbers indicate the field width */
		while (rl_isdigit(*cursor))
		{
			fwidth *= 10;
			fwidth += rl_char_int_value(*cursor);
			++cursor;
		}

		switch (*cursor++)
		{
			case 'd':
			{
				ssize_t value = va_arg(args, int);
				format_integer_signed(value, 10, falign_left, fwidth, fill, wf, writer_state);
				break;
			}

			case 'u':
			{
				size_t value = va_arg(args, unsigned int);
				format_integer_unsigned(value, 10, falign_left, fwidth, fill, wf, writer_state);
				break;
			}

			case 'x':
			{
				size_t value = va_arg(args, int);
				format_integer_unsigned(value, 16, falign_left, fwidth, fill, wf, writer_state);
				break;
			}

			case 'b':
			{
				size_t value = va_arg(args, int);
				format_integer_unsigned(value, 2, falign_left, fwidth, fill, wf, writer_state);
				break;
			}

			case 's':
			{
				const char *value = va_arg(args, const char *);
				format_string(value, rl_strlen(value), falign_left, fwidth, fill, wf, writer_state);
				break;
			}

#ifdef __AMIGA__
			case 'B':
			{
				long value = va_arg(args, long);
				const unsigned char *bstr = (const unsigned char *)(value);
				const size_t length = *((const unsigned char*) bstr);
				format_string((const char*)(bstr+1), length, falign_left, fwidth, fill, wf, writer_state);
				break;
			}

			case 'Q':
			{
				long value = va_arg(args, long);
				const unsigned char *bstr = (const unsigned char *)(value << 2);
				const size_t length = *bstr;
				format_string((const char*)(bstr+1), length, falign_left, fwidth, fill, wf, writer_state);
				break;
			}
#endif

			case 'c':
			{
				char buffer[2] = { 0, 0 };
				buffer[0] = (char) va_arg(args, int);
				format_string(&buffer[0], 1, falign_left, fwidth, fill, wf, writer_state);
				break;
			}

			case 'p':
			{
				const void *value = va_arg(args, const void *);
				fwidth = sizeof(void*) * 2;
				fill = '0';
				(*wf)("<", 1, writer_state);
				format_integer_unsigned((size_t)value, 16, falign_left, fwidth, fill, wf, writer_state);
				(*wf)(">", 1, writer_state);
				break;
			}

			case '%':
			{
				/* A simple escape of the formatting character */
				(*wf)("%", 1, writer_state);
				break;
			}
		}
	}

	/* Emit the rest of the string (after the last format control character). */
	(*wf)(cursor, rl_strlen(cursor), writer_state);
}

static const char format_base_digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";

static void format_integer_signed(ssize_t value, int base, int falign_left, int fwidth, char fill, format_write_func wf, void *writer_state)
{
	ssize_t v = value;
	char buffer[64];
	char *p = &buffer[63];
	*p-- = 0;

	if (v < 0)
		v = -v;

	do
	{
		*p-- = format_base_digits[v % base];
		v /= base;
	} while(v > 0);

	if (value < 0)
		*p-- = '-';

	format_string(&p[1], rl_strlen(&p[1]), falign_left, fwidth, fill, wf, writer_state);
}

static void format_integer_unsigned(size_t value, int base, int falign_left, int fwidth, char fill, format_write_func wf, void *writer_state)
{
	size_t v = value;
	char buffer[64];
	char *p = &buffer[63];
	*p-- = 0;

	do
	{
		*p-- = format_base_digits[v % base];
		v /= base;
	} while(v > 0);

	format_string(&p[1], rl_strlen(&p[1]), falign_left, fwidth, fill, wf, writer_state);
}

static void format_string(const char* value, size_t len, int falign_left, int fwidth, char fill, format_write_func wf, void *writer_state)
{
	int padding = fwidth - (int) len;

	if (falign_left)
	{
		(*wf)(value, len, writer_state);
		while (padding-- > 0)
			(*wf)(&fill, 1, writer_state);
	}
	else
	{
		while (padding-- > 0)
			(*wf)(&fill, 1, writer_state);
		(*wf)(value, len, writer_state);
	}
}

#ifdef __VBCC__
#pragma dontwarn 79
#endif

int rl_log_bits = RL_CONSOLE;

void rl_toggle_log_bits(const char *argument)
{
	char c;
	while (0 != (c = *argument++))
	{
		int bit = 0;

		switch (c)
		{
			case 'a': rl_log_bits = RL_ALL_LOG_BITS; break;
			case '0': rl_log_bits = 0; break;
			case 'd': bit = RL_DEBUG; break;
			case 'n': bit = RL_NETWORK; break;
			case 'i': bit = RL_INFO; break;
			case 'w': bit = RL_WARNING; break;
			case 'c': bit = RL_CONSOLE; break;
			case 'p': bit = RL_PACKET; break;
			default:
				RL_LOG_CONSOLE(("Invalid log bit: %c", c));
				break;
		}

		rl_log_bits ^= bit;
	}
}

static char log_buffer[256];
static char *log_cursor = &log_buffer[0];
static char * const log_max = &log_buffer[sizeof(log_buffer)-1];

static void log_flush()
{
	*log_cursor = 0;
#ifdef __AMIGA__
	if (DOSBase)
	{
		FPuts(Output(), log_buffer);
		Flush(Output());
		/* Delay(20); */
	}
#else
	fputs(log_buffer, stdout);
#endif
	log_cursor = &log_buffer[0];
}

static void write_log(const char *str, size_t len, void *state)
{
	size_t i;

	for (i=0; i<len; ++i)
	{
		const char ch = str[i];

		*log_cursor++ = ch;
		if (log_cursor == log_max || '\n' == ch)
		{
			log_flush();
		}
	}
}

static void do_log(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	format_message(fmt, args, write_log, NULL);
	va_end(args);
}

void rl_log_message(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	format_message(fmt, args, write_log, NULL);
	va_end(args);
	write_log("\n", 1, NULL);
}

void rl_dump_buffer(const void *ptr, size_t size)
{
	size_t i;
	const rl_uint8 *p = (const rl_uint8*) ptr;

	for (i=0; i<size; ++i)
	{
		if ((i % 8) == 0)
			do_log("\n%08x   ", (int) i);

		do_log("%02x ", p[i]);
	}
	do_log("\n");
}

typedef struct safe_format_state_tag
{
	char *next_char;
	size_t space_left;
} safe_format_state_t;

static void safe_format_writer(const char *input, size_t amount, void *state_)
{
	safe_format_state_t *state = (safe_format_state_t*) state_;

	if (amount > state->space_left)
		amount = state->space_left;

	state->space_left -= amount;

	rl_memcpy(state->next_char, input, amount);
	state->next_char += amount;
}

size_t rl_format_msg(char *buffer, size_t buffer_size, const char *fmt, ...)
{
	va_list args;
	safe_format_state_t state;

	RL_ASSERT(buffer_size > 0);

	state.next_char = buffer;
	state.space_left = buffer_size - 1;

	va_start(args, fmt);
	format_message(fmt, args, safe_format_writer, &state);
	va_end(args);

	state.next_char[0] = '\0';
	return (size_t) (state.next_char - buffer);
}
#ifdef __VBCC__
#pragma popwarn
#endif

#if defined(__AMIGA__)

static void *rl_amiga_pool = 0;

int rl_init_alloc(void)
{
	rl_amiga_pool = CreatePool(MEMF_ANY|MEMF_PUBLIC, 8192, 1024);
	return rl_amiga_pool == 0;
}

void rl_fini_alloc(void)
{
	if (rl_amiga_pool)
		DeletePool(rl_amiga_pool);
}

void *rl_alloc_sized(size_t sz)
{
	void* result;
	result = AllocPooled(rl_amiga_pool, sz);
	RL_LOG_DEBUG(("rl_alloc_sized(%d) => %p", (int) sz, result));
	return result;
}

void *rl_alloc_sized_and_clear(size_t sz)
{
	void *memory = AllocPooled(rl_amiga_pool, sz);
	RL_LOG_DEBUG(("rl_alloc_sized_and_clear(%d) => %p", (int) sz, memory));
	if (memory)
		rl_memset(memory, 0, sz);
	return memory;
}

void rl_free_sized(void *ptr, size_t sz)
{
	RL_LOG_DEBUG(("rl_free_sized(%p, %d)", ptr, (int) sz));
	FreePooled(rl_amiga_pool, ptr, sz);
}

#else /* __AMIGA__ */

int rl_init_alloc(void) { return 0; }

void rl_fini_alloc(void) { }

void *rl_alloc_sized(size_t sz)
{
	void *p = malloc(sz);
	RL_LOG_DEBUG(("rl_alloc_sized(%d) => %p", sz, p));
	return p;
}

void *rl_alloc_sized_and_clear(size_t sz)
{
	return calloc(sz, 1);
}

void rl_free_sized(void *ptr, size_t sz)
{
	RL_LOG_DEBUG(("rl_free_sized(%p, %d)", ptr, (int) sz));
	free(ptr);
}
#endif

#ifndef BIG_ENDIAN
void byte_swap2(void *ptr_)
{
	unsigned char *ptr = (unsigned char *) ptr_;
	char tmp0 = ptr[0];
	ptr[0] = ptr[1];
	ptr[1] = tmp0;
}

void byte_swap4(void *ptr_)
{
	unsigned char *ptr = (unsigned char *) ptr_;
	char tmp0 = ptr[0];
	char tmp1 = ptr[1];
	ptr[0] = ptr[3];
	ptr[1] = ptr[2];
	ptr[2] = tmp1;
	ptr[3] = tmp0;
}
#endif /* !BIG_ENDIAN */

#if defined(NO_C_LIB)
void rl_memcpy(void *dest_, const void *src_, size_t len)
{
	char *dest = (char*) dest_;
	const char *src = (const char*) src_;
	while (len--)
	{
		*dest++ = *src++;
	}
}

void rl_memmove(void *dest_, const void *src_, size_t len)
{
	char *dst = (char *) dest_;
	const char *src = (const char *) src_;

	if (src < dst)
	{
		src += len;
		dst += len;
		while (len--)
			*--dst = *--src;
	}
	else if (dst < src)
	{
		while (len--)
			*dst++ = *src++;
	}
}

void rl_memset(void *dest_, int value, size_t len)
{
	char *dest = (void*) dest_;
	while (len--)
	{
		*dest++ = value;
	}
}

size_t rl_strlen(const char* str)
{
	size_t result = 0;
	while (*str++) ++result;
	return result;
}

const char* rl_strchr(const char *input, char ch)
{
	char test;
	do
	{
		test = *input;
		if (ch == test)
			return input;
		++input;
	} while (test);
	
	return NULL;
}

long rl_time(const void *ignored)
{
	return 0;
}

void rl_abort(void)
{
	for (;;) Delay(50);
}
#endif /* NO_C_LIB */

int rl_string_copy(size_t dest_size, char *dest, const char *source)
{
	char * const dest_max = dest + dest_size - 1;

	RL_ASSERT(dest_size > 1);

	while (dest != dest_max)
	{
		char ch = *source++;
		if (!ch)
			break;
		*dest++  = ch;
	}

	*dest = '\0';

	return dest == dest_max ? -1 : 0;
}

static INLINE void strbuf_append_ch(rl_strbuf_t *buf, char ch)
{
	if (buf->buffer != buf->max)
		*(buf->buffer)++ = ch;
}

static INLINE void strbuf_null_terminate(rl_strbuf_t *buf)
{
	if (buf->buffer != buf->max)
		*(buf->buffer) = '\0';
	else
		*(buf->max - 1) = '\0';
}

void rl_strbuf_init(rl_strbuf_t* buf, char *buffer, size_t size)
{
	buf->buffer = buffer;
	buf->max = buffer + size;
	strbuf_null_terminate(buf);
}

int rl_strbuf_append(rl_strbuf_t* buf, const char *str)
{
	char ch;
	while (0 != (ch = *str++))
	{
		strbuf_append_ch(buf, ch);
	}
	strbuf_null_terminate(buf);
	return buf->buffer != buf->max;
}

int rl_strbuf_append_str_len(rl_strbuf_t* buf, const char *str, size_t len)
{
	for (; len > 0; --len)
	{
		char ch = *str++;
		strbuf_append_ch(buf, ch);
	}
	strbuf_null_terminate(buf);
	return buf->buffer != buf->max;
}

