#!/usr/bin/python
import os, sys
import platform

debug = ARGUMENTS.get('debug', 0)
libthrash_test = ARGUMENTS.get('lthrash-test', 0)

if GetOption('help'):
    sys.exit(1)

env = Environment()

extra_cflags = ARGUMENTS.get('CFLAGS')
extra_libdir = ARGUMENTS.get('LIBDIR')
static       = ARGUMENTS.get('static')

arch = platform.system() 

if arch == 'OpenBSD':
    env.Append(CFLAGS='-DPLATFORM_OPENBSD')
elif arch == 'Linux':
    env.Append(CFLAGS='-DPLATFORM_LINUX')
else:
    print "Unsupported architecture %s" % arch
    sys.exit(1)

if extra_cflags:
    env.Append(CFLAGS=extra_cflags)

if extra_libdir:
    el = extra_libdir.split()
    env.Append(LIBPATH=el)

env.ParseConfig('pkg-config --cflags --libs glib-2.0')

if int(debug):
    env.Append(CFLAGS='-DDEBUG -ggdb -O2') 
else:
    env.Append(CFLAGS='-O3')

env.Append(LIBS=['event'])

if static:
    env.Append(LIBS='rt')
    env.Append(LINKFLAGS='-static')

env.Object('libthrasher', ['libthrasher.c'])
env.Program('thrashd', ['iov.c', 'thrashd.c'])
env.Program('master_thrasher', ['libthrasher.c', 'iov.c', 'master_thrasher.c'])

if ARGUMENTS.get('bgp'):
    env.Append(CFLAGS='-I/home/mthomas/openbgpd-linux-4.6/bgpd')
    env.Append(CFLAGS='-I/home/mthomas/openbgpd-linux-4.6/openbsd-compat')
    env.Append(CFLAGS='-I/home/mthomas/openbgpd-linux-4.6/')
    env.Program('bgp', ['bgp.c',
        '/home/mthomas/openbgpd-linux-4.6/bgpctl/buffer.o',
        '/home/mthomas/openbgpd-linux-4.6/bgpctl/imsg.o'])

if libthrash_test:
    env.Append(CFLAGS="-DLIBTHRASHER_MAIN");
    env.Program('libthrasher-test', ['iov.c', 'libthrasher.c'])
