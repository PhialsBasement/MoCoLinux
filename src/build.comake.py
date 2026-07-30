# This is not a standalone Python script, but a build declaration file
# to be read by bin/make.py. Please run bin/make.py --help.

import os

from comake.settings import settings

settings.arch = os.getenv('COLINUX_ARCH')
if not settings.arch:
    settings.arch = 'i386'
    print("Target architecture not specified, defaulting to %s" % (settings.arch, ))

current_arch_symlink = target_pathname(pathjoin('colinux', 'arch', 'current'))
if os.path.exists(current_arch_symlink):
    os.unlink(current_arch_symlink)
os.symlink(settings.arch, current_arch_symlink)

settings.host_os = os.getenv('COLINUX_HOST_OS')
if not settings.host_os:
    settings.host_os = 'winnt'
    print("Target OS not specified, defaulting to %s" % (settings.host_os, ))

current_os_symlink = target_pathname(pathjoin('colinux', 'os', 'current'))
if os.path.exists(current_os_symlink):
    os.unlink(current_os_symlink)
os.symlink(settings.host_os, current_os_symlink)

settings.cflags = os.getenv('COLINUX_CFLAGS')
if not settings.cflags:
    settings.cflags = ''

settings.lflags = os.getenv('COLINUX_LFLAGS')
if not settings.lflags:
    settings.lflags = ''

# Setup "i686-co-linux", if local gcc can't use for linux kernel
settings.gcc_guest_target = os.getenv('COLINUX_GCC_GUEST_TARGET');

compiler_defines = dict(
    COLINUX_FILE_ID='0',
    COLINUX=None,
    CO_HOST_API=None,
    COLINUX_DEBUG=None,
    COLINUX_ARCH=settings.host_os,
)

if settings.host_os == 'winnt':
    cross_compilation_prefix = 'i686-pc-mingw32-'
    compiler_flags = ['-mpush-args', '-mno-accumulate-outgoing-args']
    compiler_defines['WINVER'] = '0x0500'
else:
    if settings.gcc_guest_target:
        cross_compilation_prefix = settings.gcc_guest_target + '-'
    else:
        cross_compilation_prefix = ''
    compiler_flags = []

settings.target_kernel_source = getenv('COLINUX_TARGET_KERNEL_SOURCE')

if not settings.target_kernel_source:
    settings.target_kernel_source = getenv('COLINUX_TARGET_KERNEL_PATH')

if not settings.target_kernel_source:
    print()
    print("COLINUX_TARGET_KERNEL_PATH not set. Please set this environment variable to the")
    print("pathname of a coLinux-enabled kernel source tree, i.e, a Linux kernel tree that")
    print("is patched with the patch file which is under the patch/ directory.")
    raise BuildCancelError()

settings.target_kernel_build = getenv('COLINUX_TARGET_KERNEL_BUILD')

# Handle headers from in source and out of tree builds
if not settings.target_kernel_build:
    settings.target_kernel_build = settings.target_kernel_source

if settings.target_kernel_build == settings.target_kernel_source:
    settings.target_kernel_includes = [
        pathjoin(settings.target_kernel_source, 'include') ]
else:
    settings.target_kernel_includes = [
        pathjoin(settings.target_kernel_build, 'include'),
        pathjoin(settings.target_kernel_build, 'include2'),
        pathjoin(settings.target_kernel_source, 'arch/x86/include'),
        pathjoin(settings.target_kernel_source, 'include') ]

if not hasattr(settings, 'final_build_target'):
    settings.final_build_target = 'executables'

targets['build'] = Target(
    inputs=[Input('colinux/os/%s/build/%s' % (settings.host_os,
                                              settings.final_build_target))],
    options=Options(
        overriders=dict(
            cross_compilation_prefix=cross_compilation_prefix,
        ),
        appenders=dict(
            compiler_flags=[
                '-Wno-trigraphs', '-fno-strict-aliasing', '-Wall',
                settings.cflags,
            ] + compiler_flags,
            linker_flags=[
                settings.lflags,
            ],
            compiler_includes=[
                'src',
            ] + settings.target_kernel_includes,
            compiler_defines=compiler_defines,
        )
    ),
    tool = Empty(),
)
