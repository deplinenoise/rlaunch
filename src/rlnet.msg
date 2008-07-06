
*/request
	.hdr_type				: byte
	.hdr_flags				: byte
	.hdr_length				: word
	.hdr_sequence_num		: longword

*/answer
	.hdr_type				: byte
	.hdr_flags				: byte
	.hdr_length				: word
	.hdr_in_reply_to		: longword

error/answer
	.error_code			: longword

ping/request
ping/answer

handshake/request
	.version_major		: byte
	.version_minor		: byte
	.node_name			: string
	.platform_name		: string
	.platform_version	: string
	.password_hash		: string

handshake/answer
	.version_major		: byte
	.version_minor		: byte
	.host_name			: string
	.platform_name		: string
	.platform_version	: string

open_handle/request
	.path				: string
	.mode				: longword

open_handle/answer
	.handle				: longword
	.size				: longword
	.type				: byte

close_handle/request
	.handle				: longword

read_file/request
	.handle				: longword
	.offset_hi			: longword
	.offset_lo			: longword
	.length				: longword

read_file/answer
	.data				: array

find_next_file/request
	.handle				: longword
	.reset				: byte

find_next_file/answer
	.end_of_sequence	: byte
	.type				: byte
	.name				: string
	.size				: longword

# controller->target requests

launch_executable/request
	.path				: string

launch_executable/answer
