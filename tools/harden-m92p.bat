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

rem  The account the box logs straight into. Leave LOGONPASS empty if it has
rem  no password.
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
rem  py, because that is what is on this box -- there is no python.exe or
rem  pythonw.exe on PATH. Also the right choice regardless: it keeps a console,
rem  so the agent is visible and killable exactly as when run by hand.
> "%~dp0run-agent.bat" echo @echo off
>>"%~dp0run-agent.bat" echo title xp agent
>>"%~dp0run-agent.bat" echo cd /d "%~dp0"
>>"%~dp0run-agent.bat" echo py "%~dp0xp_agent.py" --run --root "%ROOT%" --port %PORT% --peer %PEER%
>>"%~dp0run-agent.bat" echo echo.
>>"%~dp0run-agent.bat" echo echo agent exited -- window kept open on purpose
>>"%~dp0run-agent.bat" echo pause
type "%~dp0run-agent.bat"

echo.
echo === 4. start it at logon, in a console ===
copy /y "%~dp0run-agent.bat" "%ALLUSERSPROFILE%\Start Menu\Programs\Startup\run-agent.bat"

echo.
echo === 5. log on without a human ===
rem  This is the step that makes the whole thing work. Without it the box boots
rem  to a login prompt and is just as unreachable as before.
rem
rem  A blank LOGONPASS is fine if the account has no password -- XP allows
rem  autologon with an empty DefaultPassword, and LimitBlankPasswordUse only
rem  restricts network logons, not the console one this uses.
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v AutoAdminLogon /t REG_SZ /d 1 /f
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v DefaultUserName /t REG_SZ /d "%LOGONUSER%" /f
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v DefaultPassword /t REG_SZ /d "%LOGONPASS%" /f
reg delete "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v AutoLogonCount /f >nul 2>&1
rem  Winlogon decrements AutoLogonCount and stops autologging in when it hits
rem  zero, so a stale one from anything else would make this work once.
echo   autologon set for %LOGONUSER%

echo.
echo === 6. start it now, so you can see it work ===
start "" "%~dp0run-agent.bat"

echo.
echo Reboot once and confirm from the Linux side that it answers by itself.
echo.
pause
