@echo off
rem  A shell inside MoCoLinux, on this machine's console.
rem
rem  The console server listens on 127.0.0.1:2323 and negotiates SUPPRESS-GO-AHEAD
rem  and ECHO on connect, so a stock telnet client behaves like a raw socket. That
rem  is why telnet was chosen: XP ships it, and it meant one less thing the target
rem  machine had to have.
rem
rem  Windows stopped shipping it enabled. Telnet Client has been an optional
rem  feature since Vista and is OFF by default, so on 8.1 there is no telnet.exe
rem  at all -- and the shortcut that pointed at a bare "telnet.exe" resolved it
rem  against the creating process's directory, wrote C:\Users\<name>\Desktop\
rem  telnet.exe into the .lnk, and opened nothing for the rest of the install's
rem  life. Both halves of that are fixed here: the path is absolute, and the
rem  missing-feature case is handled instead of assumed away.

setlocal
set TELNET=%SystemRoot%\System32\telnet.exe

if exist "%TELNET%" goto run

echo.
echo  The Telnet Client is not installed on this Windows.
echo.
echo  It is an optional Windows feature, off by default since Vista. MoCoLinux
echo  uses it to reach the guest's console on 127.0.0.1 port 2323.
echo.
echo  Turning it on now. This needs Administrator; if the prompt is refused, or
echo  nothing happens, enable it by hand:
echo.
echo      Control Panel  -^>  Programs and Features
echo      -^>  Turn Windows features on or off
echo      -^>  tick "Telnet Client"  -^>  OK
echo.

rem  Elevate for dism. A shortcut runs with the filtered token even for an
rem  administrator, so this cannot be done in place -- ShellExecute with the
rem  "runas" verb is what raises the prompt. Waiting is not possible through
rem  ShellExecute here, so the install is re-checked below rather than assumed.
powershell -NoProfile -Command "Start-Process dism.exe -ArgumentList '/online','/enable-feature','/featurename:TelnetClient','/norestart' -Verb runas -Wait" 2>nul

if exist "%TELNET%" goto run

echo.
echo  Still not installed. Enable it by hand as above, then run this again.
echo.
pause
exit /b 1

:run
rem  Not "start": this window IS the terminal, and the client should own it.
"%TELNET%" 127.0.0.1 2323
endlocal
