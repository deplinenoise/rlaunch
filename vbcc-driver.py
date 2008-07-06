#! /usr/bin/python

import sys
import os
import os.path
import subprocess
import re

line_re = re.compile(r'^(warning|error) (\d+) in line (\d+) of "([^"]*)":\s*(.*)$')

def fix_fn(root_dir, fn):
	# If there are path separators in the filename, assume the path is valid
	if fn.find(os.sep) != -1:
		return fn
	
	if os.path.exists(fn):
		return fn

	full_path = os.path.join(root_dir, fn)

	if os.path.exists(full_path):
		return full_path

	return 'bah'

def munge(root_dir, line):
	m = re.match(line_re, line)
	if not m:
		return line.strip()

	fn = fix_fn(root_dir, m.group(4))
	return '%s(%s) : %s %s: %s' % (fn, m.group(3), m.group(1), m.group(2), m.group(5))


if __name__ == '__main__':

	vbcc_root = os.environ.get('VBCC')
	if not vbcc_root:
		sys.stderr.write('VBCC environment variable not set')
		sys.exit(1)

	vc_bin = os.path.join(vbcc_root, 'bin' + os.sep + 'vc')

	if os.name == 'nt':
		vc_bin += '.exe'

	root_dir = '.'

	for arg in sys.argv[1:]:
		if arg.endswith('.c'):
			root_dir, dummy = os.path.split(arg)

	vc = subprocess.Popen(
			args = sys.argv[1:],
			executable = vc_bin,
			universal_newlines=True,
			stdin = None,
			stdout = subprocess.PIPE,
			stderr = subprocess.STDOUT)

	for line in vc.stdout:
		sys.stdout.write(munge(root_dir, line))
		sys.stdout.write('\n')
