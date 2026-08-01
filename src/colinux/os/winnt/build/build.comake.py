from comake.settings import settings

def optional_targets():
    import os
    from os import getenv
    optional = []
    if getenv('COLINUX_ENABLE_WX') == "yes":
        optional.append(Input('colinux-console-wx.exe'))
    # The FLTK console needs a hand-built patched FLTK 1.1.10 for mingw (see
    # doc/building). Opt-in, like the wx console, so the driver and the daemons
    # build without it. colinux-console-nt.exe needs no external library.
    if getenv('COLINUX_ENABLE_FLTK') == "yes":
        optional.append(Input('colinux-console-fltk.exe'))
    # The bridged network daemon needs the WinPcap developer pack for pcap.h.
    if getenv('COLINUX_ENABLE_WINPCAP') == "yes":
        optional.append(Input('colinux-bridged-net-daemon.exe'))
    return optional

targets['executables'] = Target(
    inputs=[
    Input('colinux-daemon.exe'),
    Input('colinux-net-daemon.exe'),
    Input('colinux-debug-daemon.exe'),
    Input('colinux-console-nt.exe'),
    Input('colinux-ndis-net-daemon.exe'),
    Input('colinux-slirp-net-daemon.exe'),
    Input('colinux-serial-daemon.exe'),
    Input('linux.sys'),
    ] + optional_targets(),
    tool = Empty(),
)

# The libraries every winnt daemon links, named once so the slirp daemon's
# hand-written link line below cannot drift away from generate_options(). The
# C runtime is not in here: it differs per architecture and is added by both
# callers.
winnt_daemon_libs = [
    'user32', 'gdi32', 'ws2_32', 'ntdll', 'kernel32', 'ole32', 'uuid', 'gdi32',
]

# Both users below take the list as a default argument rather than reading it
# from module scope. This file is exec'd with separate globals and locals -- the
# note above generate_options()'s own import says so -- so a name defined here
# is not visible from inside a function body. Defaults are evaluated at def
# time, where it is.

def generate_options(compiler_def_type, libs=None, lflags=None,
                     winnt_daemon_libs=winnt_daemon_libs):
    if not libs:
        libs = []
    if not lflags:
        lflags = []
    # This file is exec'd with separate globals and locals, so the import at the
    # top of it is not visible from inside a function body.
    from comake.settings import settings

    crt = ['msvcrt']
    if settings.arch != 'x86_64':
        # crtdll is the NT 3.x/4.0 C runtime. It predates Win64 entirely, so
        # mingw-w64 ships libcrtdll.a for i686 only. msvcrt is the CRT either way.
        crt.append('crtdll')

    return Options(
        overriders = dict(
            compiler_def_type = compiler_def_type,
            compiler_strip = True,
        ),
        appenders = dict(
        compiler_flags = [ '-mno-cygwin' ],
        #
        # The GCC runtime goes in the binary, not beside it.
        #
        # mingw-w64 links libgcc dynamically by default, and for x86-64 that is
        # libgcc_s_seh-1.dll -- the SEH unwinder. Every daemon that needs it
        # then refuses to start on a machine that does not have it, which is
        # any Windows that has not had a mingw runtime installed on it. The
        # test box is exactly that machine, and the failure is a dialog about a
        # missing DLL rather than anything to do with coLinux.
        #
        # libstdc++ for the same reason one target over: colinux-console-nt is
        # C++, so it would want libstdc++-6.dll beside it as well.
        #
        # These are shipped as single files that get copied onto a box by hand;
        # a binary that needs a runtime next to it is a binary that will
        # eventually be copied without it.
        #
        # -static rather than the two narrower flags, because those left
        # libwinpthread-1.dll behind on the two targets that pull in the C++
        # runtime, and a second missing-DLL dialog is no better than the first.
        #
        linker_flags = lflags + [ '-static' ],
        compiler_libs = libs + winnt_daemon_libs + crt + ['shlwapi']),
    )

def generate_wx_options():
    return Options(
        overriders = dict(
            compiler_def_type = 'g++',
            compiler_strip = True,
        ),
        appenders = dict(
                compiler_flags = [ '`wx-config --cxxflags`' ],
                linker_add = [ '`wx-config --libs`' ],
        )
    )

