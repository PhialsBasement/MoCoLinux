// Reboot this machine from the kernel, without asking anything in user mode.
//
//     csc /nologo /platform:x64 /out:hardreset.exe hardreset.cs
//     hardreset.exe
//
// Compiled on the box, like tools/shot.cs, with the csc.exe that ships in the
// .NET framework -- there is no compiler in the transfer agent and nothing to
// cross-build C# with here.
//
// Why this exists when shutdown.exe is right there.
//
// A driver-development box spends its day in states shutdown.exe cannot get
// out of. `shutdown -r -f` is a user-mode program that asks the service
// controller to stop services and terminates applications that will not
// close: if a process is blocked inside an ioctl in our driver, that
// termination cannot complete, and the machine sits there. Worse, forcing it
// is the documented way to produce this port's recurring bugcheck 0x50 --
// killing a process whose thread is inside the driver closes its handles
// underneath kernel-mode code. The history here has four of those in one
// evening.
//
// NtShutdownSystem is the other end of the same operation: a system call that
// goes straight to the kernel's shutdown path. Nothing in user mode is asked
// to co-operate, so nothing in user mode can refuse.
//
// It is not a power cut, and the difference matters for the disk. The kernel
// still runs IoShutdownSystem, which sends IRP_MJ_SHUTDOWN to every driver
// that registered for it -- the file systems among them -- so volumes are
// flushed before the reset. What is skipped is the user-mode half: the
// service control manager, the "end program" dialogs, and the wait for
// processes that are never going to exit.
//
// Still not a substitute for shutting the guest down first. A cooperative
// guest's ext4 is journalled in data=ordered mode, so its metadata survives
// and its recent file contents do not; flushing the Windows volume says
// nothing about a filesystem living inside a file on it. Power the guest off
// with `systemctl poweroff`, wait for colinux-daemon.exe to exit, and only
// then use this.
//
// NtShutdownSystem is not enough, and this was learned the way everything here
// is learned. Fired at a box with a guest running, it did not reboot: the
// process sat in the task list alongside the colinux-daemon it was supposed to
// remove, and every PID on the machine was unchanged minutes later. The
// kernel's shutdown path still waits on things, and a thread wedged inside our
// own driver is one of them -- the same wedge that makes `sc stop` park in
// STOP_PENDING and shutdown.exe hang. A reset that cannot reset a box with a
// stuck driver is useless here, because that is the only state anybody needs
// one in.
//
// So the shutdown path is skipped altogether. RtlSetProcessIsCritical marks
// this process as one the kernel may not lose; terminating it then is an
// unrecoverable condition and the machine bugchecks -- CRITICAL_PROCESS_DIED
// -- which reboots immediately because tools/harden-m92p.bat sets AutoReboot.
// Nothing is asked, nothing is waited for, and no driver gets a vote.
//
// What that costs, stated plainly: this is a crash. Volumes are not flushed,
// so anything Windows had in its cache is lost, and a cooperative guest's ext4
// comes back needing its journal replayed. Power the guest off first whenever
// its disk matters -- and if the guest is already wedged, that is not
// available, which is precisely when this tool is the only way back.
//
// It also leaves a minidump, which is a side benefit rather than a nuisance:
// the bugcheck is ours and identifiable, so it cannot be mistaken later for
// one of the driver's own.
//
// -s asks for the polite version instead: NtShutdownSystem, which is right
// when nothing is stuck and a clean shutdown is wanted.
//
// Privilege 19 is SeShutdownPrivilege and 20 is SeDebugPrivilege, which
// RtlSetProcessIsCritical requires. RtlAdjustPrivilege enables them by LUID
// without the three-call dance of OpenProcessToken, LookupPrivilegeValue and
// AdjustTokenPrivileges; the process must already hold them, which any
// administrator does.
//
// Action 1 is ShutdownReboot. 0 is halt-without-reboot, which on this box
// means a machine that has to be visited, and 2 is power off -- tools/off.bat
// does that more politely and tools/wol.py brings it back.

using System;
using System.Runtime.InteropServices;

class HardReset
{
	[DllImport("ntdll.dll", SetLastError = true)]
	static extern uint RtlAdjustPrivilege(
		int privilege, bool enable, bool currentThread, out bool wasEnabled);

	[DllImport("ntdll.dll", SetLastError = true)]
	static extern uint NtShutdownSystem(int action);

	[DllImport("ntdll.dll", SetLastError = true)]
	static extern uint RtlSetProcessIsCritical(
		bool newValue, out bool oldValue, bool checkFlag);

	[DllImport("kernel32.dll", SetLastError = true)]
	static extern IntPtr GetCurrentProcess();

	[DllImport("kernel32.dll", SetLastError = true)]
	static extern bool TerminateProcess(IntPtr handle, uint exitCode);

	const int SE_SHUTDOWN_PRIVILEGE = 19;
	const int SE_DEBUG_PRIVILEGE    = 20;
	const int SHUTDOWN_REBOOT       = 1;

	static int Main(string[] args)
	{
		bool wasEnabled;
		bool polite = args.Length > 0 && args[0] == "-s";

		RtlAdjustPrivilege(SE_SHUTDOWN_PRIVILEGE, true, false, out wasEnabled);
		RtlAdjustPrivilege(SE_DEBUG_PRIVILEGE,    true, false, out wasEnabled);

		if (polite) {
			Console.WriteLine("hardreset: asking the kernel to reboot");
			Console.Out.Flush();
			uint src = NtShutdownSystem(SHUTDOWN_REBOOT);
			Console.WriteLine("hardreset: NtShutdownSystem returned 0x{0:X8}", src);
			return 1;
		}

		// Said before rather than after, because after does not happen, and
		// flushed because the machine is about to stop mid-sentence.
		Console.WriteLine("hardreset: bugchecking now");
		Console.Out.Flush();

		bool old;
		uint rc = RtlSetProcessIsCritical(true, out old, false);
		if (rc != 0) {
			Console.WriteLine(
				"hardreset: RtlSetProcessIsCritical failed (0x{0:X8})", rc);
			Console.WriteLine("hardreset: run this as an administrator");
			return 1;
		}

		TerminateProcess(GetCurrentProcess(), 1);

		// Unreachable: the kernel bugchecks inside the call above.
		return 1;
	}
}
