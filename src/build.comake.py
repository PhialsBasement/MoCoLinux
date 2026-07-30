# This is not a standalone Python script, but a build declaration file
# to be read by bin/make.py. Please run bin/make.py --help.

import os
import subprocess

from comake.settings import settings


def find_ddk_include(cross_prefix):
    """Locate mingw-w64's DDK headers.

    coLinux includes <ddk/ntddk.h>, which resolves off the standard include
    path, but mingw-w64's ntddk.h then includes <wdm.h> unqualified -- so the
    ddk directory has to be a search path in its own right. The old w32api 3.13
    headers coLinux was written against did not need this.

    Ask the compiler where it actually looks rather than guessing a layout.
    """
    # This file is exec'd with separate globals and locals, so the imports at
    # the top of it are not visible from inside a function body.
    import os
    import subprocess

    override = os.getenv('COLINUX_DDK_INCLUDE')
    if override:
        return [override]
    try:
        result = subprocess.run([cross_prefix + 'gcc', '-xc', '-E', '-v', os.devnull],
                                capture_output=True, text=True)
    except OSError:
        return []
    for line in result.stderr.splitlines():
        line = line.strip()
        if line.endswith('/include') and os.path.isdir(pathjoin(line, 'ddk')):
            return [pathjoin(line, 'ddk')]
    return []

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
    COLINUX_ARCH=settings.arch,
    COLINUX_HOST_OS=settings.host_os,
)

if settings.host_os == 'winnt':
    # The i686-pc-mingw32- prefix predates mingw-w64 and no longer exists in
    # any current toolchain; mingw-w64 uses the *-w64-mingw32- triplets.
    cross_compilation_prefix = os.getenv('COLINUX_HOST_CROSS_PREFIX')
    if not cross_compilation_prefix:
        if settings.arch == 'x86_64':
            cross_compilation_prefix = 'x86_64-w64-mingw32-'
        else:
            cross_compilation_prefix = 'i686-w64-mingw32-'

    if settings.arch == 'x86_64':
        compiler_flags = []
        # 0x0502 is Windows Server 2003 / XP x64, the oldest 64-bit target.
        compiler_defines['WINVER'] = '0x0502'
    else:
        # These pin down the stack-argument ABI that the i386 passage assembly
        # reads by hand at fixed %esp offsets. They mean nothing on x86-64.
        compiler_flags = ['-mpush-args', '-mno-accumulate-outgoing-args']
        compiler_defines['WINVER'] = '0x0500'

    # Scoped onto the driver target only -- see colinux/os/winnt/build. The DDK
    # headers conflict with windows.h, so userspace must not see them.
    settings.host_ddk_includes = find_ddk_include(cross_compilation_prefix)
else:
    if settings.gcc_guest_target:
        cross_compilation_prefix = settings.gcc_guest_target + '-'
    else:
        cross_compilation_prefix = ''
    compiler_flags = []
    settings.host_ddk_includes = []

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
        pathjoin(settings.target_kernel_source, 'arch/x86/include'),
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
