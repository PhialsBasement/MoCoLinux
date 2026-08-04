@echo off
rem  Start VcXsrv 1.14.2.1, the last release that runs on Windows XP.
rem
rem  Why this version and not the one already installed on this box. VcXsrv
rem  1.20.14's vcxsrv.exe declares MajorSubsystemVersion 6.0, so a 5.2 loader
rem  refuses the image outright, and it wants vcruntime140 plus the Universal
rem  CRT besides. It runs here only because this machine has one-core-api,
rem  which is not something a release can assume. 1.14.2.1 is the last build
rem  before that changed: every PE in its installer declares 5.2, and the only
rem  runtime it needs -- MSVCR100, the Visual C++ 2010 redistributable, which
rem  supports XP -- ships inside the installer directory.
rem
rem  And the flag that had to go. xstart.bat passes +iglx, which turns indirect
rem  GLX back on; it exists because xorg-server 1.17 disabled IGLX by default.
rem  This server is xorg-server 1.14, which predates that entirely -- the
rem  string "iglx" does not appear in the binary at all. Indirect GLX is simply
rem  on, so the option is unnecessary here and passing it is a fatal-error
rem  dialog on a machine that is usually headless.
rem
rem  Everything else is as before, and the reasoning is in xstart.bat: -wgl for
rem  the host's own OpenGL, -multiwindow for one Windows window per X window,
rem  -ac because guest and host share no xauth cookie and slirp's loopback
rem  rewrite means a connection reaching here came from this machine anyway,
rem  -clipboard both ways, -notrayicon because the box is headless.
rem
rem  WScript.Shell rather than `start`, for the reason xstart.bat spells out at
rem  length: `start` hands its console handles to the child whatever you do,
rem  the caller then waits for an end-of-file that cannot come, and the call
rem  never returns.
rem  Where the server is, asked rather than assumed.
rem
rem  This said F:\xfer\mocolinux-m2\vcxsrv1142 until 2026-08-04, which is this
rem  development box and nowhere else, so every installed machine took the "is
rem  not there" branch -- and moco-boot.vbs runs this hidden, so the
rem  explanation went to a console nobody sees. The symptom is the worst shape
rem  available: Linux applications start, connect to nothing and never appear,
rem  with no error anywhere on either side.
rem
rem  Four sources, in order of how much they know:
rem    1. an argument, which is what moco-boot.vbs passes -- the shortcuts hand
rem       it both directories, so it is the only caller that knows for certain;
rem    2. mocolinux.ini, which is the installer's own record of where it put
rem       things and is authoritative for an installed machine;
rem    3. beside this script, which is true whenever the two travel together;
rem    4. the development boxes, so this is still runnable by hand.
setlocal
set XVBS=%TEMP%\mocolinux-xstart1142.vbs
set PROG=%~1
if not "%PROG%"=="" goto have_prog

rem  The ini lives with the images, in the Linux directory. Its default is the
rem  root of the system drive; the program directory is written inside it.
set INI=%SystemDrive%\MoCoLinux\mocolinux.ini
if not exist "%INI%" set INI=%~dp0mocolinux.ini
if exist "%INI%" for /f "usebackq tokens=1,* delims==" %%a in (`findstr /b /i "program=" "%INI%"`) do set PROG=%%b
if not "%PROG%"=="" goto have_prog

set PROG=%~dp0

:have_prog
rem  %~dp0 ends in a backslash and an ini value does not; normalise so the
rem  path below is built the same way whichever source answered.
if "%PROG:~-1%"=="\" set PROG=%PROG:~0,-1%

set XEXE=%PROG%\vcxsrv1142\vcxsrv.exe
if not exist "%XEXE%" set XEXE=F:\xfer\mocolinux-m2\vcxsrv1142\vcxsrv.exe
if not exist "%XEXE%" set XEXE=E:\xfer\mocolinux-m2\vcxsrv1142\vcxsrv.exe

if not exist "%XEXE%" (
	echo xstart1142: no vcxsrv.exe under "%PROG%" or on the fallback paths
	exit /b 1
)

tasklist /fi "imagename eq vcxsrv.exe" 2>nul | find /i "vcxsrv.exe" >nul
if not errorlevel 1 (
	echo xstart1142: an X server is already running on :0 -- leaving it alone
	exit /b 0
)

echo CreateObject("WScript.Shell").Run """%XEXE%"" :0 -multiwindow -wgl -ac -clipboard -notrayicon", 0, False > "%XVBS%"
cscript //nologo "%XVBS%"
echo xstart1142: VcXsrv 1.14.2.1 launched on :0 (127.0.0.1:6000)
exit /b 0
