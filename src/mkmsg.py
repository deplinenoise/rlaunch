#! /usr/bin/env python

import sys
import os.path
import re

# dot, field_name, colon, type, optional: guard expression in brackets
re_field = re.compile(r'^\s*\.(\w+)\s*:\s*(\w+)\s*(?:\[\s*(.*)\s*\])?$')

SOURCE_START = r'''

#include <stddef.h>
#include <string.h>
#include "protocol.h"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable:4189) /* local variable is initialized but not referenced */
#endif

static rl_msg_kind_t peek_msg_kind(const void *buffer, int size)
{
	rl_uint8 val;

	if (size < 4)
		return RL_MSG_BOGUS;

	val = ((const rl_uint8*)buffer)[0];

	if (val > RL_MSG_MAX)
		return RL_MSG_BOGUS;
	else
		return (rl_msg_kind_t) val;
}

'''

SOURCE_END = r'''
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
'''

def append_or_prepend(lst, value, append):
	if append:
		lst.append(value)
	else:
		lst.insert(0, value)

class Message:
	def __init__(self, name, type):
		self.name = name
		self.type = type
		self.all_fields = []
		self.fixed_fields = []
		self.var_fields = []
		self.fixed_size = 0

	def __insert_field(self, key, type, app, guard):
		v = (key, type, guard)
		if type.variable_length:
			append_or_prepend(self.var_fields, v, app)
		else:
			append_or_prepend(self.fixed_fields, v, app)
			if not guard:
				self.fixed_size += type.size
		append_or_prepend(self.all_fields, v, app)

	def prepend_field(self, key, type, guard):
		self.__insert_field(key, type, False, guard)

	def add_field(self, key, type, guard):
		self.__insert_field(key, type, True, guard)

class NetType:
	def __init__(self, c_name, name, size, variable_length=False):
		self.c_name = c_name
		self.name = name
		self.size = size
		self.variable_length = variable_length

types = {
		'string'	: NetType('const char *', 'string', -1, variable_length = True),
		'array'		: NetType('rl_net_array_t', 'array', -1, variable_length = True),
		'byte'		: NetType('rl_uint8', 'byte', 1),
		'word'		: NetType('rl_uint16', 'word', 2),
		'longword'	: NetType('rl_uint32', 'longword', 4)
}

