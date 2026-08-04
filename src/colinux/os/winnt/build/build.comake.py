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
    Input('kread-hammer.exe'),
    Input('kmap-test.exe'),
    Input('cogpu-daemon.exe'),
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

def generate_options(compiler_def_type, libs=None, lflags=None, ladd=None,
                     winnt_daemon_libs=winnt_daemon_libs):
    if not libs:
        libs = []
    if not lflags:
        lflags = []
    if not ladd:
        ladd = []
    # This file is exec'd with separate globals and locals, so the import at the
    # top of it is not visible from inside a function body.
    from comake.settings import settings

    # The C runtime, and this is the difference between a binary that loads on
    # the target and one that does not.
    #
    # mingw-w64 16.1 resolves -lmsvcrt to a library that forwards to the
    # Universal CRT, so a default link imports api-ms-win-crt-*-l1-1-0.dll.
    # Those are Windows 10 API sets; Microsoft's own UCRT redistributable
    # supports Vista SP2 and later and has never supported XP. Every userspace
    # binary this port produced before this change therefore required a runtime
    # that does not exist on the operating system it targets, and the test box
    # could not reveal it: it has one-core-api installed, which supplies
    # ucrtbase.dll and kernelbase.dll to a 5.2 kernel.
    #
    # libmsvcrt-os.a is the genuine msvcrt.dll import library, and gcc's own
    # spec carries the selector -- %{!mcrtdll=*:-lmsvcrt} %{mcrtdll=*:-l%*}.
    # Measured on a hello-world: -static alone imports nine api-ms-win-crt
    # sets, -static -mcrtdll=msvcrt-os imports KERNEL32.dll and msvcrt.dll and
    # nothing else. libmingwex.a references no __stdio_common_* at all, so it
    # fills the gaps rather than pulling the UCRT back in by another route.
    #
    # The flag alone is not enough while the CRT is also named explicitly:
    # adding -lmsvcrt back to that same command line re-imports all nine, since
    # the forwarding library is still on the line. So the name changes too, and
    # both have to stay in step.
    crt_flags = []
    if settings.arch == 'x86_64':
        crt = ['msvcrt-os']
        crt_flags = ['-mcrtdll=msvcrt-os']
    else:
        crt = ['msvcrt']
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
        linker_flags = lflags + [ '-static' ] + crt_flags,
        # After the objects, which is where a library has to be for ld to
        # resolve anything from it.
        linker_add = ladd,
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

    low, fixup, crt = '', '', 'msvcrt'
    if settings.arch == 'x86_64':
        low = '-Wl,--image-base,0x400000 -mcrtdll=msvcrt-os '
        fixup = ' && python3 tools/pe-clear-laa.py ' + target
        # Same reasoning as generate_options(): the default -lmsvcrt forwards to
        # the Universal CRT, which XP does not have. Named here as well because
        # this link line is hand-written and does not go through that function.
        crt = 'msvcrt-os'

    return ('%s -static %s-o %s %s %s%s' %
            (gcc, low, target, inputs,
             ' '.join(['-l' + l for l in
                       ['iphlpapi'] + winnt_daemon_libs +
                       [crt, 'shlwapi']]),
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

# R0's verification tool, not a shipped daemon: it hammers CO_MANAGER_IOCTL_KREAD
# from a second process across a guest teardown, which is the race the KREAD
# lock exists to close. Built here rather than by hand so it cannot drift from
# the headers it tests against.
targets['kread-hammer.exe'] = Target(
    inputs = [
        Input('../user/kread-hammer/build.o'),
    ] + user_dep,
    tool = Compiler(),
    mono_options = generate_options('gcc'),
)

# R3 step 1's verification tool. Maps the guest's RAM into itself and then lets
# go in one of three ways -- tidily, by exiting without unmapping, or by
# crashing outright -- because the assumption the whole rung stands on is that
# IRP_MJ_CLEANUP unmaps for a process that never asked.
targets['kmap-test.exe'] = Target(
    inputs = [
        Input('../user/kmap-test/build.o'),
    ] + user_dep,
    tool = Compiler(),
    mono_options = generate_options('gcc'),
)

# R4's daemon: the host side of the guest's GPU. A sibling of the slirp bridge
# -- an ordinary userspace program that services one of the guest's device
# rings -- and it lives in userspace because that is where WGL and the NVIDIA
# driver are. Will embed virglrenderer at R5.
# Links virglrenderer and libepoxy from the cross build in download/, plus the
# WGL winsys R2 wrote. The renderer is what turns a guest command stream into
# GL on the host's card.
VIRGL_PREFIX = '/mnt/big-bricks/RProject/MoCoLinux/download/prefix-mingw'

targets['cogpu-daemon.exe'] = Target(
    inputs = [
        Input('../user/cogpu-daemon/build.o'),
    ] + user_dep,
    tool = Compiler(),
    # The import libraries are named outright rather than with -l, because
    # generate_options passes -static (so every daemon carries its runtime
    # rather than needing DLLs beside it) and -static makes ld skip .dll.a
    # entirely. Naming the archives directly still works: an import library is
    # a static archive of stubs.
    mono_options = generate_options(
        'gcc',
        libs = ['opengl32'],
        ladd = [VIRGL_PREFIX + '/lib/libvirglrenderer.dll.a',
                VIRGL_PREFIX + '/lib/libepoxy.dll.a'],
    ),
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
                #
                # _WIN32_WINNT is 0x0502 everywhere else, so that the userspace
                # headers stop declaring functions XP x64 does not export. The
                # DDK cannot take it: mingw-w64's ddk/wdm.h uses
                # SYSTEM_POWER_STATE_CONTEXT unconditionally while winnt.h
                # declares it only from Vista, so pinning the driver to 0x0502
                # is an "unknown type name" on a line nothing here calls.
                #
                # 0xa00 is not a choice, it is what this subtree already had:
                # ddk/ntddk.h defaults _WIN32_WINNT to 0xa00 when it is unset,
                # which is how every driver this port has shipped was compiled.
                # Restoring it keeps the driver exactly as it was while the
                # userspace side gets the narrower value it needs.
                #
                # The version a kernel-mode header thinks it is targeting says
                # nothing about what the driver imports, which is checked
                # separately: linux.sys resolves against ntoskrnl.exe, HAL.dll
                # and NDIS.SYS and nothing else.
                _WIN32_WINNT='0x0A00',
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
