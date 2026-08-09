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

' Ask for administrator, on the Windows that need asking.
'
' Starting a kernel service needs it, and the logon entry cannot have it: UAC
' launches Startup-folder shortcuts with the filtered token and never elevates
' them. So on NT 6 and later this script came up unprivileged, "sc start
' CoLinuxDriver" was refused, the driver stayed STOPPED and the boot daemon
' exited immediately for want of it. What made that hard to see is that the X
' server needs no privilege at all: VcXsrv appeared exactly as usual, the
' desktop looked like a working install, and only the Linux half was missing.
'
' Seen on Windows 8.1 after a clean install. XP has no UAC, so 5.2 was never
' affected and neither was any run started by hand from an elevated console --
' which is every test this launcher had.
'
' Re-launching itself through the "runas" verb is the whole fix: Windows shows
' the prompt, the elevated copy does the work, and this copy stands down. The
' marker argument is what stops that being a loop -- an elevated instance that
' somehow still cannot start the service must fail honestly rather than spawn
' another one.
Const ELEVATED_MARK = "--elevated"

Function Elevated()
	' Asked by trying. "net session" needs administrator and touches nothing;
	' its exit code is the answer, and it is the same question the service
	' manager is about to be asked.
	Elevated = (CreateObject("WScript.Shell") _
		    .Run("cmd /c net session", HIDDEN, WAIT) = 0)
End Function

Function NeedsElevation()
	Dim v
	NeedsElevation = False
	On Error Resume Next
	v = CreateObject("WScript.Shell").RegRead( _
		"HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\CurrentVersion")
	If Err.Number = 0 Then
		' "5.2" on XP x64 and Server 2003; "6.1", "6.3" and later above it.
		' CDbl on the leading number rather than a string compare, because
		' "10.0" sorts before "6.1" as text.
		If CDbl(Split(v, ".")(0)) >= 6 Then NeedsElevation = True
	End If
	On Error GoTo 0
End Function

