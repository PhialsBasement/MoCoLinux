@echo off
rem  Force the box off, now, whatever it thinks it is doing.
rem
rem  -f is the point: no "an application is preventing shutdown", no waiting for
rem  a wedged colinux-daemon to answer, no dialogs on a machine nobody is sitting
rem  at. Anything with unsaved state loses it, which is correct here -- the guest
rem  is expected to be shut down cleanly first if its filesystem matters, and
rem  this exists for the times it cannot be.
rem
rem  Three seconds rather than zero so this batch file and the transfer agent's
rem  pipe can close first, which is what lets the caller see the exit code
rem  instead of a dropped connection.
rem
rem  shutdown.exe is not in system32 on this machine -- only the copies under
rem  ServicePackFiles and the service-pack uninstall folder survive, so it is not
rem  on PATH and plain `shutdown` fails. Call the one that exists, and say so if
rem  neither does rather than exiting 0 having done nothing.
rem
rem  In a .bat rather than through the agent's argv because backslashes in a path
rem  do not survive that: "%SystemRoot%\system32\shutdown.exe" arrived as
rem  "E:\WINDOWSsystem32shutdown.exe".
rem
rem  What this is NOT: a way to recover a frozen host. If Windows has stopped
rem  scheduling, nothing running under Windows can help, and that includes this.
rem  Out-of-band power control on this machine would mean Intel AMT, whose ports
rem  are closed -- unprovisioned or unsupported -- and provisioning it needs
rem  Ctrl+P at boot with somebody in front of the machine. Failing that, a
rem  switched plug. See reboot.bat for the restart equivalent, and note that
rem  Wake-on-LAN to the Intel 82579LM (FC:4D:D4:2D:DB:4B) does work, so the
rem  machine can be brought back remotely once it is off.
set SD=%SystemRoot%\ServicePackFiles\amd64\shutdown.exe
if not exist "%SD%" set SD=%SystemRoot%\$NtServicePackUninstall$\shutdown.exe
if not exist "%SD%" (
	echo off: no shutdown.exe found under %SystemRoot%
	exit /b 1
)
echo off: powering down in 3 seconds
"%SD%" -s -t 3 -f
