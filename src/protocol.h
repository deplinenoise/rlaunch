#ifndef RLAUNCH_PROTOCOL_H
#define RLAUNCH_PROTOCOL_H

#include "util.h"

#define RL_FILEHANDLE_VIRTUAL_INPUT (0x7ffffffe)
#define RL_FILEHANDLE_VIRTUAL_OUTPUT (0x7ffffffd)

enum
{
	RL_PROTO_HDRF_REQUEST		= 1 << 0,
	RL_PROTO_HDRF_ERROR			= 1 << 1
};

enum
{
	RL_OPENFLAG_READ			= 1 << 0,
	RL_OPENFLAG_WRITE			= 1 << 1,
	RL_OPENFLAG_CREATE			= 1 << 2
};

typedef enum rl_node_type_tag
{
	RL_NODE_TYPE_FILE			= 1,
	RL_NODE_TYPE_DIRECTORY		= 2
} rl_node_type_t;

typedef enum rl_proto_neterror_tag {
	RL_NETERR_SUCCESS				= 0,
	RL_NETERR_ACCESS_DENIED			= 1,
	RL_NETERR_NOT_FOUND				= 2,
	RL_NETERR_NOT_A_FILE			= 3,
	RL_NETERR_NOT_A_DIRECTORY		= 4,
	RL_NETERR_IO_ERROR				= 5,
	RL_NETERR_INVALID_VALUE			= 6,
	RL_NETERR_BAD_REQUEST			= 128,
	RL_NETERR_TOO_MANY_FILES_OPEN	= 129,
	RL_NETERR_SPAWN_FAILURE			= 254,
	RL_NETERR_UNKNOWN				= 255 
} rl_proto_neterror_t;

#define RL_MSG_INIT(msg, kind) \
do { \
	rl_memset(&(msg), 0, sizeof(msg));		\
	(msg).ping_request.hdr_type		= kind;	\
} while(0)

/*
 * Strings use the following encoding:
 * length: one byte (implies that the maximum string length is 255 bytes, excluding null termination)
 * payload: [length] bytes
 * null: a null byte (rl_decode_string verifies that it is there)
 *
 * This is useful because it allows client code to safely use the network
 * buffer as a backing store for incoming strings. rl_decode_string() verifies
 * that all this is safe.
 */

/*
 * Decode a string stored at [cursor], storing a pointer into the buffer into
 * [*result] if successful. [*cursor] and [*size] are updated to reflect the
 * remaining buffer space and the position of the next datum.
 *
 * Returns nonzero on error.
 */
int rl_decode_string(const unsigned char **cursor, int *size, const char **result);

/*
 * Encode a string into [cursor]. [*cursor] and [*size] are updated to reflect the
 * remaining buffer space and the position of the next possible encoding.
 *
 * Returns nonzero on error.
 */
int rl_encode_string(unsigned char **cursor, int *size, const char *string);

/* decoding helpers */
static INLINE void rl_decode_int1(const unsigned char **cursor, rl_uint8 *result)
{
	*result = **cursor;
	*cursor += 1;
}

static INLINE void rl_decode_int2(const unsigned char **cursor, rl_uint16 *result)
{
	rl_uint8* dest = (rl_uint8*) result;

#ifdef BIG_ENDIAN
	dest[0] = (*cursor)[0];
	dest[1] = (*cursor)[1];
#else
	dest[0] = (*cursor)[1];
	dest[1] = (*cursor)[0];
#endif

	*cursor += 2;
}

static INLINE int rl_decode_int4(const unsigned char **cursor, rl_uint32 *result)
{
	rl_uint8* dest = (rl_uint8*) result;

#ifdef BIG_ENDIAN
	dest[0] = (*cursor)[0];
	dest[1] = (*cursor)[1];
	dest[2] = (*cursor)[2];
	dest[3] = (*cursor)[3];
#else
	dest[0] = (*cursor)[3];
	dest[1] = (*cursor)[2];
	dest[2] = (*cursor)[1];
	dest[3] = (*cursor)[0];
#endif

	*cursor += 4;
	return 0;
}

static INLINE void rl_encode_int1(unsigned char **cursor, rl_uint8 v)
{
	**cursor = v;
	++*cursor;
}

static INLINE void rl_encode_int2(unsigned char **cursor, rl_uint16 v)
{
	const rl_uint8 *source = (rl_uint8*) &v;

#ifdef BIG_ENDIAN
	(*cursor)[0] = source[0];
	(*cursor)[1] = source[1];
#else
	(*cursor)[1] = source[0];
	(*cursor)[0] = source[1];
#endif

	*cursor += 2;
}

static INLINE void rl_encode_int4(unsigned char **cursor, rl_uint32 v)
{
	const rl_uint8 *source = (rl_uint8*) &v;

#ifdef BIG_ENDIAN
	(*cursor)[0] = source[0];
	(*cursor)[1] = source[1];
	(*cursor)[2] = source[2];
	(*cursor)[3] = source[3];
#else
	(*cursor)[3] = source[0];
	(*cursor)[2] = source[1];
	(*cursor)[1] = source[2];
	(*cursor)[0] = source[3];
#endif

	*cursor += 4;
}

int rl_encode_array(unsigned char **cursor, int *size, const rl_net_array_t array);

int rl_decode_array(const unsigned char **cursor, int *size, rl_net_array_t *result);


#endif