' The real arguments, with the marker taken out.
'
' Kept separately rather than indexing WScript.Arguments below, because the
' marker is appended when this re-launches itself: a script started with no
' arguments would come back with exactly one, and the directory lookup would
' take "--elevated" for the program directory.
Dim i, already, passthru, argv, argc
already = False
passthru = ""
argc = 0
ReDim argv(WScript.Arguments.Count)
For i = 0 To WScript.Arguments.Count - 1
	If WScript.Arguments(i) = ELEVATED_MARK Then
		already = True
	Else
		passthru = passthru & " """ & WScript.Arguments(i) & """"
		argv(argc) = WScript.Arguments(i)
		argc = argc + 1
	End If
Next

' Nested, not "A And B And C".
'
' VBScript's And does not short-circuit: every operand is evaluated, always. So
' a single combined condition ran Elevated() -- and therefore "net session" --
' on XP too, where NeedsElevation() is already False and the answer cannot
' matter. On XP x64 that call reports "No network provider accepted the given
' network path", which is what the boot appeared to fail with, on the one
' Windows that never needed any of this.
If Not already Then
	If NeedsElevation() Then
		If Not Elevated() Then
			CreateObject("Shell.Application").ShellExecute _
				"wscript.exe", _
				"""" & WScript.ScriptFullName & """" & passthru & _
				" " & ELEVATED_MARK, _
				"", "runas", SHOWN
			WScript.Quit 0
		End If
	End If
End If

' Where things are. The shortcuts pass both directories, because only the
' installer knows where the user chose to put them.
'
' The fallbacks used to be this development box's own layout --
' F:\xfer\mocolinux-m2 and E:\MoCoLinux -- which is fine on the machine that
' wrote them and quietly wrong everywhere else. Run without arguments on a
' real install it went looking for the binaries in a stale staging directory:
' on the XP side that path still held a pre-GPU build, so the guest and the
' network bridge started from OLD executables and the GPU daemon failed with
' 'the system cannot find the file specified' -- leaving a guest that ran with
' no virtio-gpu device and no obvious reason why.
'
' This script is installed beside the binaries it launches, so it can simply
' ask where it is; and the images live under the system drive, which is what
' mocolinux.ini already assumes. Neither fallback can now name another
' machine's layout.
If argc >= 1 Then
	moco = argv(0)
Else
	moco = Left(WScript.ScriptFullName, _
	            InStrRev(WScript.ScriptFullName, "\") - 1)
End If
If argc >= 2 Then
	linux = argv(1)
Else
	linux = CreateObject("WScript.Shell") _
	        .ExpandEnvironmentStrings("%SystemDrive%") & "\MoCoLinux"
End If

' Deliberately narrow development switches. Normal shortcuts pass only the two
' directories above, so neither presentation rung becomes the default while
' its protocol is still being proved.
cogpu_extra = ""
If argc >= 3 Then
	If argv(2) = "--present-r1" Then cogpu_extra = " --present-r1"
	If argv(2) = "--present-r2" Then cogpu_extra = " --present-r2"
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

' Whether the guest itself is already up.
'
' Only the guest launch is skipped when it is -- everything below is checked
' individually. This used to quit outright here, ensuring nothing but the X
' server, and that is a real failure rather than a tidiness point: every one of
' these can be absent while the guest runs. The GPU daemon can die, or never
' have started, or the guest can have been launched by hand; re-running this
' script then looked like it had done its job and left the machine with no
' virtio-gpu device at all. Seen on XP, where the guest came up at logon with
' no cogpu-daemon.exe in the task list, and re-launching never repaired it.
guest_up = Running("colinux-daemon.exe")

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
' Two processors. Each one is a daemon thread of its own, pinned to a host core
' the driver picks -- never the boot processor's -- and crossing into the guest
' through a passage page of its own. The guest starts the second itself, by
' crossing with START_VCPU from smp_init(); there is no INIT-SIPI here and no
' local APIC for one to go through.
'
' This is the default because a second processor is the point of the SMP work,
' not an option to opt into. Drop it back to 1 if a guest ever has to be
' compared against the uniprocessor build.
If Not guest_up Then
	shell.Run """" & moco & "\colinux-daemon.exe"" --boot-kernel vmlinux" & _
		" --max-switches none" & _
		" --cpus 2" & _
		" --cobd0 \DosDevices\" & linux & "\root.img" & _
		" --cobd1 \DosDevices\" & linux & "\root-arch.img" & _
		" --init /sbin/init", SHOWN, NOWAIT
End If

' The network bridge and the terminal server. Both may be started before the
' guest has finished booting -- they wait for it -- and both exit on their own
' once it is gone, so nothing here has to clean them up.
'
' -r tcp:2222:22 exposes the guest's sshd on host port 2222. The terminal on
' 2323 stays the recovery path; ssh is the everyday one -- scp, keys, exit
' codes, and none of the console's line-echo mangling.
If Not Running("colinux-slirp-net-daemon.exe") Then
	shell.Run """" & moco & "\colinux-slirp-net-daemon.exe"" -R" & _
		" -r tcp:2222:22", HIDDEN, NOWAIT
End If

' The GPU daemon: the host side of the guest's virtio-gpu device. Hidden like
' the other two -- nothing for a person to read, and it writes its own log
' beside the images. After the guest, because it maps the guest's RAM, which
' does not exist until the boot daemon has allocated it. The transport
' tolerates a late daemon by design, so this ordering is safe rather than
' merely convenient.
If Not Running("cogpu-daemon.exe") Then
	' Per-command tracing writes one line for every fenced frame and materially
	' throttles direct presentation. Startup, errors, periodic counters and R2
	' frame-rate reports are logged without --verbose; reserve that flag for a
	' deliberately launched diagnostic daemon.
	shell.Run """" & moco & "\cogpu-daemon.exe""" & cogpu_extra, _
		  HIDDEN, NOWAIT
End If
' The terminal server shares an image name with the guest, so Running()
' cannot tell them apart; started only on a cold run. A second one would
' find port 2323 taken and exit anyway.
If Not guest_up Then
	shell.Run """" & moco & "\colinux-daemon.exe"" --console 2323", HIDDEN, NOWAIT
End If

' The X server last, so the guest's windows have somewhere to go. It checks for
' itself whether one is already running on :0.
StartX moco
