@echo off
rem  Put this box back on the development network.
rem
rem  The wired adapter is given a fixed address on the 192.168.137.0/24 link the
rem  development machine shares over ICS, which is where the transfer agent is
rem  reached and where every remote session in this project begins. A fresh
rem  Windows install comes up on DHCP, finds no server on that link, falls back
rem  to a 169.254 address and is then invisible -- so this is the first thing to
rem  run after installing an OS on the M92p, before anything else can be done
rem  remotely.
rem
rem  Run as Administrator. On NT 6 and later a console started normally is not
rem  elevated even for an administrator account, so right-click, Run as
rem  administrator -- netsh fails quietly-ish otherwise and the address does not
rem  stick.
rem
rem  Safe to run twice: netsh set is idempotent, and re-applying the same
rem  address is not an error.

setlocal

rem  The adapter's name, which is not the same on every Windows. XP and 7 call
rem  it "Local Area Connection"; 8.1 and 10 call it "Ethernet". Rather than
rem  guess, take the first connected wired adapter and use whatever it is
rem  called. %%i..%%k because the name itself can contain spaces.
rem  Filtered on "Dedicated" -- the type column -- rather than on the state,
rem  because findstr matches substrings and "Disconnected" contains
rem  "Connected". Filtering on the state word would happily pick an unplugged
rem  adapter and configure that instead. The state is checked exactly, in the
rem  loop, where a full-token comparison is possible.
set IF=
for /f "tokens=1,2,3,*" %%i in ('netsh interface show interface ^| findstr /i "Dedicated"') do (
	if /i "%%j"=="Connected" if not defined IF set IF=%%l
)

if not defined IF (
	echo setup-net: no connected wired adapter found.
	echo            Check the cable, then: netsh interface show interface
	exit /b 1
)

echo Adapter : "%IF%"

rem  Address, mask, gateway. The gateway is the development machine's ICS end
rem  of the link; without it this box has a network but no route off it, which
rem  matters because the guest's package installs come through here.
netsh interface ip set address name="%IF%" static 192.168.137.54 255.255.255.0 192.168.137.1 1
if errorlevel 1 (
	echo setup-net: setting the address failed -- not elevated?
	exit /b 1
)

rem  Public resolvers rather than the gateway's. ICS does run a DNS proxy, but
rem  it answers only while the sharing host is up and awake, and a box that
rem  cannot resolve looks exactly like a box that cannot route.
netsh interface ip set dns name="%IF%" static 1.1.1.1 primary
netsh interface ip add dns name="%IF%" 8.8.8.8 index=2

echo.
echo Configured. Verifying:
netsh interface ip show config name="%IF%" | findstr /i "IP Address Gateway DNS"

echo.
echo Reaching the gateway:
ping -n 2 192.168.137.1 | findstr /i "Reply Request timed"

echo.
echo If the gateway does not answer, the development machine either is not
echo sharing its connection or is on a different subnet. Nothing here can tell
echo those apart from this end.

endlocal
