' Put MoCoLinux's application shortcuts on the desktop, and start it at logon.
'
' Shortcuts rather than batch files on the desktop, because a .lnk can carry an
' icon, a working directory and a window style, and because double-clicking a
' .bat flashes a console window on its way to doing nothing visible.
'
' Each application shortcut runs moco-app.py through pythonw (no console window
' at all) and asks the guest's shell to start the program with DISPLAY pointed
' at the X server on this machine. The window that appears is an ordinary
' Windows window -- the X server is running -multiwindow, so to Windows these
' are its own windows, with taskbar entries and XP frames.
'
' Icons come from shell32.dll: the guest's own .desktop icons live inside the
' image and Windows cannot read them, and shipping a set of .ico files for
' applications the user may not have installed is worse than a recognisable
' stand-in. The terminal, editor and browser glyphs are close enough to be
' useful at a glance.
'
' Run it again after installing more applications; it overwrites what it made
' and leaves everything else alone.

Set shell = CreateObject("WScript.Shell")
Set fso   = CreateObject("Scripting.FileSystemObject")

' Where things are. The installer passes both directories, because it is the
' only thing that knows where the user chose to put them; the fallbacks are this
' development box's layout so the script is still runnable by hand.
If WScript.Arguments.Count >= 1 Then
	moco = WScript.Arguments(0)
Else
	' This script is installed beside the binaries the shortcuts point at, so
	' it asks where it is. The old fallback named this development box's own
	' staging directory, which on another machine is either absent or -- worse,
	' and seen on the XP side -- a stale build, so the shortcuts were written
	' aiming at binaries nobody meant to run.
	moco = Left(WScript.ScriptFullName, _
	            InStrRev(WScript.ScriptFullName, "\") - 1)
End If
If WScript.Arguments.Count >= 2 Then
	linuxdir = WScript.Arguments(1)
Else
	linuxdir = CreateObject("WScript.Shell") _
	           .ExpandEnvironmentStrings("%SystemDrive%") & "\MoCoLinux"
End If

desktop = shell.SpecialFolders("AllUsersDesktop")
startup = shell.SpecialFolders("AllUsersStartup")

' No Python anywhere. colinux-daemon --run sends the command to the guest's
' console through the driver, so a shortcut needs nothing but the daemon that is
' already installed -- the first version of this used a Python script and a
' socket, which worked only on a machine that happens to have Python, and a
' stock XP has none. Every icon would have silently done nothing.
daemon = moco & "\colinux-daemon.exe"

' name | command in the guest | icon
apps = Array( _
	"Konsole|konsole|shell32.dll,3", _
	"xterm|xterm|shell32.dll,3", _
	"Kate|kate|shell32.dll,70", _
	"Firefox|firefox|shell32.dll,14", _
	"Files (Dolphin)|dolphin|shell32.dll,4", _
	"System Monitor|plasma-systemmonitor|shell32.dll,24", _
	"System Settings|systemsettings|shell32.dll,21" )

made = 0
For Each entry In apps
	part = Split(entry, "|")
	Set lnk = shell.CreateShortcut(desktop & "\" & part(0) & " (Linux).lnk")
	lnk.TargetPath       = daemon
	' Every application goes through moco-gl, whether or not it draws with
	' OpenGL.
	'
	' The wrapper points the program's GL at /dev/dri/renderD128 -- virgl,
	' which is the host's real graphics card -- and pushes the finished
	' frames into the same X window. A program that never issues a GL call
	' loses nothing by being wrapped; one that does and is NOT wrapped gets
	' software rendering, or no GL at all, because software GLX against the
	' X server on Windows fails outright with GLXBadDrawable. Wrapping
	' everything is therefore the safe default, and it means a user never
	' has to know which of these applications happen to use the GPU.
	lnk.Arguments        = "--run moco-gl " & part(1)
	lnk.WorkingDirectory = moco
	lnk.IconLocation     = part(2)
	lnk.Description      = part(1) & ", running in MoCoLinux on the GPU"
	lnk.WindowStyle      = 7          ' minimised: nothing to look at
	lnk.Save
	made = made + 1
Next

' The terminal, which needs a console window of its own rather than hiding it.
Set lnk = shell.CreateShortcut(desktop & "\MoCoLinux Terminal.lnk")
' telnet, not a Python client: XP ships telnet.exe, and the console server
' negotiates SUPPRESS-GO-AHEAD and ECHO on connect precisely so a default telnet
' client behaves like a raw socket. One less thing the target machine must have.
lnk.TargetPath       = "telnet.exe"
lnk.Arguments        = "127.0.0.1 2323"
lnk.WorkingDirectory = moco
lnk.IconLocation     = "shell32.dll,3"
lnk.Description      = "A shell inside MoCoLinux, on this machine's console"
lnk.Save

' Start everything, by hand from the desktop...
Set lnk = shell.CreateShortcut(desktop & "\Start MoCoLinux.lnk")
lnk.TargetPath       = "wscript.exe"
lnk.Arguments        = """" & moco & "\moco-boot.vbs"" """ & moco & """ """ & linuxdir & """"
lnk.WorkingDirectory = moco
lnk.IconLocation     = "shell32.dll,43"
lnk.Description      = "Start MoCoLinux: driver, Linux, network and X server"
lnk.WindowStyle      = 7
lnk.Save

' ...and at logon, so Manjaro is simply up.
'
' Logon rather than a service at boot: the X server needs a desktop to put
' windows on, and a session that has one is the only place this makes sense.
Set lnk = shell.CreateShortcut(startup & "\MoCoLinux.lnk")
lnk.TargetPath       = "wscript.exe"
lnk.Arguments        = """" & moco & "\moco-boot.vbs"" """ & moco & """ """ & linuxdir & """"
lnk.WorkingDirectory = moco
lnk.IconLocation     = "shell32.dll,43"
lnk.Description      = "Start MoCoLinux at logon"
lnk.WindowStyle      = 7
lnk.Save

WScript.Echo "moco-icons: " & made & " application shortcuts + terminal + start on the desktop"
WScript.Echo "moco-icons: MoCoLinux will now start at logon"
WScript.Echo "moco-icons: launcher is " & daemon
