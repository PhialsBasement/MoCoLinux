@echo off
rem  Murder a running guest, cleanly.
rem
rem  Sets the abort flag the monitor loop checks on every crossing -- the same
rem  mechanism driver unload uses. The run then ends through its own exit: the
rem  daemon that booted the guest prints its full report (kernel log, network
rem  rings, all of it) and tears down normally. For a guest that is wedged but
rem  alive, this replaces waiting out the fifteen-minute deadline or walking
rem  to the power button.
rem
rem  %~dp0 is this script's own directory. It said F:\xfer\mocolinux-m2 until
rem  2026-08-05, which is where the development box keeps its build -- so on an
rem  installed machine the cd failed, and the daemon that ran afterwards was
rem  whichever one happened to be on PATH, or none at all. The same fallback
rem  appeared in moco-boot.vbs, moco-icons.vbs and xstart1142.bat; all four
rem  now locate themselves.
cd /d "%~dp0"
colinux-daemon.exe --stop
