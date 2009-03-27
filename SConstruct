# vim: syn=python

import os
import os.path
import sys

import SCons.Defaults
import SCons.Tool
import SCons.Util

if ARGUMENTS.get('amiga', 0):
	env = Environment(ENV = os.environ)

	env['PLATFORM'] = 'amiga'
	env['PROGSUFFIX'] = '.exe'
	env['LIBPREFIX'] = ''

	env['CC']		 = 'python %s%svbcc-driver.py' % (os.getcwd(), os.sep)
	env['CCFLAGS']	= SCons.Util.CLVar('-hunkdebug -cpu=68020')
	env['CCCOM']	 = '$CC $CFLAGS $CCFLAGS $CPPFLAGS $_CPPDEFFLAGS $_CPPINCFLAGS -c -o $TARGET $SOURCES'
	env['SHCC']		 = '$CC'
	env['SHCCFLAGS'] = SCons.Util.CLVar('$CCFLAGS')
	env['SHCFLAGS'] = SCons.Util.CLVar('$CFLAGS')
	env['SHCCCOM']	 = '$SHCC -WD $SHCFLAGS $SHCCFLAGS $CPPFLAGS $_CPPDEFFLAGS $_CPPINCFLAGS -c -o$TARGET $SOURCES'
	env['CPPDEFPREFIX']  = '-D'
	env['CPPDEFSUFFIX']  = ''
	env['INCPREFIX']  = '-I'
	env['INCSUFFIX']  = ''
	env['SHOBJSUFFIX'] = '.library'
	env['STATIC_AND_SHARED_OBJECTS_ARE_THE_SAME'] = 0
	env['CFILESUFFIX'] = '.c'

	env['LIBDIRPREFIX'] = '-L'
	env['LIBDIRSUFFIX'] = ''
	env['LIBLINKPREFIX'] = '-l'
	env['LIBLINKSUFFIX'] = ''

	env['VBCC'] = os.environ['VBCC']
	env['VBCCTARGET'] = 'm68k-amigaos'
	env['VBCCSTARTUP'] = 'minstart.o'

	env['LINK'] = os.path.join(os.environ['VBCC'], 'bin/vlink')
	# Adding -b amigahunk doesn't seem required; it's the default format of vlink.
	env['LINKFLAGS'] = SCons.Util.CLVar('-Bstatic -nostdlib $VBCC/targets/$VBCCTARGET/lib/$VBCCSTARTUP')
	env['_LIBDIRFLAGS'] = ''
	env['LIBDIRPREFIX']='-L'
	env['LIBLINKPREFIX']='-l'
	env['_LIBFLAGS']='${_stripixes(LIBLINKPREFIX, LIBS, LIBLINKSUFFIX, LIBPREFIXES, LIBSUFFIXES, __env__)}'
	env['LINKCOM'] = '$LINK -L$VBCC/targets/$VBCCTARGET/lib $LINKFLAGS $SOURCES $_LIBDIRFLAGS $_LIBFLAGS -o $TARGET'

	env.Append(LIBPATH = Split(''))
	env.Append(LIBS = Split(''))
	env.Append(CPPDEFINES = Split('NO_C_LIB'))
	env.Append(LINKFLAGS = Split(''))
	env.Append(CCFLAGS = Split('-warn=-1'))
	for wn in [163, 307, 65, 166, 167, 81]:
		env.Append(CCFLAGS = Split('-dontwarn=%d' % (wn)))
else:
	env = Environment(ENV = os.environ)

env.Replace(ROOT_DIR = os.getcwd())

target_os = os.name

if env['PLATFORM'] == 'win32':
	env.Append(CPPDEFINES=Split('WIN32 WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _CRT_SECURE_NO_DEPRECATE'))
	env.Append(CCFLAGS=Split('/W4 /wd4100 /wd4127 /WX'))
elif env['PLATFORM'] == 'posix':
	env.Append(CPPDEFINES=Split('RL_POSIX _GNU_SOURCE _XOPEN_SOURCE=500'))
	env.Append(CCFLAGS=Split('-Wall -Werror -ansi -std=c89 -pedantic'))

Export('env')

for flavor in (('debug', '-debug'), ('release', '')):
	bd = flavor[0] + '/' + env['PLATFORM']
	env.SConscript(bd+'/SConscript', exports=['env', 'flavor'], build_dir=bd, src_dir='src', duplicate=0)

env.Command(target='tags', source='', action='ctags -R')

Default('debug')
