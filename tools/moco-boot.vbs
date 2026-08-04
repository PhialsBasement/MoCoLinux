' Bring MoCoLinux up, with nothing to look at.
'
' This is what the desktop shortcut and the logon entry both run. The three
' daemons it starts are services, not applications: the boot daemon spends the
' whole session inside one ioctl, the bridge relays packets and the console
' server waits for a client. None of them has anything for a person to read, and
' a minimised window is still a taskbar button in the way of whatever the user
' is actually doing -- so they are started hidden.
'
' A .vbs rather than a .bat because there is no way to start a detached, windowless
' child from cmd that does not either flash a console (`start /min`) or hand its
' own console handles to the child and wedge (`start /b`). This project has lost
' an evening to the second one; xstart1142.bat goes through WScript.Shell for the
' same reason.
'
' The only window that ever appears is the terminal, and only because somebody
' opened it from its own shortcut.
'
' Everything here is idempotent. Run it twice and the second run finds the guest
' already up and leaves it alone -- which matters more than tidiness: two boot
' daemons means two lots of contiguous guest RAM, and that has taken this box
' down every time it has happened. The daemons refuse to start twice on their
' own account as well (co_os_claim_single_instance), so this is the outer of two
' guards, not the only one.

Const HIDDEN = 0
Const SHOWN = 1
Const WAIT = True
Const NOWAIT = False

' Where things are. The shortcuts pass both directories, because only the
' installer knows where the user chose to put them; the fallbacks are this
' development box's layout so this is still runnable by hand.
If WScript.Arguments.Count >= 1 Then
	moco = WScript.Arguments(0)
Else
	moco = "F:\xfer\mocolinux-m2"
End If
If WScript.Arguments.Count >= 2 Then
	linux = WScript.Arguments(1)
Else
	linux = "E:\MoCoLinux"
End If

Set shell = CreateObject("WScript.Shell")
Set wmi   = GetObject("winmgmts:\\.\root\cimv2")

Function Running(name)
	Running = wmi.ExecQuery( _
		"select ProcessId from Win32_Process where Name='" & name & "'").Count > 0
End Function

' Start the X server, passing the program directory.
'
' The doubled outer quotes are required and are not a typo. `cmd /c` has a
' documented rule: when what follows /c begins with a quote and contains more
' than one quoted token, it strips the first and last quote character and runs
' what is left. So the obvious
'
'     cmd /c "C:\Program Files\MoCoLinux\xstart1142.bat" "C:\Program Files\MoCoLinux"
'
' arrives as C:\Program Files\...bat" "C:\Program Files\MoCoLinux and dies on
' the first space -- 'C:\Program' is not recognized. Wrapping the whole command
' line in one more pair defeats the stripping, because the pair cmd removes is
' the one that was added for it. This cost a silent X server twice: the failure
' happens inside a hidden window, so a Linux application simply never appears.
'
' Chr(34) rather than escaped quotes, so the count is legible.
Sub StartX(dir)
	q = Chr(34)
	shell.Run "cmd /c " & q & q & dir & "\xstart1142.bat" & q & _
		  " " & q & dir & q & q, HIDDEN, NOWAIT
End Sub

shell.CurrentDirectory = moco

' The driver. --install-driver only creates the service; sc start is what loads
' it, and it says 1056 harmlessly when it is already loaded. Both waited on:
' nothing below can work until the driver is there.
shell.Run "cmd /c """ & moco & "\colinux-daemon.exe"" --install-driver", HIDDEN, WAIT
shell.Run "cmd /c sc start CoLinuxDriver", HIDDEN, WAIT

If Running("colinux-daemon.exe") Then
	' Already up. Still make sure the X server is there, because it can be
	' closed independently of the guest and an X client with no server
	' produces no error at all -- it simply never appears.
	StartX moco
	WScript.Quit 0
End If

' Linux. No switch limit and no deadline: this is a session somebody is using,
' and it ends when the guest powers off or stop.bat asks it to.
'
' Shown, unlike the other two. This is the one child with something worth
' reading: it narrates the load, streams the guest's kernel log as it boots,
' and when a run ends it dumps that log -- which is the evidence for every
' failure that leaves no bugcheck behind. Hidden, all of that goes nowhere and
' a guest that dies during boot looks identical to one that never started.
' Closing its window ends the guest, which is the same as pulling the plug, so
' stop.bat remains the way to shut down.
' \DosDevices\, never \??\. They name the same NT object directory, but the
' daemon's msvcrt CRT expands ? as a wildcard in argv before main() runs, so a
' \??\ path whose pattern matches anything on disk arrives as its own basename
' and the attach fails with "cannot attach cobd0 to '\root.img'". It only bites
' when a match exists, which is why F: paths worked for days and the first C:
' one did not.
shell.Run """" & moco & "\colinux-daemon.exe"" --boot-kernel vmlinux" & _
	" --max-switches none" & _
	" --cobd0 \DosDevices\" & linux & "\root.img" & _
	" --cobd1 \DosDevices\" & linux & "\root-arch.img" & _
	" --init /sbin/init", SHOWN, NOWAIT

' The network bridge and the terminal server. Both may be started before the
' guest has finished booting -- they wait for it -- and both exit on their own
' once it is gone, so nothing here has to clean them up.
shell.Run """" & moco & "\colinux-slirp-net-daemon.exe"" -R", HIDDEN, NOWAIT

' The GPU daemon: the host side of the guest's virtio-gpu device. Hidden like
' the other two -- nothing for a person to read, and it writes its own log
' beside the images. After the guest, because it maps the guest's RAM, which
' does not exist until the boot daemon has allocated it. The transport
' tolerates a late daemon by design, so this ordering is safe rather than
' merely convenient.
shell.Run """" & moco & "\cogpu-daemon.exe"" --verbose", HIDDEN, NOWAIT
shell.Run """" & moco & "\colinux-daemon.exe"" --console 2323", HIDDEN, NOWAIT

' The X server last, so the guest's windows have somewhere to go. It checks for
' itself whether one is already running on :0.
StartX moco
