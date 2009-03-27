#include "protocol.h"
#include "util.h"
#include <string.h>

int rl_decode_string(const unsigned char **cursor, int *size, const char **result)
{
	int my_len;

	if (*size < 2)
		return -1;

	my_len = (unsigned int) **cursor;
	
	if (my_len >= *size)
		return -1;

	if ((*cursor)[1 + my_len])
		return -1;

	*result = (const char*) (*cursor + 1);
	*size -= my_len + 2;
	*cursor += my_len + 2;
	return 0;
}


int rl_encode_string(unsigned char **cursor, int *size, const char *string)
{
	const size_t string_len = rl_strlen(string);

	if (*size < 2)
		return -1;

	if ((int) (string_len + 2) > *size)
		return -1;

	if (string_len > 255)
		return -1;

	**cursor = (unsigned char) string_len;

	rl_memcpy(*cursor+1, string, string_len + 1);
	*size -= (int) string_len + 2;
	*cursor += string_len + 2;
	return 0;
}

int rl_decode_array(const unsigned char **cursor, int *size, rl_net_array_t *result)
{
	if (*size < 4)
		return -1;

	if (0 != rl_decode_int4(cursor, &result->length)) /* bumps cursor */
		return -1;

	if ((int) result->length > *size)
		return -1;

	result->base = (const rl_uint8*) (*cursor);
	*size -= result->length + 4;
	*cursor += result->length;
	return 0;
}


int rl_encode_array(unsigned char **cursor, int *size, const rl_net_array_t array)
{
	if (*size < (int)(4 + array.length))
		return -1;

	rl_encode_int4(cursor, array.length);
	memcpy(*cursor, array.base, array.length);
	*cursor += array.length;
	*size -= (int)(4 + array.length);
	return 0;
}

