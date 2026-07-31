@echo off
rem ---------------------------------------------------------------------------
rem  Make the M92p recoverable without physical access.
rem
rem  Run this once, from an elevated command prompt, in F:\xfer.
rem
rem  The problem it solves: a bugcheck or a hang during driver work leaves the
rem  box off the network until someone walks to it, powers it on, logs in, and
rem  starts the agent by hand. Three manual steps, any one of which makes the
rem  machine unreachable.
rem ---------------------------------------------------------------------------

echo.
echo === 1. reboot automatically after a bugcheck, and keep a minidump ===
rem  AutoReboot 1  : do not sit on the blue screen waiting for a keypress
rem  CrashDumpEnabled 3 : small (64K) minidump -- enough to read the stop code
rem                       and faulting module later, cheap to write
reg add "HKLM\SYSTEM\CurrentControlSet\Control\CrashControl" /v AutoReboot /t REG_DWORD /d 1 /f
reg add "HKLM\SYSTEM\CurrentControlSet\Control\CrashControl" /v CrashDumpEnabled /t REG_DWORD /d 3 /f
reg add "HKLM\SYSTEM\CurrentControlSet\Control\CrashControl" /v Overwrite /t REG_DWORD /d 1 /f

echo.
echo === 2. shorten the boot menu wait after a bad shutdown ===
rem  Without this XP stops at "Windows did not shut down successfully" and waits
rem  for a choice, which is the same as being down.
bootcfg /timeout 3 >nul 2>&1

echo.
echo === 3. start the agent at boot, as SYSTEM, before anyone logs in ===
rem  A scheduled task running as SYSTEM needs no password stored anywhere and no
rem  interactive session -- the agent is a network service, so it does not need a
rem  desktop. This is deliberately NOT AutoAdminLogon, which would put the
rem  account password in the registry in clear text on a networked machine.
schtasks /delete /tn xpagent /f >nul 2>&1
schtasks /create /tn xpagent /ru SYSTEM /sc onstart /tr "\"%~dp0start_xp_agent.bat\"" /f

echo.
echo === 4. verify ===
schtasks /query /tn xpagent
reg query "HKLM\SYSTEM\CurrentControlSet\Control\CrashControl" /v AutoReboot

echo.
echo Done. Reboot once to confirm the agent comes back on its own,
echo then check from the Linux side that it answers before trusting it.
echo.
pause