user_dep = [Input('../user/user-all.a')]

targets['colinux-daemon.exe'] = Target(
    inputs = [
        Input('../user/daemon/res/daemon.res'),
        Input('../user/daemon/daemon.o'),
    ] + user_dep,
    tool = Compiler(),
    mono_options = generate_options('gcc'),
)

targets['colinux-net-daemon.exe'] = Target(
    inputs = [
        Input('../user/daemon/res/colinux-net.res'),
        Input('../user/conet-daemon/build.a'),
        Input('../../../user/daemon-base/build.a'),
    ] + user_dep,
    tool = Compiler(),
    mono_options = generate_options('g++'),
)

targets['colinux-bridged-net-daemon.exe'] = Target(
    inputs = [
        Input('../user/daemon/res/colinux-bridged-net.res'),
        Input('../user/conet-bridged-daemon/build.o'),
    ] + user_dep,
    tool = Compiler(),
    mono_options = generate_options('gcc', libs=['wpcap']),
)

targets['colinux-ndis-net-daemon.exe'] = Target(
    inputs = [
        Input('../user/daemon/res/colinux-ndis-net.res'),
        Input('../user/conet-ndis-daemon/build.o'),
    ] + user_dep,
    tool = Compiler(),
    mono_options = generate_options('gcc'),
)

#
# The slirp daemon is confined to a 2 GB address space, so that every pointer
# in the process fits in 32 bits.
#
# It needs that because the vendored slirp's protocol structures are overlays
# on wire format: the queue links inside them are u_int32_t and
# insque_32/remque_32 store pointers through them (slirp/misc.c, and the note
# in slirp/slirp_config.h). Widening those fields is not available -- their
# size is the IP header's.
#
# Two halves. --image-base 0x400000 puts the image itself low, because
# mingw-w64 defaults to 0x140000000 and the IP fragment queue head is a static:
# above 4 GB, the first fragment queued truncates its address and the next
# traversal dereferences the remains. Clearing
# IMAGE_FILE_LARGE_ADDRESS_AWARE then caps the heap. ld can do the second part
# itself for i386 (--disable-large-address-aware); the x86-64 linker rejects
# that option outright, so a post-link pass does it, chained onto the link so
# it cannot be forgotten.
#
# i386 keeps its ordinary link: a 32-bit process has no addresses to lose.
#
# The confinement costs nothing here. This daemon relays frames between a
# socket and the monitor; it maps no guest memory.
#
def slirp_daemon_cmdline(scripter, tool_run_inf,
                         winnt_daemon_libs=winnt_daemon_libs):
    inputs = ' '.join([i.pathname for i in tool_run_inf.target.inputs])
    target = tool_run_inf.target.pathname
    gcc = scripter.get_cross_build_tool('gcc', tool_run_inf)

    from comake.settings import settings

    low, fixup = '', ''
    if settings.arch == 'x86_64':
        low = '-Wl,--image-base,0x400000 '
        fixup = ' && python3 tools/pe-clear-laa.py ' + target

    return ('%s -static %s-o %s %s %s%s' %
            (gcc, low, target, inputs,
             ' '.join(['-l' + l for l in
                       ['iphlpapi'] + winnt_daemon_libs +
                       ['msvcrt', 'shlwapi']]),
             fixup))


targets['colinux-slirp-net-daemon.exe'] = Target(
    inputs = [
        Input('../user/daemon/res/colinux-slirp-net.res'),
        Input('../user/conet-slirp-daemon/build.o'),
        Input('../../../user/slirp/build.o'),
    ] + user_dep,
    tool = Script(slirp_daemon_cmdline),
    mono_options = generate_options('gcc', libs=['iphlpapi']),
)

targets['colinux-serial-daemon.exe'] = Target(
    inputs = [
        Input('../user/daemon/res/colinux-serial.res'),
        Input('../user/coserial-daemon/build.o'),
    ] + user_dep,
    tool = Compiler(),
    mono_options = generate_options('gcc'),
)

