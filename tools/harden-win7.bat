@echo off
rem ---------------------------------------------------------------------------
rem  Make the Windows 7 x64 side of the M92p come back on its own after a
rem  crash, load test-signed drivers, and run the transfer agent at logon.
rem
rem  Run once from an ELEVATED command prompt, in the xfer directory.
rem  (Right-click cmd.exe, Run as administrator -- unlike XP, a console started
rem  normally is not elevated even for an administrator account.)
rem
rem  The Windows 7 deltas against harden-m92p.bat, each of which strands the
rem  box if missed:
rem    * kernel-mode code signing: unsigned linux.sys will not load at all;
rem      testsigning mode accepts the project's self-made cert.
rem    * Startup Repair: after an unclean shutdown Win7 boots into a repair
rem      wizard and waits for a mouse click, which is the same as being down.
rem    * sleep: Win7 suspends after 30 minutes by default; XP never did.
rem    * UAC: the agent starts at logon unelevated, so sc/driver work fails
rem      with ACCESS_DENIED that XP never produced.
rem
rem  Set PEER to this machine's view of the Linux box, ROOT to the agent root
rem  (drive letters can differ from XP's view of the same disk!).
rem ---------------------------------------------------------------------------

set PEER=192.168.137.1
set ROOT=E:\xfer
set PORT=5000
rem  The account that logs in. NOT Administrator: Win7 ships the built-in
rem  Administrator disabled and the OOBE-created account is the admin here.
set LOGONUSER=phiality

echo.
echo === 1. reboot after a bugcheck instead of sitting on the blue screen ===
reg add "HKLM\SYSTEM\CurrentControlSet\Control\CrashControl" /v AutoReboot /t REG_DWORD /d 1 /f
reg add "HKLM\SYSTEM\CurrentControlSet\Control\CrashControl" /v CrashDumpEnabled /t REG_DWORD /d 3 /f
reg add "HKLM\SYSTEM\CurrentControlSet\Control\CrashControl" /v Overwrite /t REG_DWORD /d 1 /f

echo.
echo === 2. boot straight back into Windows 7, no menus, no repair wizard ===
rem  timeout 3: the dual-boot menu otherwise waits 30 s -- and a crash reboot
rem  must land HERE, so the default is pinned to this installation.
bcdedit /timeout 3
bcdedit /default {current}
rem  Without these two, one unclean shutdown parks the box in Startup Repair
rem  ("Windows Error Recovery") waiting for a keypress it will never get.
bcdedit /set {current} bootstatuspolicy ignoreallfailures
bcdedit /set {current} recoveryenabled no

echo.
echo === 3. load test-signed drivers ===
rem  KMCS refuses unsigned kernel modules on x64. linux.sys is signed by
rem  tools/sign-driver.sh with a self-made cert; testsigning mode skips chain
rem  validation so that signature is enough. Desktop watermark is the price.
bcdedit /set {current} testsigning on

echo.
echo === 4. never sleep ===
rem  Default power plan suspends after 30 minutes. A suspended box does not
rem  answer WoL from S3 reliably on this NIC and does not run the agent.
powercfg -h off
powercfg -change -standby-timeout-ac 0
powercfg -change -monitor-timeout-ac 20
powercfg -change -disk-timeout-ac 0

echo.
echo === 5. UAC off, XP semantics back ===
rem  The agent runs at logon in a plain console; with UAC on it is a filtered
rem  token and every sc create / sc start / driver copy fails ACCESS_DENIED.
rem  Takes effect at the next reboot, which step 9 asks for anyway.
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System" /v EnableLUA /t REG_DWORD /d 0 /f

echo.
echo === 6. firewall: let the agent and ping through ===
rem  XP's firewall was permissive on this link; Win7's blocks both by default.
netsh advfirewall firewall add rule name="xfer agent %PORT%" dir=in action=allow protocol=TCP localport=%PORT%
netsh advfirewall firewall add rule name="icmp echo in" dir=in action=allow protocol=icmpv4:8,any

echo.
echo === 7. internet through the Linux box ===
rem  The link's dnsmasq deliberately hands out no gateway and no DNS (the XP
rem  side never wanted them), so both go static here: the Linux machine NATs
rem  this subnet out its own uplink. Same IP the DHCP lease already gave, so
rem  nothing about the agent connection changes.
netsh interface ip set address "Local Area Connection" static 192.168.137.54 255.255.255.0 192.168.137.1
netsh interface ip set dns "Local Area Connection" static 1.1.1.1
netsh interface ip add dns "Local Area Connection" 8.8.8.8 index=2

echo.
echo === 8. telnet client, for the guest terminal shortcut ===
rem  XP ships telnet.exe; on Win7 it is an optional feature, off by default.
rem  Not fatal if this fails -- only the terminal desktop icon needs it.
dism /online /Enable-Feature /FeatureName:TelnetClient /NoRestart

echo.
echo === 9. write the launcher and start it at logon ===
rem  py launcher, same as the XP side. Install Python 3.8.x (the last CPython
rem  that runs on Windows 7) before rebooting, or the agent will not start.
> "%~dp0run-agent.bat" echo @echo off
>>"%~dp0run-agent.bat" echo title win7 agent
>>"%~dp0run-agent.bat" echo cd /d "%~dp0"
>>"%~dp0run-agent.bat" echo py "%~dp0xp_agent.py" --run --root "%ROOT%" --port %PORT% --peer %PEER%
>>"%~dp0run-agent.bat" echo echo.
>>"%~dp0run-agent.bat" echo echo agent exited -- window kept open on purpose
>>"%~dp0run-agent.bat" echo pause
type "%~dp0run-agent.bat"
rem  Startup folder moved since XP: it is under ProgramData now.
copy /y "%~dp0run-agent.bat" "%ProgramData%\Microsoft\Windows\Start Menu\Programs\Startup\run-agent.bat"

echo.
echo === 10. log on without a human ===
rem  Same Winlogon keys as XP; the password sits in the registry in clear
rem  text because that is how AutoAdminLogon works.
set "LOGONPASS="
set /p LOGONPASS=  Password for %LOGONUSER% (blank to skip autologon):
if "%LOGONPASS%"=="" (
  echo.
  echo   Skipped. Everything else is set, but the box will stop at the login
  echo   prompt after a crash and stay unreachable. Re-run to finish.
  goto done
)
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v AutoAdminLogon /t REG_SZ /d 1 /f
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v DefaultUserName /t REG_SZ /d "%LOGONUSER%" /f
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v DefaultPassword /t REG_SZ /d "%LOGONPASS%" /f
reg delete "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v AutoLogonCount /f >nul 2>&1
echo   autologon set for %LOGONUSER%

:done
echo.
echo Manual follow-ups this script cannot do:
echo   * Device Manager, NIC, Power Management: "Allow this device to wake
echo     the computer" + "Only allow a magic packet" (WoL is per-OS driver
echo     config; the XP side's setting does not carry over).
echo   * Install Python 3.8.x if not done yet, with the py launcher.
echo   * Reboot once (UAC + testsigning need it) and confirm from the Linux
echo     side that the box answers by itself.
echo.
pause