def mkmsg(desc, output_prefix):
	current = None
	common_request = Message('*common*', 'request')
	common_answer = Message('*common*', 'answer')
	messages = []
	common_fields = []

	for line in desc:
		line = line.strip()

		if len(line) == 0 or line[0] == '#':
			continue

		if line.find('/') != -1:
			name, type = line.split('/')
			assert type in ('request', 'answer')
			if name == '*':
				if type == 'request':
					current = common_request
				else:
					current = common_answer
			else:
				current = Message(name, type)
				messages.append(current)
			continue

		key, type_name, guard = re_field.match(line).groups()
		if key and type_name:
			current.add_field(key, types[type_name], guard)
			continue
		else:
			raise Exception('illegal line: ' + line)

	# distribute common fields
	# reverse these lists; inserting them restores the order
	common_request.all_fields.reverse()
	common_answer.all_fields.reverse()
	for msg in messages:
		if msg.type == 'request':
			for k, t, g in common_request.all_fields:
				msg.prepend_field(k, t, g)
		else:
			for k, t, g in common_answer.all_fields:
				msg.prepend_field(k, t, g)

	header = open(output_prefix + '.h', 'w')
	header.write('#ifndef RL_PROTOCOL_AUTOGEN_H\n')
	header.write('#define RL_PROTOCOL_AUTOGEN_H\n')
	header.write('#include "util.h"\n\n')

	source = open(output_prefix + '.c', 'w')
	source.write('#include "%s.h"\n' % (os.path.basename(output_prefix)))
	source.write(SOURCE_START)

	header.write('typedef enum rl_msg_kind_tag {\n')
	for i in range(0, len(messages)):
		msg = messages[i]
		header.write('\tRL_MSG_%s_%s = 0x%x,\n' % (msg.name.upper(), msg.type.upper(), i))
	header.write('\tRL_MSG_MAX = %d,\n' % (len(messages)-1))
	header.write('\tRL_MSG_BOGUS = -1\n')
	header.write('} rl_msg_kind_t;\n\n')

	# public message structs
	for msg in messages:
		header.write('typedef struct rl_msg_%s_%s_tag {\n' % (msg.name, msg.type))
		for key, type, guard in msg.all_fields:
			header.write('\t%s %s;\n' % (type.c_name, key))
		header.write('} rl_msg_%s_%s_t;\n\n' % (msg.name, msg.type))

	header.write('typedef union rl_msg_tag {\n')
	for msg in messages:
		header.write('\trl_msg_%s_%s_t %s_%s;\n' % (msg.name, msg.type, msg.name, msg.type))
	header.write('} rl_msg_t;\n\n')

	# emit decoders, encoders and describers
	for msg in messages:
		ct_name = 'rl_msg_%s_%s_t' % (msg.name, msg.type)
		source.write('/*')
		source.write('-' * 70)
		source.write('*/\n\n')

		source.write('static int decode_%s_%s(const void *buffer_, int size, rl_msg_t *msg_out) {\n' % (msg.name, msg.type))
		source.write('\tconst unsigned char *buffer = (const unsigned char *)buffer_;\n')
		source.write('\t%s *target = &msg_out->%s_%s;\n' % (ct_name, msg.name, msg.type))
		source.write('\tif (size < %d) return -1;\n' % (msg.fixed_size))
		for name, type, guard in msg.fixed_fields:
			if guard:
				source.write('\tif (%s)\n\t' % (guard));
			source.write('\trl_decode_int%d(&buffer, &target->%s);\n' % (type.size, name))
		if len(msg.var_fields) > 0:
			source.write('\tsize -= %d;\n' % (msg.fixed_size))
			for name, type, guard in msg.var_fields:
				if guard:
					source.write('\tif (%s)\n\t' % (guard))
				source.write('\tif (0 != rl_decode_%s(&buffer, &size, &target->%s)) return -1;\n' % (type.name, name))
		
		source.write('\treturn 0;\n')
		source.write('}\n\n')

		source.write('static int encode_%s_%s(const rl_msg_t *msg, void *buffer_, int size) {\n' % (msg.name, msg.type))
		source.write('\tconst int initial_size = size;\n')
		source.write('\tconst %s *source = (const %s *)msg;\n' % (ct_name, ct_name))
		source.write('\tunsigned char *buffer = (unsigned char *)buffer_;\n')
		source.write('\tunsigned char *length_pos = NULL;\n')

		length_type = None

		for name, type, guard in msg.fixed_fields:
			if name == 'hdr_length':
				length_type = type
				source.write('\tlength_pos = buffer; buffer += %d;\n' % (type.size))
				continue

			if guard:
				source.write('\tif (%s)\n\t' % (guard))
			source.write('\trl_encode_int%d(&buffer, source->%s);\n' % (type.size, name))

		source.write('\tsize -= %d;\n' % (msg.fixed_size))
		for name, type, guard in msg.fixed_fields:
			if guard:
				source.write('\tif (%s) size -= sizeof(%d);\n' % (guard, type.size))

		for name, type, guard in msg.var_fields:
			source.write('\tif (0 != rl_encode_%s(&buffer, &size, source->%s)) return -1;\n' % (type.name, name))
		
		source.write('\trl_encode_int%d(&length_pos, (rl_uint%d)(initial_size - size));\n' % (length_type.size, length_type.size * 8))
		source.write('\treturn initial_size - size;\n')
		source.write('}\n\n')

		# emit describer
		source.write('static void describe_%s_%s(char *buffer, size_t buffer_max, const rl_msg_t *msg) {\n' % (msg.name, msg.type))
		source.write('\tconst %s *source = &msg->%s_%s;\n' % (ct_name, msg.name, msg.type))
		fmt = []
		arg = []

		fmt.append('%s/%s { ' % (msg.name, msg.type))

		for name, type, guard in msg.fixed_fields:
			fmt.append(name + '=%d ')
			arg.append('source->' + name)
			if guard:
				fmt.append('[G] ')

		fmt.append('| ')

		if len(msg.var_fields) > 0:
			for name, type, guard in msg.var_fields:
				fmt.append(name + '=')
				if type.name == 'string':
					fmt.append('\\"%s\\" ')
					arg.append('source->%s ? source->%s : "<null>"' % (name, name))
				elif type.name == 'array':
					fmt.append('array(%d) ')
					arg.append('source->%s.length' % (name))
				if guard:
					fmt.append('[G] ')

		source.write('\trl_format_msg(buffer, buffer_max, "');
		source.write(''.join(fmt))
		source.write('", ')
		source.write(', '.join(arg))
		source.write(');\n');

		source.write('}\n\n')

	# emit decoder table
	source.write('typedef void (*rl_describe_fn_t)(char *buffer, size_t max_buf, const rl_msg_t *msg);\n')
	source.write('typedef int (*rl_decode_fn_t)(const void *buffer, int size, rl_msg_t *msg_out);\n')
	source.write('typedef int (*rl_encode_fn_t)(const rl_msg_t *msg, void *buffer, int size);\n')

	source.write('static const rl_decode_fn_t decoders[%d] = {\n' % (len(messages)))
	for i in range(0, len(messages)):
		msg = messages[i]
		source.write('\tdecode_%s_%s' % (msg.name, msg.type))
		if i + 1 != len(messages):
			source.write(',')
		source.write('\n')
	source.write('};\n')

	# emit encoder table
	source.write('static const rl_encode_fn_t encoders[%d] = {\n' % (len(messages)))
	for i in range(0, len(messages)):
		msg = messages[i]
		source.write('\tencode_%s_%s' % (msg.name, msg.type))
		if i + 1 != len(messages):
			source.write(',')
		source.write('\n')
	source.write('};\n')

	# emit describer table
	source.write('static const rl_describe_fn_t describers[%d] = {\n' % (len(messages)))
	for i in range(0, len(messages)):
		msg = messages[i]
		source.write('\tdescribe_%s_%s' % (msg.name, msg.type))
		if i + 1 != len(messages):
			source.write(',')
		source.write('\n')
	source.write('};\n')

	# encoder and decoder functions
	header.write(r'''
#define rl_msg_kind_of(msg) ((rl_msg_kind_t) (msg)->handshake_request.hdr_type)
int rl_decode_msg(const void *buffer, int size, rl_msg_t *msg_out);
int rl_encode_msg(const rl_msg_t *message, void *buffer, int size, size_t *used_size);
void rl_describe_msg(const rl_msg_t *message, char *buffer, size_t max);
const char *rl_msg_name(rl_msg_kind_t kind); 
''')

	source.write('const char *rl_msg_name(rl_msg_kind_t kind) {\n')
	source.write('\tswitch(kind) {\n')
	for msg in messages:
		source.write('\t\tcase RL_MSG_%s_%s: return "%s/%s";\n' % (msg.name.upper(), msg.type.upper(), msg.name, msg.type))
	source.write('\t\tdefault: return "bogus";\n')
	source.write('\t}\n')
	source.write('}\n')


	source.write(r'''
int rl_decode_msg(const void *buffer, int size, rl_msg_t *msg_out)
{
	const rl_msg_kind_t kind = peek_msg_kind(buffer, size);
	if (RL_MSG_BOGUS == kind)
		return -1;
	else
		return (*decoders[kind])(buffer, size, msg_out);
}

int rl_encode_msg(const rl_msg_t *message, void *buffer, int size, size_t *used_size)
{
	int size_result;
	size_result = (*encoders[rl_msg_kind_of(message)])(message, buffer, size);

	if (-1 == size_result)
		return -1;

	*used_size = (size_t) size_result;
	return 0;
}

void rl_describe_msg(const rl_msg_t *message, char *buffer, size_t max)
{
	(*describers[rl_msg_kind_of(message)])(buffer, max, message);
}
''')
	source.write(SOURCE_END)

	header.write('#endif\n')

if __name__ == '__main__':
	mkmsg(sys.stdin, sys.argv[1])
