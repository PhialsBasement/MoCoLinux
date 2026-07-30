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

def generate_options(compiler_def_type, libs=None, lflags=None):
    if not libs:
        libs = []
    if not lflags:
        lflags = []
    return Options(
        overriders = dict(
            compiler_def_type = compiler_def_type,
            compiler_strip = True,
        ),
        appenders = dict(
        compiler_flags = [ '-mno-cygwin' ],
        linker_flags = lflags,
        compiler_libs = libs + [
            'user32', 'gdi32', 'ws2_32', 'ntdll', 'kernel32', 'ole32', 'uuid', 'gdi32',
            'msvcrt', 'crtdll', 'shlwapi',
        ]),
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

targets['colinux-slirp-net-daemon.exe'] = Target(
    inputs = [
        Input('../user/daemon/res/colinux-slirp-net.res'),
        Input('../user/conet-slirp-daemon/build.o'),
        Input('../../../user/slirp/build.o'),
    ] + user_dep,
    tool = Compiler(),
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
    inputs = tool_run_inf.target.get_actual_inputs()
    command_line = ((
        "%s "
        "-Wl,--strip-debug "
        "-Wl,--subsystem,native "
        "-Wl,--image-base,0x10000 "
        "-Wl,--file-alignment,0x1000 "
        "-Wl,--section-alignment,0x1000 "
        "-Wl,--entry,_DriverEntry@8 "
        "-Wl,%s "
        "-mdll -nostartfiles -nostdlib "
        "-o %s %s -lndis -lntoskrnl -lhal -lgcc ") %
    (scripter.get_cross_build_tool('gcc', tool_run_inf),
     inputs[1].pathname,
     tool_run_inf.target.pathname,
     inputs[0].pathname))
    return command_line

targets['linux.sys'] = Target(
    tool = Script(script_cmdline),
    inputs = [
       Input('driver.o'),
       Input('driver.base.exp'),
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

def script_cmdline(scripter, tool_run_inf):
    inputs = tool_run_inf.target.get_actual_inputs()
    command_line = ((
        "%s "
        "--dllname linux.sys "
        "--base-file %s "
        "--output-exp %s") %
    (scripter.get_cross_build_tool('dlltool', tool_run_inf),
     inputs[0].pathname,
     tool_run_inf.target.pathname))
    return command_line


targets['driver.base.exp'] = Target(
    tool = Script(script_cmdline),
    inputs = [
       Input('driver.base.tmp'),
    ],
)

def script_cmdline(scripter, tool_run_inf):
    inputs = tool_run_inf.target.get_actual_inputs()
    command_line = ((
        "%s "
        "-Wl,--base-file,%s "
        "-Wl,--entry,_DriverEntry@8 "
        "-nostartfiles -nostdlib "
        "-o junk.tmp %s -lndis -lntoskrnl -lhal -lgcc ; "
        "rm -f junk.tmp") %
    (scripter.get_cross_build_tool('gcc', tool_run_inf),
     tool_run_inf.target.pathname,
     inputs[0].pathname))
    return command_line


targets['driver.base.tmp'] = Target(
    tool = Script(script_cmdline),
    inputs = [
       Input('driver.o'),
    ],
)

targets['installer'] = Target(
    inputs = [
       Input('executables'),
       Input('../user/install/coLinux.exe'),
    ],
    tool = Empty(),
)
