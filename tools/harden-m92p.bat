@echo off
rem ---------------------------------------------------------------------------
rem  Make the M92p recoverable without physical access.
rem
rem  Run once from an elevated command prompt, in F:\xfer.
rem
rem  The problem: a bugcheck or hang during driver work leaves the box off the
rem  network until someone walks to it, powers it on, logs in, and starts the
rem  agent by hand. Three manual steps, any one of which means it is simply gone.
rem
rem  Set PEER below to this machine's view of the Linux box before running.
rem ---------------------------------------------------------------------------

set PEER=192.168.137.1
set ROOT=F:\xfer
set PORT=5000

echo.
echo === 1. reboot automatically after a bugcheck, and keep a minidump ===
reg add "HKLM\SYSTEM\CurrentControlSet\Control\CrashControl" /v AutoReboot /t REG_DWORD /d 1 /f
reg add "HKLM\SYSTEM\CurrentControlSet\Control\CrashControl" /v CrashDumpEnabled /t REG_DWORD /d 3 /f
reg add "HKLM\SYSTEM\CurrentControlSet\Control\CrashControl" /v Overwrite /t REG_DWORD /d 1 /f

echo.
echo === 2. do not stop at the recovery menu after a bad shutdown ===
rem  Otherwise XP waits for a keypress, which is the same as being down.
bootcfg /timeout 3 >nul 2>&1

echo.
echo === 3. write the launcher ===
rem  A batch file, not a schtasks /TR argument.
rem
rem  xp_agent.py --install builds one long /TR string and escapes its inner
rem  quotes as \" -- that is the MSVCRT convention, not cmd.exe's, so schtasks
rem  receives a mangled command. Pointing /TR at a bare path with no spaces and
rem  no quotes avoids the question entirely, and cmd.exe parses the quoting
rem  inside the batch file normally.
> "%~dp0run-agent.bat" echo @echo off
>>"%~dp0run-agent.bat" echo pythonw "%~dp0xp_agent.py" --run --root "%ROOT%" --port %PORT% --peer %PEER%
type "%~dp0run-agent.bat"

echo.
echo === 4. run it at boot, as SYSTEM, before anyone logs in ===
schtasks /Delete /TN xpagent /F >nul 2>&1
schtasks /Create /F /TN xpagent /SC ONSTART /RU SYSTEM /TR %~dp0run-agent.bat
if errorlevel 1 goto fallback

echo.
echo Scheduled task created. Starting it now to check it works:
schtasks /Run /TN xpagent
goto verify

:fallback
echo.
echo schtasks refused. Falling back to the machine Run key, which starts the
echo agent at LOGON rather than at boot -- so the box still needs someone to log
echo in, but not to start anything by hand.
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run" /v xpagent /t REG_SZ /d "%~dp0run-agent.bat" /f

:verify
echo.
echo === 5. verify ===
schtasks /Query /TN xpagent 2>nul
reg query "HKLM\SYSTEM\CurrentControlSet\Control\CrashControl" /v AutoReboot
echo.
echo Now reboot once and confirm from the Linux side that it answers on its own
echo before trusting it. If it does not, say what this printed.
echo.
pause
