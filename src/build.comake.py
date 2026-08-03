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


def make_ndis_compat_include(ddk_includes, output_dir):
    """Repair mingw-w64's ddk/ndis.h so NDIS drivers can include it.

    As of mingw-w64 16.1 the declaration of NdisMWanIndicateReceiveComplete()
    is missing the comma between its two parameters:

        NdisMWanIndicateReceiveComplete(
                IN NDIS_HANDLE  MiniportAdapterHandle
                IN NDIS_HANDLE  NdisLinkContext);

    It sits in the body of the header rather than behind an optional guard, so
    no NDIS driver can include ndis.h at all.

    Rather than vendoring a snapshot of a 12,000-line header that would drift
    from the installed one, derive a corrected copy from whatever is installed
    and put it earlier on the driver's include path. If the installed header is
    already correct -- because it was fixed upstream -- nothing is generated and
    the system header is used directly.

    Returns the include directories to prepend, or [] if none are needed.
    """
    import os
    import re

    source = None
    for candidate in ddk_includes:
        if os.path.exists(pathjoin(candidate, 'ndis.h')):
            source = pathjoin(candidate, 'ndis.h')
            break
    if source is None:
        return []

    with open(source, encoding='latin-1') as handle:
        lines = handle.read().splitlines(True)

    # A parameter line ending in an identifier with no comma, whose successor is
    # another IN/OUT/IN OUT parameter, is a missing comma.
    parameter = re.compile(r'^(\s*(?:IN|OUT|IN OUT)\s+.*[A-Za-z0-9_])\s*$')
    successor = re.compile(r'^\s*(?:IN|OUT|IN OUT)\s')
    repaired = 0
    for index in range(len(lines) - 1):
        match = parameter.match(lines[index].rstrip('\n'))
        if match and successor.match(lines[index + 1]):
            lines[index] = match.group(1) + ',\n'
            repaired += 1

    if not repaired:
        return []

    generated = pathjoin(output_dir, 'ddk', 'ndis.h')
    content = ''.join(lines)
    if os.path.exists(generated):
        with open(generated, encoding='latin-1') as handle:
            if handle.read() == content:
                return [output_dir]
    os.makedirs(pathjoin(output_dir, 'ddk'), exist_ok=True)
    with open(generated, 'w', encoding='latin-1') as handle:
        handle.write(content)
    print("Repaired %d missing comma(s) in a local copy of %s" % (repaired, source))
    return [output_dir]

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

# Lets the daemon default to a console that was actually built. See
# CO_DEFAULT_CONSOLE in colinux/user/daemon.h.
if os.getenv('COLINUX_ENABLE_FLTK') == 'yes':
    compiler_defines['CO_DEFAULT_CONSOLE_FLTK'] = None

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
        #
        # Both names, because they do different jobs and mingw-w64 derives one
        # from the other only when the other is absent. WINVER gates the shell
        # and GDI declarations, _WIN32_WINNT the kernel ones -- so with WINVER
        # alone the headers still declare kernel32 functions this target does
        # not have, and calling one compiles cleanly and fails at load time
        # with an unresolved import. That is the same class of failure as the
        # UCRT one in os/winnt/build, and equally invisible on a test box that
        # has one-core-api supplying the missing exports.
        compiler_defines['WINVER'] = '0x0502'
        compiler_defines['_WIN32_WINNT'] = '0x0502'
    else:
        # These pin down the stack-argument ABI that the i386 passage assembly
        # reads by hand at fixed %esp offsets. They mean nothing on x86-64.
        compiler_flags = ['-mpush-args', '-mno-accumulate-outgoing-args']
        compiler_defines['WINVER'] = '0x0500'

    # Scoped onto the driver target only -- see colinux/os/winnt/build. The DDK
    # headers conflict with windows.h, so userspace must not see them.
    ddk_includes = find_ddk_include(cross_compilation_prefix)
    settings.host_ddk_includes = make_ndis_compat_include(
        ddk_includes,
        target_pathname(pathjoin('colinux', 'os', 'winnt', 'kernel', 'ndis-compat'))
    ) + ddk_includes
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
