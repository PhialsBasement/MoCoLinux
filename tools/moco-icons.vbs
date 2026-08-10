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
' Two GPU tests, one per API, because the two stacks fail independently and a
' user needs to know WHICH one is broken. glxgears goes through the patched
' Mesa's virgl driver; vkcube goes through Venus to the host's own Vulkan
' driver -- and then both frames reach the screen through the same presenter.
' Both animate when the GPU path is healthy; a still picture means the host
' is not presenting (see --no-present in moco-boot.vbs).
'
' vkcube takes no arguments on purpose. Presentation is fire-and-forget, so
' FIFO is not throttled and the default present mode runs: measured 326 FPS
' at 500x500 on a GT 730.
'
' The comment lives here rather than inside the array because VBScript does
' not allow a comment between a line-continuation underscore and the line it
' continues -- it is a syntax error, and a syntax error in this script means
' an install with no shortcuts at all.
apps = Array( _
	"Konsole|konsole|shell32.dll,3", _
	"xterm|xterm|shell32.dll,3", _
	"Kate|kate|shell32.dll,70", _
	"Firefox|firefox|shell32.dll,14", _
	"Files (Dolphin)|dolphin|shell32.dll,4", _
	"System Monitor|plasma-systemmonitor|shell32.dll,24", _
	"System Settings|systemsettings|shell32.dll,21", _
	"GPU test (OpenGL)|glxgears|shell32.dll,18", _
	"GPU test (Vulkan)|vkcube|shell32.dll,18" )

made = 0
For Each entry In apps
	part = Split(entry, "|")
	Set lnk = shell.CreateShortcut(desktop & "\" & part(0) & " (Linux).lnk")
	lnk.TargetPath       = daemon
	' The application is launched directly. There is nothing to wrap.
	'
	' These shortcuts used to run everything through moco-gl, which ran
	' VirtualGL, because that was the only way a program's OpenGL reached
	' the host's card. CoPresent replaced it: the patched Mesa in the root
	' filesystem IS the system's GL driver, so every process gets the GPU
	' whether or not anything remembered a prefix. Wrapping is not merely
	' unnecessary now, it was actively harmful -- vglrun exported LD_PRELOAD
	' into every child process, and the faker it preloaded pulled in
	' libturbojpeg, which is what stopped Steam's own helper scripts from
	' running at all.
	'
	' moco-gl still exists in the guest as a no-op, so shortcuts written by
	' an older release keep working.
	'
	' Quoted, because --run takes ONE argument. The daemon parses this with
	' the single-argument form, so an unquoted "--run xterm -geometry ..."
	' hands it the command "xterm" and leaves the rest as stray tokens.
	lnk.Arguments        = "--run " & Chr(34) & part(1) & Chr(34)
	lnk.WorkingDirectory = moco
	lnk.IconLocation     = part(2)
	lnk.Description      = part(1) & ", running in MoCoLinux on the GPU"
	lnk.WindowStyle      = 7          ' minimised: nothing to look at
	lnk.Save
	made = made + 1
Next

' The terminal, which needs a console window of its own rather than hiding it.
Set lnk = shell.CreateShortcut(desktop & "\MoCoLinux Terminal.lnk")
' Through moco-term.bat, with an absolute path, for two separate reasons.
'
' A bare "telnet.exe" is not a path, and CreateShortcut resolves what it is
' given: with no telnet on PATH it wrote C:\Users\<name>\Desktop\telnet.exe --
' the creating process's directory -- into the .lnk, naming a file that has
' never existed on any machine. Absolute paths cannot do that.
'
' And telnet itself is no longer a given. Telnet Client has been an optional
' Windows feature, off by default, since Vista; 8.1 has no telnet.exe at all
' until somebody turns it on. The .bat detects that, offers to enable it, and
' says plainly what to tick if the elevation prompt is refused, instead of
' flashing a console and vanishing.
lnk.TargetPath       = moco & "\moco-term.bat"
lnk.Arguments        = ""
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
