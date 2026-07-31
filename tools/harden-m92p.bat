@echo off
rem ---------------------------------------------------------------------------
rem  Make the M92p come back on its own after a crash.
rem
rem  Run once from an elevated command prompt, in F:\xfer.
rem
rem  No scheduled tasks. The agent runs in a console window at logon, the same
rem  way it gets run by hand, so its output is visible and it can be killed and
rem  restarted normally.
rem
rem  Set PEER to this machine's view of the Linux box, and USERNAME/PASSWORD to
rem  the account that logs in, before running.
rem ---------------------------------------------------------------------------

set PEER=192.168.137.1
set ROOT=F:\xfer
set PORT=5000

set LOGONUSER=Administrator
set LOGONPASS=

echo.
echo === 1. reboot after a bugcheck instead of sitting on the blue screen ===
reg add "HKLM\SYSTEM\CurrentControlSet\Control\CrashControl" /v AutoReboot /t REG_DWORD /d 1 /f
reg add "HKLM\SYSTEM\CurrentControlSet\Control\CrashControl" /v CrashDumpEnabled /t REG_DWORD /d 3 /f
reg add "HKLM\SYSTEM\CurrentControlSet\Control\CrashControl" /v Overwrite /t REG_DWORD /d 1 /f

echo.
echo === 2. do not stop at the recovery menu after a bad shutdown ===
rem  Otherwise XP waits for a keypress, which is the same as being down.
bootcfg /timeout 3 >nul 2>&1

echo.
echo === 3. write the launcher ===
rem  python, not pythonw: it should have a console so you can see it and stop it.
rem  It stays in the foreground of its own window, exactly like running it by hand.
> "%~dp0run-agent.bat" echo @echo off
>>"%~dp0run-agent.bat" echo title xp agent
>>"%~dp0run-agent.bat" echo cd /d "%~dp0"
>>"%~dp0run-agent.bat" echo python "%~dp0xp_agent.py" --run --root "%ROOT%" --port %PORT% --peer %PEER%
>>"%~dp0run-agent.bat" echo echo.
>>"%~dp0run-agent.bat" echo echo agent exited -- window kept open on purpose
>>"%~dp0run-agent.bat" echo pause
type "%~dp0run-agent.bat"

echo.
echo === 4. start it at logon, in a console ===
copy /y "%~dp0run-agent.bat" "%ALLUSERSPROFILE%\Start Menu\Programs\Startup\run-agent.bat"

echo.
echo === 5. log on without a human ===
if "%LOGONPASS%"=="" (
  echo   LOGONPASS is blank -- skipping autologon.
  echo   Set it at the top of this file and re-run if you want the box to come
  echo   all the way back on its own. It goes into the registry in clear text,
  echo   which is why it is not filled in for you.
) else (
  reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v AutoAdminLogon /t REG_SZ /d 1 /f
  reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v DefaultUserName /t REG_SZ /d "%LOGONUSER%" /f
  reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v DefaultPassword /t REG_SZ /d "%LOGONPASS%" /f
  echo   autologon set for %LOGONUSER%
)

echo.
echo === 6. start it now, so you can see it work ===
start "" "%~dp0run-agent.bat"

echo.
echo Reboot once and confirm from the Linux side that it answers by itself.
echo.
pause
