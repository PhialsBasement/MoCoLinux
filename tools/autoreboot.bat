@echo off
rem  Make a bugcheck reboot the machine instead of sitting on the blue screen.
rem
rem  Run once per Windows install. Survives reboots; it is a registry setting,
rem  not a running thing.
rem
rem  The problem it solves. This box has three ways to die and they need three
rem  different recoveries:
rem
rem    * A triple fault resets the processor immediately. No bugcheck, no dump,
rem      nothing written anywhere. The machine comes back on its own.
rem    * A hard hang leaves it powered with nothing executing. Only the power
rem      button helps.
rem    * A bugcheck stops the kernel and WAITS -- and with AutoReboot clear it
rem      waits forever, showing a screen nobody is in front of.
rem
rem  That third one is indistinguishable from the second at a distance, and it
rem  is the one that wasted a night: the box was unreachable, Wake-on-LAN did
rem  nothing (correctly -- the NIC only arms its magic-packet filter in a real
rem  power-off state, and the machine was powered ON, just not running), and
rem  the only remaining move was to walk over and hold the button.
rem
rem  AutoReboot is 1 on a stock XP install. If it is 0 here, we cleared it
rem  ourselves to read bugcheck codes off the screen -- which was the right call
rem  when somebody was sitting in front of the machine and is the wrong one now
rem  that it is driven over the network.
rem
rem  Nothing is lost by turning it back on. The bugcheck code and parameters go
rem  into the minidump before the reboot, getdumps.bat collects them, and a
rem  minidump says everything the blue screen does and more -- it is where the
rem  0x50 in ExFreePool and the 0xD5 in co_debug_read were both read from. The
rem  screen was never the instrument.
rem
rem  CrashDumpEnabled 3 is a small (64 KB) memory dump into %SystemRoot%\Minidump.
rem  Kernel and complete dumps need a pagefile at least as large as RAM and
rem  write 4 GB on every crash, which on a machine that bugchecks several times
rem  an evening is minutes of disk each time for information the minidump
rem  already carries.
rem
rem  Overwrite 0 keeps every dump rather than replacing one file. They are 64 KB
rem  and the pattern across a session is often the point -- four 0x50s in one
rem  evening was itself the finding.
set K=HKLM\SYSTEM\CurrentControlSet\Control\CrashControl

echo autoreboot: before --
reg query "%K%" /v AutoReboot 2>nul
reg query "%K%" /v CrashDumpEnabled 2>nul

reg add "%K%" /v AutoReboot /t REG_DWORD /d 1 /f > nul
reg add "%K%" /v CrashDumpEnabled /t REG_DWORD /d 3 /f > nul
reg add "%K%" /v Overwrite /t REG_DWORD /d 0 /f > nul
reg add "%K%" /v LogEvent /t REG_DWORD /d 1 /f > nul

echo autoreboot: after --
reg query "%K%" /v AutoReboot
reg query "%K%" /v CrashDumpEnabled

echo.
echo autoreboot: a bugcheck will now write a minidump and reboot by itself.
echo autoreboot: triple faults still reset with no dump; hard hangs still need
echo autoreboot: the power button. Neither of those is what this fixes.