targets['colinux-console-fltk.exe'] = Target(
    inputs = [
        Input('../user/daemon/res/colinux-fltk.res'),
        Input('../user/console-fltk/build.a'),
        Input('../../../user/console-fltk/build.a'),
    ] + user_dep,
    tool = Compiler(),
    mono_options = generate_options('g++', libs=['fltk', 'mingw32'], lflags=['-mwindows']),
)

targets['colinux-console-nt.exe'] = Target(
    inputs = [
        Input('../user/daemon/res/colinux-nt.res'),
        Input('../user/console-nt/build.a'),
        Input('../../../user/console-nt/build.a'),
    ] + user_dep,
    tool = Compiler(),
    mono_options = generate_options('g++'),
)

targets['colinux-console-wx.exe'] = Target(
    inputs = [
        Input('../user/daemon/res/colinux-wx.res'),
        Input('../../../user/console-wx/build.o'),
    ],
    tool = Compiler(),
    mono_options = generate_wx_options(),
)

targets['colinux-debug-daemon.exe'] = Target(
    inputs = [
        Input('../user/daemon/res/colinux-debug.res'),
        Input('../user/debug/build.o'),
        Input('../../../user/debug/build.o'),
    ] + user_dep,
    tool = Compiler(),
    mono_options = generate_options('gcc'),
)

targets['driver.o'] = Target(
    inputs = [
       Input('../../../kernel/build.o'),
       Input('../kernel/build.o'),
       Input('../../../arch/build.o'),
       Input('../../../common/common.a'),
    ],
    tool = Linker(),
)

def script_cmdline(scripter, tool_run_inf):
    from comake.settings import settings

    inputs = tool_run_inf.target.get_actual_inputs()

    # _DriverEntry@8 is the i386 stdcall decoration of DriverEntry. There is no
    # stdcall name decoration in the Win64 ABI, so on x86-64 the symbol is plain
    # DriverEntry -- get this wrong and the linker silently defaults the entry
    # point to the start of the image, producing a driver that cannot load.
    if settings.arch == 'x86_64':
        entry = 'DriverEntry'
    else:
        entry = '_DriverEntry@8'

    command_line = ((
        "%s "
        "-Wl,--strip-debug "
        "-Wl,--subsystem,native "
        "-Wl,--image-base,0x10000 "
        "-Wl,--file-alignment,0x1000 "
        "-Wl,--section-alignment,0x1000 "
        "-Wl,--entry,%s "
        "-mdll -nostartfiles -nostdlib "
        "-o %s %s -lndis -lntoskrnl -lhal -lgcc ") %
    (scripter.get_cross_build_tool('gcc', tool_run_inf),
     entry,
     tool_run_inf.target.pathname,
     inputs[0].pathname))
    return command_line

targets['linux.sys'] = Target(
    tool = Script(script_cmdline),
    inputs = [
       Input('driver.o'),
    ],
    options = Options(
        appenders = dict(
            compiler_defines = dict(
                __KERNEL__=None,
                CO_KERNEL=None,
                CO_HOST_KERNEL=None,
            ),
            # mingw-w64's ddk/ntddk.h includes <wdm.h> unqualified, so the ddk
            # directory has to be on the search path in its own right. Confined
            # to the driver: these headers collide with windows.h.
            compiler_includes = settings.host_ddk_includes,
        )
    )
)

"""
The driver used to be linked twice: once with --base-file to capture relocations,
then dlltool --output-exp turned that base file into an object which was fed to a
second link. That was necessary with the binutils of the time, which did not emit
base relocations for this kind of image.

Current binutils ld does emit them, so doing both put every fixup in .reloc
twice. The Windows loader applies each one it finds, so every absolute address in
the driver got the load-bias added twice and pointed into nowhere. On XP x64 that
showed up as PAGE_FAULT_IN_NONPAGED_AREA on the first dereference of a .refptr
slot in co_manager_load(), reading an address exactly (2 * bias + target) away.
The i386 driver had the same duplication.

Let ld generate them once.
"""


targets['installer'] = Target(
    inputs = [
       Input('executables'),
       Input('../user/install/coLinux.exe'),
    ],
    tool = Empty(),
)
