@echo off
rem  Start the X server the guest's applications will appear on, and return.
rem
rem  This is the i386 coLinux arrangement: an X server on the Windows side in
rem  multiwindow mode, so a guest application is an ordinary Windows window with
rem  no desktop-inside-a-desktop and no framebuffer anywhere. The guest has no X
rem  server of its own and needs none -- only the client libraries, which the
rem  root filesystem already carries.
rem
rem  How the connection arrives: the guest has DISPLAY=10.0.2.2:0, slirp's
rem  gateway alias, and slirp rewrites a connection to that address into one to
rem  the host's own loopback -- tcp_fconnect() in user/slirp/tcp_subr.c, the
rem  CTL_ALIAS case. So this server is reached on 127.0.0.1:6000, with no port
rem  redirection, nothing new in the driver, and nothing listening anywhere the
rem  network can see.
rem
rem  -wgl          use the machine's own OpenGL -- the GT730's driver -- instead
rem                of the server's software renderer.
rem  +iglx         allow indirect GLX, which is what actually gets GL commands
rem                to the card. It has been off by default since xorg 1.17 and
rem                -wgl is useless here without it: the guest has no GPU and no
rem                DRI device, so its Mesa renders with llvmpipe in its own CPU
rem                and sends finished pixels. Nothing the server does can
rem                accelerate that, because by the time the server sees it the
rem                drawing is already done. The guest has to speak GLX protocol
rem                instead, which needs LIBGL_ALWAYS_INDIRECT=1 on its side.
rem  -multiwindow  rootless: one Windows window per X window.
rem  -ac           no access control. Required, because guest and host share no
rem                xauth cookie and there is nowhere to put one. Safe only
rem                because of the loopback rewrite above: by the time a
rem                connection reaches this server it came from this machine.
rem  -clipboard    share the Windows clipboard both ways.
rem  -notrayicon   the box is headless most of the time.
rem
rem  Why this goes through WScript.Shell instead of `start`.
rem
rem  `start` hands its own standard handles to the child whatever you do with
rem  them -- with /b, without /b, and with the child's stdio redirected to nul.
rem  All three were tried and all three wedge: the X server inherits the pipe
rem  the transfer agent is reading, the agent waits for an end-of-file that
rem  cannot come while the server lives, and the call never returns. One agent
rem  thread is lost, and worse, whoever launched it has no way to tell whether
rem  the server came up -- which is how a second one gets started by hand, and
rem  duplicate processes are what made this box unusable more than once tonight.
rem  dbg-start.bat carries the same note and solves it by never being waited on,
rem  which only moves the problem to the caller.
rem
rem  WScript.Shell's Run creates the process without inheriting this console's
rem  handles, so the pipe closes when this batch file exits and the caller gets
rem  its exit code immediately. 0 = hidden window, False = do not wait.
rem
rem  And it refuses to start a second one, rather than letting it fail.
rem
rem  A second server cannot bind port 6000, so it dies -- but it dies with a
rem  fatal-error box on a machine that is usually headless, and the error reads
rem  like something is wrong when nothing is. Every other daemon in this tree
rem  now refuses duplicate instances for the same reason; this is the same rule
rem  applied to the one process that is not ours.
setlocal
set XVBS=%TEMP%\mocolinux-xstart.vbs
set XEXE=E:\Program Files\VcXsrv\vcxsrv.exe

if not exist "%XEXE%" (
	echo xstart: %XEXE% is not there -- is VcXsrv installed?
	exit /b 1
)

tasklist /fi "imagename eq vcxsrv.exe" 2>nul | find /i "vcxsrv.exe" >nul
if not errorlevel 1 (
	echo xstart: an X server is already running on :0 -- leaving it alone
	exit /b 0
)

echo CreateObject("WScript.Shell").Run """%XEXE%"" :0 -multiwindow -wgl +iglx -ac -clipboard -notrayicon", 0, False > "%XVBS%"
cscript //nologo "%XVBS%"
echo xstart: X server launched on :0 (127.0.0.1:6000)
exit /b 0
