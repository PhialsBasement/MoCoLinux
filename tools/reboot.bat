@echo off
rem  Reboot the box.
rem
rem  shutdown.exe is not in system32 on this machine -- only the copies under
rem  ServicePackFiles and the service-pack uninstall folder survive, so it is
rem  not on PATH and plain `shutdown` fails. Call the one that exists.
rem
rem  In a .bat rather than through the agent's argv, because backslashes in a
rem  path do not survive that: "%SystemRoot%\system32\shutdown.exe" arrived as
rem  "E:\WINDOWSsystem32shutdown.exe".
set SD=%SystemRoot%\ServicePackFiles\amd64\shutdown.exe
if not exist "%SD%" set SD=%SystemRoot%\$NtServicePackUninstall$\shutdown.exe
"%SD%" -r -t 3 -f
