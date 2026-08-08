/*
 * MoCoLinux setup: the window a user actually sees.
 *
 * Plain Win32, drawn by hand, one executable, no dependencies. Every part of
 * that is forced rather than chosen.
 *
 * No .NET. A stock Windows XP x64 does not have it -- the framework on the test
 * box arrived with something else, which is exactly the kind of assumption that
 * works everywhere it is tested and nowhere it ships. WinForms and WPF are
 * therefore unavailable, and so is anything that wants a runtime.
 *
 * No NSIS. It would do the file-laying competently and it looks like 2004, which
 * matters more than it sounds: this port's whole proposition is that the old
 * machine is doing something new, and an installer wearing its grandfather's
 * clothes argues the opposite before a word is read. It also cannot do the
 * second half of the job -- twenty minutes of progress driven by a guest over a
 * console -- so it would have been a second program regardless.
 *
 * Nothing themed. No common controls, no group boxes, no 3D bevels, no
 * DrawFrameControl. Windows' own widgets are what date a window fastest, and
 * they change appearance across the three versions this has to run on. Drawing
 * everything means one appearance everywhere, and the only thing the OS supplies
 * is the fonts.
 *
 * Which leaves Win32, and that is the forward-compatible answer as well as the
 * backward one: it is the most stable ABI Microsoft has, so the same binary
 * draws the same window on XP, on 7, and on 10 if the port ever reaches them.
 */

#include <windows.h>
#include <cpuid.h>		/* __get_cpuid, for the hypervisor-present bit */
#include <shlobj.h>
#include <shlwapi.h>
#include <winsvc.h>
#include <winsock2.h>
#include <tlhelp32.h>
#include <stdio.h>

/* ------------------------------------------------------------------ the work */

/*
 * What the installer actually does, and where it refuses.
 *
 * Every check below names what to do about it. "Setup cannot continue" with no
 * reason is how a user concludes the software is broken rather than that their
 * machine is unsuitable, and most of these are things they can fix in a minute.
 */

/*
 * Shown in the wizard's heading, so a running installer says which release it
 * is. The release directory and zip are named from the same string by hand;
 * nothing reads it back, so keep the two in step.
 */
#define MOCO_VERSION "0.5.5"

#define PAYLOAD_MAX 20

/* Laid down beside each other; the image goes to the Linux directory instead. */
static const char *payload[PAYLOAD_MAX] = {
	"colinux-daemon.exe",
	"colinux-slirp-net-daemon.exe",
	"linux.sys",
	"vmlinux",
	/*
	 * The launchers. Without these the install produces a working Linux that
	 * the user has no way to start or reach: everything below was created by
	 * hand on the development box and never shipped, which is exactly the
	 * class of gap the X server tickbox was.
	 *
	 * moco-boot.vbs   starts driver, guest, network, console and X, hidden
	 * moco-icons.vbs  makes the desktop shortcuts and the logon entry
	 * moco-term.bat   the console shortcut's target; finds or enables telnet
	 * xstart1142.bat  starts the X server, once, if one is not already there
	 * stop.bat        ends a wedged guest without a reboot
	 */
	"moco-boot.vbs",
	"moco-icons.vbs",
	"moco-term.bat",
	"xstart1142.bat",
	"stop.bat",
	/*
	 * The GPU half. Without these an install has a working Linux whose
	 * graphics are rendered by the guest's single core -- which is the
	 * state every release before this one shipped in.
	 *
	 * cogpu-daemon is the virtio-gpu device: it services the guest's
	 * rings, maps guest memory, and drives virglrenderer. moco-boot.vbs
	 * already starts it, so the only thing missing was the file.
	 *
	 * The DLLs are its dependencies and neither is optional: virglrenderer
	 * is the renderer and libepoxy resolves the GL entry points. Both
	 * import nothing but KERNEL32 and msvcrt.
	 *
	 * libgcc_s_seh-1.dll and libwinpthread-1.dll used to be here too, and
	 * they were an XP defect nobody had noticed. The toolchain builds those
	 * two against the Universal CRT whatever this tree does, so shipping
	 * them dragged api-ms-win-crt-* dependencies into a release that is
	 * supposed to run on a machine with no UCRT at all -- the GPU daemon
	 * would have failed to load on XP with nothing said about why, and the
	 * guest would have fallen back to software rendering. virglrenderer now
	 * links both statically (download/mingw64-cross.ini), so neither file
	 * is needed and neither ships.
	 */
	"cogpu-daemon.exe",
	"libvirglrenderer-1.dll",
	"libepoxy-0.dll",
	/* The licence travels with the program, and the licence page reads it from
	 * beside Setup rather than from a compiled-in copy that could drift. */
	"COPYING",
	NULL,
};

/*
 * The X server, and why this exact version.
 *
 * 1.14.2.1 is the last VcXsrv before XP support was dropped in 1.14.3.0: every
 * PE in it declares subsystem 5.2 and it needs only MSVCR100, which its own
 * installer lays down. 1.20.x declares 6.0 and wants the Universal CRT, so a
 * 5.2 loader refuses it outright -- the same trap this port's own binaries were
 * in until they were relinked against msvcrt.
 *
 * Installed into the program directory rather than Program Files, and silently:
 * /S is NSIS's silent flag and /D= is its destination, which must come last and
 * must not be quoted even when the path has spaces.
 *
 * Not part of `payload` above, because that list is mandatory -- a missing entry
 * there fails the install. A missing X server should not: Linux still boots,
 * still networks and still has a terminal without one. It is reported instead,
 * and the tickbox no longer lies about what happened.
 */
#define XSERVER_INSTALLER "vcxsrv-64.1.14.2.1.installer.exe"
#define XSERVER_SUBDIR	  "vcxsrv1142"

/*
 * The root image that ships, and why there has to be one.
 *
 * A release cannot be only a kernel and a driver. mkmanjarorootfs.sh needs a
 * Linux with pacman and e2fsprogs already running, so it can reproduce a root
 * filesystem but cannot create the first one -- something bootable must be in
 * the download or nothing can bootstrap.
 *
 * So the download carries one ordinary root image, and that image carries the
 * builder at /usr/local/bin/mkmanjarorootfs.sh. On the target it boots, and
 * Setup runs the builder inside it against the empty second disk. There is no
 * separate seed system with its own package set: one image, one script, and the
 * path that has actually run end to end on this hardware.
 *
 * It is copied to the Linux directory rather than the program directory, because
 * it is gigabytes of data the user may want to move or delete, and because the
 * system it builds ends up beside it.
 */
/*
 * The Arch root, by its own name. It is the image that has actually built a
 * Manjaro filesystem on this hardware, and it carries the maker script. Renaming
 * a copy of it to something neutral only meant an extra gigabyte copied for no
 * reason and a second name for one file.
 */
#define IMAGE_BASE  "root-arch.img"
#define IMAGE_BUILT "root.img"

static char dir_program[MAX_PATH];
static char dir_linux[MAX_PATH];
static char self_dir[MAX_PATH];

static int want_desktop = 1;		/* else the console system */
static int want_xserver = 1;
static int disk_gb = 16;

/* Set by --finish: the autostart re-entry, which skips the pages. */
static int auto_finish;

/*
 * Progress, written by the worker thread and read by the painter.
 *
 * A critical section rather than atomics or a lock-free queue: this is updated a
 * few times a second by one writer and read by one reader, so the only thing
 * that matters is that a half-written string is never painted.
 */
static CRITICAL_SECTION work_lock;
static int work_stage;			/* 1..work_total, 0 before it starts */
static int work_total = 8;
static int work_pct = -1;		/* -1 means indeterminate */
static char work_what[160];
static char work_log[8][160];		/* the fold-out pane, newest last */
static int work_log_n;
static int work_failed;
static char work_fail[240];
static int work_done;

#define WM_WORK	(WM_APP + 1)
#define WM_ASK_REBOOT	(WM_APP + 2)

static HWND main_wnd;

static void work_say(const char *fmt, ...)
{
	char buf[160];
	va_list ap;

	va_start(ap, fmt);
	_vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
	buf[sizeof(buf) - 1] = 0;
	va_end(ap);

	EnterCriticalSection(&work_lock);
	if (work_log_n < 8) {
		lstrcpyn(work_log[work_log_n++], buf, sizeof(work_log[0]));
	} else {
		int i;

		for (i = 1; i < 8; i++)
			lstrcpyn(work_log[i - 1], work_log[i], sizeof(work_log[0]));
		lstrcpyn(work_log[7], buf, sizeof(work_log[0]));
	}
	LeaveCriticalSection(&work_lock);

	if (main_wnd)
		PostMessage(main_wnd, WM_WORK, 0, 0);
}

static void work_at(int stage, int pct, const char *what)
{
	EnterCriticalSection(&work_lock);
	work_stage = stage;
	work_pct = pct;
	if (what)
		lstrcpyn(work_what, what, sizeof(work_what));
	LeaveCriticalSection(&work_lock);

	if (main_wnd)
		PostMessage(main_wnd, WM_WORK, 0, 0);
}

static void work_fatal(const char *fmt, ...)
{
	va_list ap;

	EnterCriticalSection(&work_lock);
	va_start(ap, fmt);
	_vsnprintf(work_fail, sizeof(work_fail) - 1, fmt, ap);
	work_fail[sizeof(work_fail) - 1] = 0;
	va_end(ap);
	work_failed = 1;
	LeaveCriticalSection(&work_lock);

	if (main_wnd)
		PostMessage(main_wnd, WM_WORK, 0, 0);
}

/* ------------------------------------------------------- the refusals (§4) */

/*
 * 64-bit Windows. The driver is x86-64 and shares a processor with the NT
 * kernel; there is nothing to negotiate.
 *
 * IsWow64Process rather than the compile-time architecture, because this exe is
 * 64-bit and simply would not run on a 32-bit Windows -- so by the time this
 * code executes the answer is already yes. It is checked anyway, cheaply, so the
 * refusal exists in one place if a 32-bit build is ever made.
 */
static BOOL check_x64(char *why, int n)
{
	SYSTEM_INFO si;

	GetNativeSystemInfo(&si);
	if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64)
		return TRUE;

	lstrcpyn(why, "This is the x86-64 port and needs 64-bit Windows.", n);
	return FALSE;
}

/*
 * Which family of Windows this is, asked once and only as a floor.
 *
 * NT 6.0 is where the things this file must do differently begin: UAC (so a
 * Run-key autostart is never elevated), kernel-mode code signing (so the
 * driver needs test-signing switched on), and Task Scheduler 2.0 (which is
 * what replaces the Run key). GetVersionEx lies upward on 8.1+ without a
 * manifest -- it reports 6.2 -- and that is fine here too: everything that
 * lies about being 6.2 still has all three of those properties.
 */
static BOOL host_is_nt6(void)
{
	OSVERSIONINFO v;

	ZeroMemory(&v, sizeof(v));
	v.dwOSVersionInfoSize = sizeof(v);
	if (!GetVersionEx(&v))
		return FALSE;	/* claim XP; the XP path asks for less */
	return v.dwMajorVersion >= 6;
}

/*
 * Run a command with no window and wait for it, for the two conversations
 * Setup has with OS tools (bcdedit, schtasks). Returns the exit code, or -1
 * if it would not start at all.
 */
static void glog(const char *fmt, ...);

static int run_tool(const char *cmdline, int timeout_ms)
{
	STARTUPINFO si;
	PROCESS_INFORMATION pi;
	DWORD code = (DWORD)-1;
	char buf[1024];

	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory(&pi, sizeof(pi));
	lstrcpyn(buf, cmdline, sizeof(buf));

	if (!CreateProcess(NULL, buf, NULL, NULL, FALSE, CREATE_NO_WINDOW,
			   NULL, NULL, &si, &pi)) {
		glog("tool would not start (error %lu): %.200s",
		     (unsigned long)GetLastError(), cmdline);
		return -1;
	}

	WaitForSingleObject(pi.hProcess, timeout_ms);
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	glog("tool exited %ld: %.200s", (long)code, cmdline);
	return (int)code;
}

/* 5.2 or later: XP x64 and Server 2003 are 5.2, and nothing older is supported. */
static BOOL check_version(char *why, int n)
{
	OSVERSIONINFO v;

	ZeroMemory(&v, sizeof(v));
	v.dwOSVersionInfoSize = sizeof(v);
	/*
	 * GetVersionEx is deprecated and lies on Windows 8.1 and later without a
	 * manifest -- it reports 6.2. That is harmless here: the test is a floor,
	 * and every version that lies about being 6.2 is above it.
	 */
	if (!GetVersionEx(&v)) {
		lstrcpyn(why, "Setup could not determine the Windows version.", n);
		return FALSE;
	}

	if (v.dwMajorVersion > 5 ||
	    (v.dwMajorVersion == 5 && v.dwMinorVersion >= 2))
		return TRUE;

	lstrcpyn(why, "Windows XP x64 or later is required.", n);
	return FALSE;
}

/*
 * Is UEFI Secure Boot switched on?
 *
 * It matters because it overrules test-signing. With Secure Boot enabled the
 * kernel refuses an unsigned or test-signed driver whatever bcdedit says, so
 * everything below succeeds, the restart happens, and the driver then fails
 * with the same ERROR_INVALID_IMAGE_HASH that a missing test-signing setting
 * produces -- sending someone to fix a setting that is already correct.
 *
 * Read from the registry rather than Confirm-SecureBootUEFI, because that is a
 * PowerShell cmdlet and this program has no dependencies. The key exists only
 * on UEFI firmware; its absence means a legacy/BIOS boot, where Secure Boot
 * cannot be on.
 *
 * Nothing here can turn it off. It is a firmware setting with no API, by
 * design -- software that could disable Secure Boot would defeat the point of
 * it. All an installer can do is notice and say so.
 */
static BOOL secure_boot_on(void)
{
	HKEY  key;
	DWORD val = 0, size = sizeof(val), type = 0;
	BOOL  on = FALSE;

	if (RegOpenKeyEx(HKEY_LOCAL_MACHINE,
			 "SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State",
			 0, KEY_READ, &key) != ERROR_SUCCESS)
		return FALSE;	/* no key: not UEFI, so not Secure Boot */

	if (RegQueryValueEx(key, "UEFISecureBoot", NULL, &type,
			    (LPBYTE)&val, &size) == ERROR_SUCCESS &&
	    type == REG_DWORD)
		on = (val != 0);

	RegCloseKey(key);
	return on;
}

/*
 * Is a hypervisor already running on this processor?
 *
 * The one condition this port cannot work around. Both kernels here run at
 * ring 0 on the bare processor and the world switch swaps CPU context between
 * them; if Windows is itself a guest of Hyper-V -- which is what VBS,
 * Memory Integrity, Credential Guard, WSL2, Sandbox and Device Guard all
 * quietly turn on -- then the processor is in VMX root mode, Windows is in a
 * partition, and there is no bare ring 0 left to switch to.
 *
 * CPUID leaf 1, ECX bit 31 is the hypervisor-present bit. It is architecturally
 * reserved-zero on real hardware and set by every hypervisor by convention, so
 * it answers the question that matters -- "is something already underneath this
 * Windows" -- rather than the question the registry answers, which is what the
 * settings say they want. The two differ: DeviceGuard keys can read enabled
 * while the feature is not running, and vice versa after a settings change but
 * before the restart that applies it.
 *
 * Present on this box's XP and 7 installs as well, where it reads zero, which
 * is what a legacy boot on bare metal should say.
 */
static BOOL hypervisor_present(void)
{
	unsigned int a = 0, b = 0, c = 0, d = 0;

	if (!__get_cpuid(1, &a, &b, &c, &d))
		return FALSE;	/* no CPUID leaf 1: far too old to be virtual */

	return (c & (1u << 31)) != 0;
}

/*
 * Refuse under a hypervisor, and say which switch turns it off.
 *
 * Written for Windows 10 and 11, where this is the usual reason an install
 * fails: Memory Integrity is on by default on many machines, and it enables
 * the hypervisor without ever using the word. Nothing on the screen tells the
 * person their PC is virtualised, so the failure is otherwise inexplicable.
 *
 * Like the Secure Boot check this runs before anything is written, and like
 * that one it is deliberately verbose: the settings live in three different
 * places depending on the Windows version, and naming only one of them is how
 * somebody concludes the check is wrong.
 */
static BOOL check_hypervisor(char *why, int n)
{
	if (!hypervisor_present())
		return TRUE;

	lstrcpyn(why,
		 "This copy of Windows is running on top of a hypervisor, and "
		 "MoCoLinux cannot install while it is.\r\n\r\n"
		 "MoCoLinux runs Linux directly on the processor, side by side "
		 "with Windows. That is only possible when nothing else is "
		 "underneath Windows already. Features like Memory Integrity, "
		 "Core Isolation, Credential Guard, Virtualization-Based "
		 "Security, Windows Sandbox, WSL 2 and Hyper-V all switch that "
		 "hypervisor on, usually without saying so.\r\n\r\n"
		 "Turn off whichever of these you have, then restart:\r\n\r\n"
		 "  Memory Integrity  --  open Windows Security, choose Device "
		 "security, then Core isolation details, and set Memory "
		 "integrity to Off.\r\n\r\n"
		 "  Hyper-V, Sandbox, WSL  --  press the Windows key, type "
		 "\"Turn Windows features on or off\", and clear the tick "
		 "beside Hyper-V, Windows Sandbox, Virtual Machine Platform "
		 "and Windows Subsystem for Linux.\r\n\r\n"
		 "  If it still will not go  --  open Command Prompt as an "
		 "administrator and run:  bcdedit /set hypervisorlaunchtype "
		 "off\r\n\r\n"
		 "Restart the computer after any of these, then run this Setup "
		 "again. A restart is required -- the setting does not take "
		 "effect until the machine boots.\r\n\r\n"
		 "Nothing has been installed or changed on your computer.",
		 n);
	return FALSE;
}

/*
 * Secure Boot, which no installer can switch off.
 *
 * A hard stop rather than a warning. With Secure Boot on, the kernel refuses a
 * test-signed driver whatever bcdedit says -- so an install that continued
 * would copy a gigabyte, restart the machine, and only then fail with a
 * signature error, having changed a boot setting for nothing.
 *
 * The wording is deliberately plain and deliberately long. This is the one
 * failure that cannot be fixed from inside Windows: the person has to reboot
 * into a firmware screen, and if they have never done that, "disable Secure
 * Boot" is not an instruction, it is a riddle.
 */
static BOOL check_secure_boot(char *why, int n)
{
	if (!secure_boot_on())
		return TRUE;

	lstrcpyn(why,
		 "Secure Boot is switched on, and MoCoLinux cannot install "
		 "while it is.\r\n\r\n"
		 "Secure Boot is a setting in your computer's firmware -- the "
		 "menu that appears before Windows starts. It blocks drivers "
		 "that are not signed by Microsoft, and MoCoLinux is not. No "
		 "program can change it for you, including this one.\r\n\r\n"
		 "To turn it off:\r\n\r\n"
		 "  1.  Save any work and close your programs.\r\n"
		 "  2.  Restart the computer.\r\n"
		 "  3.  As it starts, press the setup key repeatedly. It is "
		 "usually F2, F10, F12 or Delete -- the correct one is shown "
		 "for a moment on the first screen, often as \"Press F2 for "
		 "Setup\". If Windows loads instead, you were too late; "
		 "restart and try again, pressing sooner.\r\n"
		 "  4.  In the menus that appear, look for a page called Boot, "
		 "Security or Authentication, and an entry called Secure "
		 "Boot.\r\n"
		 "  5.  Change it to Disabled.\r\n"
		 "  6.  Choose Save and Exit (often F10).\r\n"
		 "  7.  Let Windows start, then run this Setup again.\r\n\r\n"
		 "If Secure Boot is greyed out and will not change, look for "
		 "an option to set an administrator or supervisor password "
		 "first, or to switch the boot mode from UEFI to Legacy/CSM; "
		 "either usually unlocks it.\r\n\r\n"
		 "Nothing has been installed or changed on your computer.",
		 n);
	return FALSE;
}

/*
 * Administrators, because installing a kernel service needs it.
 *
 * Asked by trying rather than by inspecting the token: OpenSCManager with
 * CREATE_SERVICE succeeds exactly when the installer will be able to do the one
 * privileged thing it needs, which is a better question than whether a
 * particular group is present in a particular token.
 */
static BOOL check_admin(char *why, int n)
{
	SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CREATE_SERVICE);

	if (scm) {
		CloseServiceHandle(scm);
		return TRUE;
	}

	lstrcpyn(why, "Run Setup as an administrator: it installs a kernel driver.", n);
	return FALSE;
}

/*
 * The Linux directory must be NTFS, and the reason is worth stating on the page
 * rather than discovering at 4 GB: the disk image is larger than FAT32 can hold,
 * so a FAT32 target fails part way through a package install with a write error
 * that reads as a corrupt download.
 */
static BOOL check_fs(const char *dir, char *why, int n)
{
	char root[MAX_PATH];
	char fs[32] = {0};

	lstrcpyn(root, dir, sizeof(root));
	if (!GetVolumePathName(dir, root, sizeof(root))) {
		lstrcpyn(why, "That path is not on a drive Setup can see.", n);
		return FALSE;
	}

	if (!GetVolumeInformation(root, NULL, 0, NULL, NULL, NULL, fs, sizeof(fs))) {
		lstrcpyn(why, "Setup could not read that volume.", n);
		return FALSE;
	}

	if (lstrcmpi(fs, "NTFS") == 0)
		return TRUE;

	_snprintf(why, n - 1, "%s is %s. The disk image is larger than %s can hold,"
			      " so it must go on an NTFS drive.", root, fs, fs);
	why[n - 1] = 0;
	return FALSE;
}

/* Image size plus the program, on the volume the image is going to. */
static BOOL check_space(const char *dir, int gb, char *why, int n)
{
	ULARGE_INTEGER freeb, total, avail;
	char root[MAX_PATH];
	double need_gb = gb + 0.1;

	lstrcpyn(root, dir, sizeof(root));
	GetVolumePathName(dir, root, sizeof(root));

	if (!GetDiskFreeSpaceEx(root, &freeb, &total, &avail)) {
		lstrcpyn(why, "Setup could not measure the free space there.", n);
		return FALSE;
	}

	if ((double)freeb.QuadPart >= need_gb * 1073741824.0)
		return TRUE;

	_snprintf(why, n - 1, "%s has %.1f GB free and %.0f GB is needed.",
		  root, (double)freeb.QuadPart / 1073741824.0, need_gb);
	why[n - 1] = 0;
	return FALSE;
}

/*
 * A real coLinux 0.7.9 install is a conflict, not a coexistence.
 *
 * Both want the service name CoLinuxDriver and both create the same device
 * object, so whichever loads first wins and the other fails in a way that looks
 * like a broken build. Detected by asking the SCM for the service and comparing
 * the binary path against where ours would be.
 */
static BOOL check_no_foreign_driver(char *why, int n)
{
	SC_HANDLE scm, svc;
	BOOL ok = TRUE;

	scm = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
	if (!scm)
		return TRUE;	/* check_admin already has the real complaint */

	svc = OpenService(scm, "CoLinuxDriver", SERVICE_QUERY_CONFIG);
	if (svc) {
		BYTE buf[4096];
		QUERY_SERVICE_CONFIG *cfg = (QUERY_SERVICE_CONFIG *)buf;
		DWORD got = 0;

		if (QueryServiceConfig(svc, cfg, sizeof(buf), &got) &&
		    cfg->lpBinaryPathName) {
			/*
			 * Ours if it points inside the directory we are about to
			 * install into -- which is the upgrade case and fine.
			 * Anything else is somebody else's coLinux.
			 */
			if (dir_program[0] &&
			    !StrStrI(cfg->lpBinaryPathName, dir_program)) {
				_snprintf(why, n - 1,
					  "Another Cooperative Linux is installed"
					  " (%s). Remove it first: both use the same"
					  " driver service and device.",
					  cfg->lpBinaryPathName);
				why[n - 1] = 0;
				ok = FALSE;
			}
		}
		CloseServiceHandle(svc);
	}

	CloseServiceHandle(scm);
	return ok;
}

/*
 * Nothing of ours may be running while its files are replaced.
 *
 * Matched on a "colinux-" prefix, not a substring. A substring search for
 * "colinux" finds mocolinux-setup.exe -- this program -- so the first real run
 * refused to install with "shut the Linux guest down" while no guest existed.
 * The prefix is what the daemons are actually named, and the current process is
 * skipped as well, because a check that can fail on account of itself is worse
 * than no check.
 *
 * It matters rather than being cosmetic: the daemon holds a handle on the driver,
 * so replacing linux.sys under a live guest fails, and forcing it is this port's
 * recurring bugcheck 0x50 -- terminating a process with a thread inside the
 * driver closes its handles beneath kernel-mode code. So the answer is to refuse
 * and let the user shut the guest down properly.
 */
static BOOL check_nothing_running(char *why, int n)
{
	HANDLE snap;
	PROCESSENTRY32 pe;
	DWORD self = GetCurrentProcessId();
	BOOL ok = TRUE;

	snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return TRUE;

	ZeroMemory(&pe, sizeof(pe));
	pe.dwSize = sizeof(pe);

	if (Process32First(snap, &pe)) {
		do {
			if (pe.th32ProcessID == self)
				continue;
			if (StrCmpNIA(pe.szExeFile, "colinux-", 8) != 0)
				continue;

			_snprintf(why, n - 1,
				  "%s is running. Shut the Linux guest down"
				  " (poweroff) and let it exit before installing.",
				  pe.szExeFile);
			why[n - 1] = 0;
			ok = FALSE;
			break;
		} while (Process32Next(snap, &pe));
	}

	CloseHandle(snap);
	return ok;
}

/* ------------------------------------------------------- doing the install */

/*
 * The root image, copied to the Linux directory.
 *
 * Separate from the payload loop because it is the one file that does not belong
 * beside the binaries, and because it is the one whose absence makes everything
 * else pointless -- an install without it finishes with nothing to boot. Reported
 * as its own stage, since it is gigabytes and takes visibly longer than the rest
 * of the copy put together.
 */
static BOOL copy_image(void)
{
	char from[MAX_PATH], to[MAX_PATH];

	_snprintf(from, sizeof(from) - 1, "%s\\%s", self_dir, IMAGE_BASE);
	_snprintf(to, sizeof(to) - 1, "%s\\%s", dir_linux, IMAGE_BASE);
	from[sizeof(from) - 1] = to[sizeof(to) - 1] = 0;

	if (GetFileAttributes(to) != INVALID_FILE_ATTRIBUTES) {
		work_say("%s is already here", IMAGE_BASE);
		return TRUE;
	}

	work_at(5, -1, "Copying the Linux image");
	work_say("%s", IMAGE_BASE);

	if (!CopyFile(from, to, FALSE)) {
		DWORD err = GetLastError();

		if (err == ERROR_FILE_NOT_FOUND)
			work_fatal("%s is missing from the Setup folder. Without it"
				   " there is no Linux to install.", IMAGE_BASE);
		else
			work_fatal("Could not copy %s (error %lu).", IMAGE_BASE,
				   (unsigned long)err);
		return FALSE;
	}

	return TRUE;
}

static BOOL copy_payload(void)
{
	int i;

	for (i = 0; payload[i]; i++) {
		char from[MAX_PATH], to[MAX_PATH];

		_snprintf(from, sizeof(from) - 1, "%s\\%s", self_dir, payload[i]);
		_snprintf(to, sizeof(to) - 1, "%s\\%s", dir_program, payload[i]);
		from[sizeof(from) - 1] = to[sizeof(to) - 1] = 0;

		work_say("%s", payload[i]);
		work_at(2, (i * 100) / 4, "Copying files");

		if (!CopyFile(from, to, FALSE)) {
			DWORD err = GetLastError();

			if (err == ERROR_FILE_NOT_FOUND) {
				work_fatal("%s is missing from the Setup folder."
					   " The download is incomplete.", payload[i]);
			} else {
				work_fatal("Could not write %s (error %lu).",
					   to, (unsigned long)err);
			}
			return FALSE;
		}
	}

	return TRUE;
}

/*
 * The driver service.
 *
 * Created here rather than by shelling out to colinux-daemon --install-driver,
 * for two reasons that are both recorded defects: that option reports success
 * when the service merely exists, which is its state after every reboot and is
 * not evidence that anything is loaded; and it does not start the service, so a
 * caller that trusts it fails one step later with "the system cannot find the
 * file specified".
 *
 * SERVICE_DEMAND_START, not AUTO_START. The driver reserves its guest memory as
 * unbroken 32 MB physical runs when it loads, and doing that at every boot on a
 * machine that may never run a guest that session is rude. The launcher starts
 * it.
 */
static BOOL install_driver(void)
{
	SC_HANDLE scm, svc;
	char sys[MAX_PATH];

	/* Stage 6: this runs in the second half now, after the restart. It said
	 * 3 from when it ran before the reboot, which made the step counter and
	 * the bar jump backwards from 6 to 3 in front of the user. */
	work_at(6, -1, "Installing the driver");

	_snprintf(sys, sizeof(sys) - 1, "%s\\linux.sys", dir_program);
	sys[sizeof(sys) - 1] = 0;

	scm = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (!scm) {
		work_fatal("Could not open the service manager (error %lu).",
			   (unsigned long)GetLastError());
		return FALSE;
	}

	svc = OpenService(scm, "CoLinuxDriver", SERVICE_ALL_ACCESS);
	if (svc) {
		/* An upgrade: point the existing service at the new file. */
		work_say("updating the existing service");
		if (!ChangeServiceConfig(svc, SERVICE_KERNEL_DRIVER,
					 SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
					 sys, NULL, NULL, NULL, NULL, NULL, NULL)) {
			work_fatal("Could not update the driver service (error %lu).",
				   (unsigned long)GetLastError());
			CloseServiceHandle(svc);
			CloseServiceHandle(scm);
			return FALSE;
		}
	} else {
		work_say("creating the CoLinuxDriver service");
		svc = CreateService(scm, "CoLinuxDriver", "Cooperative Linux",
				    SERVICE_ALL_ACCESS, SERVICE_KERNEL_DRIVER,
				    SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
				    sys, NULL, NULL, NULL, NULL, NULL);
		if (!svc) {
			work_fatal("Could not create the driver service (error %lu).",
				   (unsigned long)GetLastError());
			CloseServiceHandle(scm);
			return FALSE;
		}
	}

	CloseServiceHandle(svc);
	CloseServiceHandle(scm);
	return TRUE;
}

/*
 * Test-signing, on the Windows that check signatures.
 *
 * From Vista on, x64 kernel-mode code signing refuses the driver outright --
 * not a warning, a load failure (error 577) that surfaces one reboot later as
 * "the driver would not start". The shipped linux.sys carries a test
 * signature, which those systems accept only with test-signing switched on.
 * XP ignores embedded signatures entirely, so on 5.2 there is nothing to do.
 *
 * Done here, before the restart the install already takes, because that is
 * the reboot the setting needs. Never fatal: if bcdedit fails the install
 * still lays everything down, and the driver-start error message names this
 * exact fix. Reported plainly, because quietly changing a boot setting is
 * not something an installer gets to do.
 */
static void enable_testsigning(void)
{
	if (!host_is_nt6())
		return;

	work_at(3, -1, "Enabling test-signed drivers");

	if (run_tool("bcdedit /set testsigning on", 30000) == 0)
		work_say("test-signing enabled -- the desktop will show a Test"
			 " Mode watermark");
	else
		work_say("could not enable test-signing; the driver may refuse"
			 " to load after the restart");
}

/*
 * The X server, actually installed.
 *
 * The tickbox on the previous page has always been recorded in mocolinux.ini and
 * never acted on: `grep -i vcxsrv` over this file used to find exactly one hit,
 * and it was the label. So somebody could tick "install the X server", watch the
 * install succeed, double-click a Linux application and get nothing at all --
 * because an X client with no server produces no error, it simply never appears.
 * That is the worst shape a missing feature can have.
 *
 * Never fatal. Without an X server the guest still boots, still networks and
 * still has a terminal; what it loses is windows. So a missing installer or a
 * failed run is reported and the install carries on.
 */
static void glog(const char *fmt, ...);

static void install_xserver(void)
{
	char src[MAX_PATH], dst[MAX_PATH], cmd[MAX_PATH * 2];
	STARTUPINFO si;
	PROCESS_INFORMATION pi;
	DWORD code = 0;

	if (!want_xserver)
		return;

	work_at(3, -1, "Installing the X server");

	_snprintf(src, sizeof(src) - 1, "%s\\%s", self_dir, XSERVER_INSTALLER);
	_snprintf(dst, sizeof(dst) - 1, "%s\\%s", dir_program, XSERVER_SUBDIR);
	src[sizeof(src) - 1] = dst[sizeof(dst) - 1] = 0;

	if (GetFileAttributes(src) == INVALID_FILE_ATTRIBUTES) {
		glog("x server: %s is not beside Setup -- skipping", src);
		work_say("the X server installer is missing; Linux will have no"
			 " windows until it is installed");
		return;
	}

	if (GetFileAttributes(dst) != INVALID_FILE_ATTRIBUTES) {
		work_say("the X server is already installed");
		return;
	}

	/*
	 * /S silent, /D= destination. NSIS requires /D last and unquoted, even
	 * when the path contains spaces -- quoting it is the classic way to get a
	 * silent install into a directory called "Program".
	 */
	_snprintf(cmd, sizeof(cmd) - 1, "\"%s\" /S /D=%s", src, dst);
	cmd[sizeof(cmd) - 1] = 0;
	glog("x server: %s", cmd);

	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory(&pi, sizeof(pi));

	if (!CreateProcess(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
			   NULL, self_dir, &si, &pi)) {
		glog("x server: could not start the installer (error %lu)",
		     (unsigned long)GetLastError());
		work_say("could not start the X server installer");
		return;
	}

	WaitForSingleObject(pi.hProcess, 300000);	/* it is 46 MB; give it time */
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);

	if (GetFileAttributes(dst) == INVALID_FILE_ATTRIBUTES) {
		glog("x server: installer exited %lu but %s is not there",
		     (unsigned long)code, dst);
		work_say("the X server did not install (code %lu)",
			 (unsigned long)code);
		return;
	}

	glog("x server: installed into %s", dst);
	work_say("X server installed");
}

/*
 * The desktop shortcuts and the logon entry.
 *
 * An installed system nobody can start is not installed. Until this ran, the
 * icons and the autostart existed only because they had been made by hand on the
 * development box -- so a user finished the wizard and had a working Linux with
 * nothing to click.
 *
 * Done by handing both directories to the script that shipped beside us, rather
 * than by building shortcuts here: a .lnk needs COM, and the script already does
 * it and can be re-run later by somebody who installs more applications.
 *
 * Not fatal. Missing icons are an inconvenience; a failed install is not.
 */
static void install_shortcuts(void)
{
	char cmd[MAX_PATH * 3];
	STARTUPINFO si;
	PROCESS_INFORMATION pi;

	work_at(3, -1, "Creating shortcuts");

	_snprintf(cmd, sizeof(cmd) - 1,
		  "cscript //nologo \"%s\\moco-icons.vbs\" \"%s\" \"%s\"",
		  dir_program, dir_program, dir_linux);
	cmd[sizeof(cmd) - 1] = 0;
	glog("shortcuts: %s", cmd);

	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory(&pi, sizeof(pi));

	if (!CreateProcess(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
			   NULL, dir_program, &si, &pi)) {
		glog("shortcuts: could not run the script (error %lu)",
		     (unsigned long)GetLastError());
		work_say("could not create the desktop shortcuts");
		return;
	}

	WaitForSingleObject(pi.hProcess, 60000);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);

	work_say("shortcuts on the desktop; MoCoLinux starts at logon");
}

/*
 * The disk image, committed rather than sparse.
 *
 * SetEndOfFile after seeking, which is what `fsutil file createnew` does: the
 * space is reserved instantly without writing a byte. Sparse would be faster
 * still and is the wrong trade -- an image that fails half way through a package
 * install because the volume filled is worse than a reservation that fails now,
 * while there is a page to say so on.
 */
static BOOL create_image(void)
{
	char path[MAX_PATH];
	HANDLE h;
	LARGE_INTEGER size;

	work_at(4, -1, "Reserving the disk image");

	_snprintf(path, sizeof(path) - 1, "%s\\root.img", dir_linux);
	path[sizeof(path) - 1] = 0;

	if (GetFileAttributes(path) != INVALID_FILE_ATTRIBUTES) {
		work_say("root.img already exists -- keeping it");
		return TRUE;
	}

	h = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
		       FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		work_fatal("Could not create %s (error %lu).", path,
			   (unsigned long)GetLastError());
		return FALSE;
	}

	size.QuadPart = (LONGLONG)disk_gb * 1073741824;
	if (!SetFilePointerEx(h, size, NULL, FILE_BEGIN) || !SetEndOfFile(h)) {
		work_fatal("Could not reserve %d GB for the disk image."
			   " Is there enough room?", disk_gb);
		CloseHandle(h);
		DeleteFile(path);
		return FALSE;
	}

	/*
	 * Valid data length too, not just size.
	 *
	 * SetEndOfFile reserves the space but leaves VDL at zero, and NTFS
	 * zero-fills everything between VDL and any write's offset --
	 * synchronously, inside that write. The first thing the guest does to
	 * this disk is mke2fs, whose superblocks land gigabytes in, so the
	 * guest's one CPU sat inside a single cobd request for minutes while
	 * NTFS wrote sixteen gigabytes of zeroes underneath it. From outside
	 * that is a dead guest, and it was diagnosed as one.
	 *
	 * SetFileValidData skips the fill; the privilege it wants is held by
	 * admins, which Setup requires anyway. The contents below VDL become
	 * whatever the disk held -- fine for a disk image mke2fs is about to
	 * own. If it is refused, the reservation still stands and only the
	 * first deep writes pay the old cost, so refusal is reported, not
	 * fatal.
	 */
	{
		HANDLE tok;
		BOOL vdl_ok = FALSE;

		if (OpenProcessToken(GetCurrentProcess(),
				     TOKEN_ADJUST_PRIVILEGES, &tok)) {
			TOKEN_PRIVILEGES tp;

			ZeroMemory(&tp, sizeof(tp));
			tp.PrivilegeCount = 1;
			tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
			if (LookupPrivilegeValue(NULL, "SeManageVolumePrivilege",
						 &tp.Privileges[0].Luid))
				AdjustTokenPrivileges(tok, FALSE, &tp,
						      0, NULL, NULL);
			CloseHandle(tok);
		}

		vdl_ok = SetFileValidData(h, size.QuadPart);
		if (!vdl_ok)
			work_say("root.img: no SeManageVolumePrivilege --"
				 " the first writes will be slow");
	}

	work_say("root.img, %d GB reserved", disk_gb);
	CloseHandle(h);
	return TRUE;
}

/* ------------------------------------------------------------ the Linux half */

/*
 * Talking to the guest.
 *
 * The build writes its progress to /var/log/mocolinux-setup.log inside the guest
 * and not to the console, deliberately: hvc0 is the only byte stream out of this
 * machine and it already carries a login shell, so a build writing there too puts
 * two writers on one tty and the reader gets whichever won. A file also has
 * history, which a stream does not -- Setup can be asked "where are you" at any
 * moment and get a true answer, rather than only seeing what happened to pass
 * while it was attached.
 *
 * So the conversation is request and reply: send a command, read until a sentinel
 * this side chose, take what came back. That is what colinux-daemon --console
 * serves on a TCP port, and it is the same protocol tools/coterm.py has used for
 * every measurement in this project.
 */

#define GUEST_PORT 2323

/*
 * tools/coterm.py's sentinel, verbatim -- the protocol below is that client's,
 * re-spoken in C, because it is the client that has driven every measurement in
 * this project, and this file's own invention (MOCOEND, required twice) is the
 * part that failed: it relied on finding its own echo intact, and a tty is
 * entitled to interleave a redraw anywhere, including mid-word.
 *
 * The literal is split with a shell-level quote. The echoed command line
 * contains COTERM''_END_ and only the executed printf produces
 * COTERM_END_<status>, so matching the joined form cannot match our own echo,
 * no matter what the line editor does to it. It carries $? because the exit
 * status is the thing actually being asked about.
 */
#define SENTINEL_SEND "printf '\\nCOTERM''_END_%d\\n' $?"
#define SENTINEL_TEXT "COTERM_END_"
#define SENTINEL_ECHO "''_END_"

static SOCKET guest_sock = INVALID_SOCKET;

/*
 * Every attempt to talk to the guest, on disk.
 *
 * Setup has now twice believed a healthy guest had not booted, and both times
 * the diagnosis cost an evening because the conversation happened in memory
 * and vanished with the window. The children are DETACHED_PROCESS, so their
 * own complaints print to nowhere; this file is the only witness.
 *
 * setup-guest.log beside the running executable, appended, one line per event,
 * opened and closed per line so a crash loses nothing.
 */
static void glog(const char *fmt, ...)
{
	char path[MAX_PATH], line[1400];
	SYSTEMTIME t;
	HANDLE h;
	DWORD n;
	va_list ap;
	int len;

	_snprintf(path, sizeof(path) - 1, "%s\\setup-guest.log", self_dir);
	path[sizeof(path) - 1] = 0;

	GetLocalTime(&t);
	_snprintf(line, sizeof(line) - 3, "%02u:%02u:%02u.%03u ",
		  t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
	line[sizeof(line) - 3] = 0;

	len = lstrlen(line);
	va_start(ap, fmt);
	_vsnprintf(line + len, sizeof(line) - 3 - len, fmt, ap);
	va_end(ap);
	line[sizeof(line) - 3] = 0;

	len = lstrlen(line);
	line[len++] = '\r';
	line[len++] = '\n';

	h = CreateFile(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
		       NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return;
	WriteFile(h, line, len, &n, NULL);
	CloseHandle(h);
}

/*
 * A copy fit for one log line: control bytes were already stripped by
 * strip_escapes, and the newlines that remain become '|' so a transcript
 * fragment stays on the line that reports it.
 */
static void glog_flatten(const char *in, char *out, int outn)
{
	int i = 0;
	int skip;

	skip = lstrlen(in) - (outn - 1);
	if (skip > 0)
		in += skip;

	while (*in && i < outn - 1) {
		out[i++] = (*in == '\n') ? '|' : *in;
		in++;
	}
	out[i] = 0;
}

/*
 * Strip what a terminal would have consumed.
 *
 * The other end is a login shell on a tty, not a pipe, so it emits control
 * sequences as well as output: zsh turns on bracketed paste, redraws its prompt,
 * sets colours. A terminal eats those; this is not a terminal, so they arrived
 * intact and got drawn -- which is why the progress line read "[?2004" instead of
 * a stage name.
 *
 * Handles the three shapes that actually turn up: CSI (ESC [ ... final byte in
 * 0x40-0x7e), OSC (ESC ] ... BEL or ESC backslash), and any other two-character
 * ESC pair. Everything else outside printable ASCII is dropped, because a status
 * line is text and nothing here needs more than that.
 */
static void strip_escapes(char *t)
{
	char *in = t, *out = t;

	while (*in) {
		if (*in == 0x1b) {
			in++;
			if (*in == '[') {
				in++;
				while (*in && !(*in >= 0x40 && *in <= 0x7e))
					in++;
				if (*in)
					in++;
			} else if (*in == ']') {
				in++;
				while (*in && *in != 0x07) {
					if (*in == 0x1b && in[1] == '\\') {
						in += 2;
						break;
					}
					in++;
				}
				if (*in == 0x07)
					in++;
			} else if (*in) {
				in++;
			}
			continue;
		}

		if ((unsigned char)*in >= 0x20 && (unsigned char)*in < 0x7f)
			*out++ = *in;
		else if (*in == '\n')
			*out++ = '\n';
		in++;
	}

	*out = 0;
}

static void guest_close(void)
{
	if (guest_sock != INVALID_SOCKET) {
		closesocket(guest_sock);
		guest_sock = INVALID_SOCKET;
	}
}

static BOOL guest_connect(void)
{
	struct sockaddr_in a;

	guest_close();

	guest_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (guest_sock == INVALID_SOCKET)
		return FALSE;

	ZeroMemory(&a, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(GUEST_PORT);
	a.sin_addr.s_addr = inet_addr("127.0.0.1");

	if (connect(guest_sock, (struct sockaddr *)&a, sizeof(a)) != 0) {
		glog("console connect 127.0.0.1:%d failed (winsock %d)",
		     GUEST_PORT, WSAGetLastError());
		guest_close();
		return FALSE;
	}

	glog("console connected");
	return TRUE;
}

/*
 * Send one line and read until the sentinel's joined form arrives.
 *
 * The match runs on text with the terminal control sequences stripped, exactly
 * as coterm matches on cleaned text: raw bytes may have a redraw or a colour
 * change interleaved anywhere, including inside the sentinel itself.
 *
 * The sentinel is complete only when its digits end in a newline -- printf
 * emits one -- so a status split across two reads cannot be taken at half its
 * value.
 */
static BOOL guest_exchange(const char *line, char *out, int outn, int timeout_ms)
{
	char raw[8192], txt[8192];
	char show[420];
	int used = 0;
	DWORD started = GetTickCount();

	raw[0] = txt[0] = 0;

	{
		char head[220];

		lstrcpyn(head, line, sizeof(head));
		glog_flatten(head, show, sizeof(show));
		glog("send: \"%s\"", show);
	}

	if (send(guest_sock, line, lstrlen(line), 0) <= 0) {
		glog("send failed (winsock %d)", WSAGetLastError());
		guest_close();
		return FALSE;
	}

	for (;;) {
		fd_set r;
		struct timeval tv;
		int n;
		char *mark;

		if (GetTickCount() - started > (DWORD)timeout_ms) {
			/*
			 * The reply may still arrive after this deadline, and a
			 * socket kept open would deliver that stale text as the
			 * answer to the next question -- the next call starts
			 * from a fresh connection and its own drain instead.
			 */
			glog_flatten(txt, show, sizeof(show));
			glog("TIMEOUT after %d ms, %d bytes received, tail: \"%s\"",
			     timeout_ms, used, show);
			guest_close();
			return FALSE;
		}

		FD_ZERO(&r);
		FD_SET(guest_sock, &r);
		tv.tv_sec = 1;
		tv.tv_usec = 0;

		if (select(0, &r, NULL, NULL, &tv) <= 0)
			continue;

		n = recv(guest_sock, raw + used, sizeof(raw) - used - 1, 0);
		if (n <= 0) {
			glog("connection lost mid-reply (recv %d, winsock %d,"
			     " %d bytes so far)", n, WSAGetLastError(), used);
			guest_close();
			return FALSE;
		}

		used += n;
		raw[used] = 0;

		lstrcpyn(txt, raw, sizeof(txt));
		strip_escapes(txt);

		mark = StrStrA(txt, SENTINEL_TEXT);
		if (mark) {
			char *d = mark + lstrlen(SENTINEL_TEXT);
			char *e = d;

			while (*e >= '0' && *e <= '9')
				e++;

			if (e > d && *e == '\n') {
				glog("ok: status %.*s after %lu ms, %d bytes",
				     (int)(e - d), d,
				     (unsigned long)(GetTickCount() - started),
				     used);
				if (out) {
					/*
					 * The reply is what sits between the echo
					 * of our command line and the sentinel.
					 * The echo is found by its split form
					 * ("''_END_"), which only the echoed
					 * command can contain -- the executed
					 * printf produces "COTERM_END_", without
					 * the quote.
					 */
					char *from = StrStrA(txt, SENTINEL_ECHO);

					if (from && from < mark) {
						while (*from && *from != '\n')
							from++;
						if (*from == '\n')
							from++;

						if (mark > from && mark[-1] == '\n')
							mark[-1] = 0;
						else
							*mark = 0;

						lstrcpyn(out, from, outn);
					} else {
						/*
						 * No clean echo boundary before the
						 * sentinel. This is the console
						 * backlog after an I/O stall
						 * flushing echoes and output
						 * interleaved -- the framing is
						 * unreliable, and the old code
						 * returned the whole buffer here,
						 * which starts with the echoed
						 * command. Handing that to the
						 * caller is what made the poll read
						 * its own "grep ... RC= ..." command
						 * as a result. Return nothing
						 * instead; the caller polls again,
						 * and the status was still valid so
						 * the exchange itself succeeded.
						 */
						out[0] = 0;
					}
				}
				return TRUE;
			}
		}

		/* A reply longer than the buffer is a command that should not have
		 * been asked; keep the tail so the sentinel can still be seen. */
		if (used > (int)sizeof(raw) - 512) {
			MoveMemory(raw, raw + used / 2, used / 2 + 1);
			used = used / 2;
		}
	}
}

/*
 * Run one command in the guest and return what it printed.
 *
 * tools/coterm.py's request-and-reply, re-spoken in C. A fresh connection first
 * asks for a bare sentinel and discards everything up to it -- coterm's sync --
 * because whatever the ring still holds from before arrives first: boot
 * messages, a previous client's half-consumed reply. Without the drain, the
 * first command's answer is the past.
 */
static BOOL guest_run(const char *cmd, char *out, int outn, int timeout_ms)
{
	char line[1024];

	if (guest_sock == INVALID_SOCKET) {
		if (!guest_connect())
			return FALSE;

		glog("fresh connection: draining to a bare sentinel");
		_snprintf(line, sizeof(line) - 1, "\n%s\n", SENTINEL_SEND);
		line[sizeof(line) - 1] = 0;
		if (!guest_exchange(line, NULL, 0, timeout_ms))
			return FALSE;
	}

	_snprintf(line, sizeof(line) - 1, "%s; %s\n", cmd, SENTINEL_SEND);
	line[sizeof(line) - 1] = 0;
	return guest_exchange(line, out, outn, timeout_ms);
}

/*
 * Start the driver, then the guest, the network and the console, as children.
 *
 * Order matters and the failures name the wrong thing when it is wrong: a guest
 * with no bridge reports "cannot resolve host", which reads as DNS. The console
 * is last because it is the only one Setup then talks to.
 *
 * A Job Object with KILL_ON_JOB_CLOSE as a backstop, not as the mechanism: the
 * children exit on their own when the guest ends, and killing a process with a
 * thread inside the driver closes its handles underneath kernel-mode code, which
 * is this port's recurring bugcheck 0x50. The job is only for the case where
 * Setup itself dies without cleaning up.
 */
static HANDLE guest_job, guest_boot, guest_net_child, guest_console_child;

static HANDLE spawn_in_dir(const char *cmdline, const char *logname)
{
	STARTUPINFO si;
	PROCESS_INFORMATION pi;
	SECURITY_ATTRIBUTES sa;
	HANDLE logh = INVALID_HANDLE_VALUE;
	char buf[1024];

	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory(&pi, sizeof(pi));
	lstrcpyn(buf, cmdline, sizeof(buf));

	/*
	 * Each child's stdout goes to a file of its own, not to nothing.
	 *
	 * These daemons narrate: the boot daemon reports every load phase and,
	 * when a run ends, dumps the guest's entire kernel log -- which is the
	 * evidence for every failure that leaves no bugcheck. DETACHED with no
	 * handles threw all of that away, and tonight it cost the one report
	 * that would have said why a booted guest never reached a shell.
	 */
	if (logname) {
		char path[MAX_PATH];

		_snprintf(path, sizeof(path) - 1, "%s\\%s", dir_linux, logname);
		path[sizeof(path) - 1] = 0;

		ZeroMemory(&sa, sizeof(sa));
		sa.nLength = sizeof(sa);
		sa.bInheritHandle = TRUE;

		logh = CreateFile(path, GENERIC_WRITE,
				  FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
				  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (logh != INVALID_HANDLE_VALUE) {
			si.dwFlags = STARTF_USESTDHANDLES;
			si.hStdInput  = INVALID_HANDLE_VALUE;
			si.hStdOutput = logh;
			si.hStdError  = logh;
		} else {
			glog("cannot create %s (error %lu) -- the child runs"
			     " with its output discarded", path,
			     (unsigned long)GetLastError());
		}
	}

	/*
	 * No console windows: three of them cover the window the user is reading,
	 * and Setup is the thing that reports progress here. Tried visible first,
	 * on the theory that matching run-manjaro.bat mattered -- the guest froze
	 * identically either way, so it does not.
	 *
	 * The image directory as the working directory, which does match the batch
	 * files. DETACHED_PROCESS rather than CREATE_NO_WINDOW so the children get
	 * no console at all instead of an invisible one; a hidden console still has
	 * handles, and this port has already lost an evening to a child inheriting
	 * console handles it should not have had.
	 */
	if (!CreateProcess(NULL, buf, NULL, NULL,
			   logh != INVALID_HANDLE_VALUE,
			   DETACHED_PROCESS, NULL, dir_linux, &si, &pi)) {
		glog("spawn FAILED (error %lu): %.300s",
		     (unsigned long)GetLastError(), cmdline);
		if (logh != INVALID_HANDLE_VALUE)
			CloseHandle(logh);
		return NULL;
	}

	glog("spawned pid %lu (stdout -> %s): %.300s",
	     (unsigned long)pi.dwProcessId, logname ? logname : "nowhere",
	     cmdline);

	/* The child holds its own reference now; keeping this one open would
	 * only stop the file being deleted after the child is gone. */
	if (logh != INVALID_HANDLE_VALUE)
		CloseHandle(logh);

	if (guest_job)
		AssignProcessToJobObject(guest_job, pi.hProcess);
	CloseHandle(pi.hThread);
	return pi.hProcess;
}

/*
 * Say so, once, when a child dies.
 *
 * The children are DETACHED_PROCESS: whatever they printed on their way out
 * went nowhere. A console server that lost the port, a bridge that lost the
 * single-instance mutex, a boot daemon that refused a disk -- all of them
 * look identical from here, which is a wizard waiting politely for a guest
 * that can never answer. An exit is loud now, in the log and on the page.
 */
static void log_child_exits(void)
{
	static int said[3];
	HANDLE h[3];
	static const char *name[3] = {
		"the Linux guest", "the network bridge", "the console server"
	};
	int i;

	h[0] = guest_boot;
	h[1] = guest_net_child;
	h[2] = guest_console_child;

	for (i = 0; i < 3; i++) {
		DWORD code = 0;

		if (!h[i] || said[i])
			continue;
		if (WaitForSingleObject(h[i], 0) != WAIT_OBJECT_0)
			continue;

		GetExitCodeProcess(h[i], &code);
		glog("CHILD EXITED: %s, code %lu (0x%lx)", name[i],
		     (unsigned long)code, (unsigned long)code);
		work_say("%s exited unexpectedly (code %lu)", name[i],
			 (unsigned long)code);
		said[i] = 1;
	}
}

/*
 * Retried, because the one place this runs is the one place it fails.
 *
 * The Linux half starts at logon, and on Windows 7 the first StartService
 * minutes after boot has been seen to fail with ERROR_PATH_NOT_FOUND against
 * an ImagePath that is verifiably there -- and succeed unchanged a moment
 * later, once the volume's letter has settled. Ten tries two seconds apart
 * costs nothing when the first one works, which on a warm machine it does.
 *
 * The last error is kept for the caller: "would not start" with no number
 * sends someone hunting a build problem, when 577 (the image hash check)
 * means exactly "test-signing is off" and error 3 means "try again".
 */
static DWORD start_driver_error;

static BOOL start_driver_service(void)
{
	SC_HANDLE scm, svc;
	int attempt;

	start_driver_error = 0;

	scm = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (!scm) {
		start_driver_error = GetLastError();
		return FALSE;
	}

	svc = OpenService(scm, "CoLinuxDriver", SERVICE_START | SERVICE_QUERY_STATUS);
	if (!svc) {
		start_driver_error = GetLastError();
		CloseServiceHandle(scm);
		return FALSE;
	}

	for (attempt = 0; attempt < 10; attempt++) {
		if (StartService(svc, 0, NULL) ||
		    GetLastError() == ERROR_SERVICE_ALREADY_RUNNING) {
			start_driver_error = 0;
			break;
		}

		start_driver_error = GetLastError();
		glog("driver start attempt %d failed (error %lu)",
		     attempt + 1, (unsigned long)start_driver_error);

		/* 577 is a verdict, not a race: the kernel refused the image's
		 * signature and will refuse it identically nine more times. */
		if (start_driver_error == ERROR_INVALID_IMAGE_HASH)
			break;

		Sleep(2000);
	}

	CloseServiceHandle(svc);
	CloseServiceHandle(scm);
	return start_driver_error == 0;
}

static BOOL build_linux(void)
{
	char cmd[1024], reply[2048];
	char base[MAX_PATH], built[MAX_PATH];
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim;
	int waited;

	_snprintf(base, sizeof(base) - 1, "%s\\%s", dir_linux, IMAGE_BASE);
	_snprintf(built, sizeof(built) - 1, "%s\\%s", dir_linux, IMAGE_BUILT);
	base[sizeof(base) - 1] = built[sizeof(built) - 1] = 0;

	work_at(6, -1, "Starting Linux");
	glog("=== Linux half starting ===");

	guest_job = CreateJobObject(NULL, NULL);
	if (guest_job) {
		ZeroMemory(&lim, sizeof(lim));
		lim.BasicLimitInformation.LimitFlags =
			JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		SetInformationJobObject(guest_job, JobObjectExtendedLimitInformation,
					&lim, sizeof(lim));
	}

	if (!start_driver_service()) {
		if (start_driver_error == ERROR_INVALID_IMAGE_HASH)
			work_fatal(secure_boot_on()
				   ? "Windows refused the driver's signature, and"
				     " UEFI Secure Boot is ON -- which overrules"
				     " test-signing. Disable Secure Boot in the"
				     " firmware setup screen, then run Setup again."
				   : "Windows refused the driver's signature."
				     " Enable test-signing (bcdedit /set testsigning"
				     " on) and restart, then run Setup again.");
		else
			work_fatal("The MoCoLinux driver would not start"
				   " (error %lu). A restart may be needed before"
				   " Setup can continue.",
				   (unsigned long)start_driver_error);
		return FALSE;
	}
	work_say("driver loaded");

	/*
	 * The shipped image on cobd0, the empty one on cobd1. The builder runs
	 * inside the first and writes the second, which is the arrangement the
	 * runbook has used for every image this project has produced.
	 */
	/*
	 * \DosDevices\, never \??\. They name the same object directory, but the
	 * daemon's msvcrt CRT expands ? as a wildcard in argv before main() ever
	 * runs, and when the pattern happens to match something on disk the path
	 * arrives as its own basename. It only bites when a match exists, which
	 * is why it passed every XP run for days and then ate the first E: path
	 * it saw. \DosDevices\ carries no wildcard characters and works on both.
	 */
	_snprintf(cmd, sizeof(cmd) - 1,
		  "\"%s\\colinux-daemon.exe\" --boot-kernel \"%s\\vmlinux\""
		  " --max-switches none"
		  " --cobd0 \\DosDevices\\%s --cobd1 \\DosDevices\\%s"
		  " --init /sbin/init",
		  dir_program, dir_program, base, built);
	cmd[sizeof(cmd) - 1] = 0;

	guest_boot = spawn_in_dir(cmd, "boot-daemon.log");
	if (!guest_boot) {
		work_fatal("Setup could not start the Linux guest.");
		return FALSE;
	}
	work_say("guest booting");

	{
		char aux[MAX_PATH + 64];

		/*
		 * Not with the boot daemon. Launched together, the boot that
		 * follows freezes the machine; launched by hand with the bridge
		 * coming later, it does not. Ten seconds puts the bridge's first
		 * driver traffic after the guest's memory hunt and image load
		 * instead of during them. This is the variable under test for
		 * the installer's boot freeze; if the stagger cures it, the
		 * right fix is to hold the bridge until the boot daemon reports
		 * the guest live rather than to trust a constant.
		 */
		work_say("guest loading -- network follows in 10 s");
		Sleep(10000);

		_snprintf(aux, sizeof(aux) - 1,
			  "\"%s\\colinux-slirp-net-daemon.exe\" -R", dir_program);
		aux[sizeof(aux) - 1] = 0;
		guest_net_child = spawn_in_dir(aux, "slirp-bridge.log");

		_snprintf(aux, sizeof(aux) - 1,
			  "\"%s\\colinux-daemon.exe\" --console 2323", dir_program);
		aux[sizeof(aux) - 1] = 0;
		guest_console_child = spawn_in_dir(aux, "console-server.log");
	}

	/*
	 * Half a minute is normal before the console answers, and the two ways it
	 * fails early are both not failures: refused means the console server has
	 * not bound yet, silence means the guest has not reached a prompt.
	 */
	work_at(6, -1, "Waiting for Linux to boot");
	for (waited = 0; waited < 120; waited++) {
		log_child_exits();
		if (guest_run("echo READY", reply, sizeof(reply), 8000) &&
		    StrStrA(reply, "READY"))
			break;
		Sleep(2000);
		if (waited == 20)
			work_say("still booting");
	}
	if (waited >= 120) {
		glog("gave up: no READY after %d attempts", waited);
		work_fatal("Linux did not reach a prompt. See the Setup log.");
		return FALSE;
	}
	glog("guest answered READY on attempt %d", waited + 1);
	work_say("Linux is up");

	/*
	 * Run the builder detached, with its output to the log it will be polled
	 * from. Detached because it takes minutes and the console has to stay
	 * usable to ask about progress -- a command left in the foreground would
	 * hold the only channel for the whole build.
	 */
	work_at(7, -1, "Installing Manjaro on the disk image");
	/*
	 * Wherever the maker script actually is.
	 *
	 * The base image is the Arch root, which is the one that has built a
	 * Manjaro filesystem successfully -- and it carries the script at
	 * /root/mk.sh, because that is where it was fetched to when it was run by
	 * hand. An image built by the current maker also installs it at
	 * /usr/local/bin/mkmanjarorootfs.sh. Take the first that exists rather
	 * than requiring one layout, so a rebuilt base does not silently stop
	 * working.
	 */
	/*
	 * sudo only when not already root. The Arch seed's console logs in as
	 * root and carries no sudo at all -- "sudo: command not found" killed
	 * the line while the trailing echo still said STARTED, and Setup then
	 * polled forever for a log the builder never lived to create. The
	 * Manjaro images log in as a user and do need it. Decide in the guest,
	 * where the answer is.
	 */
	/*
	 * The completion line is MOCO_RC=<status>, not RC=<status>.
	 *
	 * The old marker was "RC=", and the poll command below greps for it --
	 * so the command string itself contains "RC=". When the console echoes
	 * that command back (which a login shell does) and the reply parser
	 * hands the echo to the checks, "RC=" matches inside the daemon's own
	 * command and the install is declared failed, quoting the command as
	 * the bogus result. A distinctive token the poll still contains but
	 * that the *checks anchor on at line-start* cannot be matched by the
	 * echoed "grep ..." line, which begins with "grep".
	 */
	_snprintf(cmd, sizeof(cmd) - 1,
		  "S=; [ \"$(id -u)\" = 0 ] || S=sudo;"
		  " $S sh -c 'MK=/root/mk.sh; [ -f $MK ] ||"
		  " MK=/usr/local/bin/mkmanjarorootfs.sh;"
		  " ( sh $MK %s /dev/cobd1 > /var/log/mocolinux-setup.log 2>&1;"
		  " echo MOCO_RC=$? >> /var/log/mocolinux-setup.log ) &' &&"
		  " echo STARTED",
		  want_desktop ? "" : "--console");
	cmd[sizeof(cmd) - 1] = 0;

	if (!guest_run(cmd, reply, sizeof(reply), 30000) ||
	    !StrStrA(reply, "STARTED")) {
		work_fatal("Setup could not start the Linux installer inside the"
			   " guest.");
		return FALSE;
	}

	/*
	 * Poll the last stage line rather than accounting for every byte. The log
	 * is the source of truth and it keeps its history, so asking for the most
	 * recent marker is idempotent: a missed poll costs nothing and a
	 * reconnection loses nothing.
	 */
	{
	int log_missing = 0;
	int said_slow = 0;

	for (;;) {
		char stage[240] = {0};

		Sleep(3000);
		log_child_exits();

		if (!guest_run("grep -a '==>\\|MOCO_RC=' /var/log/mocolinux-setup.log"
			       " | tail -1", stage, sizeof(stage), 20000))
			continue;

		/*
		 * The log not existing a minute after STARTED means the
		 * builder is not running and never will be -- polling harder
		 * cannot make it appear, and the old loop had no way out.
		 */
		if (StrStrA(stage, "No such file")) {
			if (++log_missing >= 20) {
				glog("builder log never appeared after %d polls",
				     log_missing);
				work_fatal("The Linux installer never started"
					   " inside the guest.");
				return FALSE;
			}
			continue;
		}
		log_missing = 0;

		{
			char *p = stage;
			char *nl;

			/* A blank reply is a poll that raced the shell, not news. */
			while (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t')
				p++;
			nl = p;
			while (*nl && *nl != '\r' && *nl != '\n')
				nl++;
			*nl = 0;

			if (!*p)
				continue;

			/*
			 * Anchor on the line START, not a substring anywhere.
			 *
			 * The reply is meant to be a line the builder wrote: a
			 * stage ("==> ...") or the completion marker
			 * ("MOCO_RC=<status>"). Both tokens also appear inside
			 * the poll command, so if the reply is ever a command
			 * echo instead of grep output -- which is what the
			 * console backlog after an I/O stall produced -- a
			 * substring test matches the echo and reports a failure
			 * that did not happen. The echoed command begins with
			 * "grep", so a start-anchored test rejects it and the
			 * loop simply polls again.
			 */
			if (!strncmp(p, "MOCO_RC=0", 9)) {
				work_at(8, 100, "Linux is installed");
				return TRUE;
			}
			if (!strncmp(p, "MOCO_RC=", 8)) {
				work_fatal("The Linux install failed (%s)."
					   " /var/log/mocolinux-setup.log in the"
					   " guest has the detail.", p);
				return FALSE;
			}

			if (!strncmp(p, "==>", 3)) {
				work_at(7, -1, p + 4);

				/*
				 * Say that this one is the long wait, once.
				 *
				 * The package install is the single longest
				 * stretch of the build and the only one where
				 * the stage line stops moving: every package
				 * comes down one at a time through a
				 * single-threaded NAT and is unpacked by a
				 * guest with one processor. From outside, a
				 * progress line that has not changed for ten
				 * minutes is indistinguishable from a hang --
				 * and what somebody does about a hung installer
				 * is kill it, half way through writing a
				 * filesystem.
				 *
				 * Matched on the builder's own wording rather
				 * than on a stage number, because the stage
				 * numbering belongs to the script and this file
				 * should not have to be edited when the script
				 * gains a step.
				 */
				if (!said_slow && StrStrIA(p, "package groups")) {
					said_slow = 1;
					work_say("this is the slow part:"
						 " roughly 15 minutes and 2.5 GB,"
						 " and this line will not change"
						 " until it finishes -- it has not"
						 " stopped");
				}
			}
			/* Anything else is a command echo or console noise,
			 * not a builder line -- ignore it and poll again. */
		}
	}
	}
}

/*
 * Shut the guest down properly, which is the flush of everything it just built.
 *
 * poweroff and never --stop: nothing else unmounts, and ext4 in data=ordered
 * journals metadata but not contents, so a file written seconds earlier comes
 * back as a correctly-named inode with no data. Here that would be the
 * filesystem that is the entire product.
 */
static void finish_guest(void)
{
	char reply[512];

	work_at(8, -1, "Shutting Linux down");
	guest_run("S=; [ \"$(id -u)\" = 0 ] || S=sudo; $S systemctl poweroff",
		  reply, sizeof(reply), 20000);
	guest_close();

	if (guest_boot) {
		if (WaitForSingleObject(guest_boot, 90000) == WAIT_TIMEOUT)
			work_say("the guest did not exit; not forcing it");
		else
			work_say("guest shut down cleanly");
		CloseHandle(guest_boot);
		guest_boot = NULL;
	}

	if (guest_job) {
		CloseHandle(guest_job);
		guest_job = NULL;
	}
}

/*
 * What the launcher needs to know, in a plain ini beside the images because it
 * names the disks. The built system becomes cobd0 and the image that built it
 * stays on cobd1 as the recovery disk -- if the new system will not boot, the one
 * that made it still will.
 */
static void write_ini(void)
{
	char path[MAX_PATH];
	HANDLE h;
	char text[1024];
	DWORD wrote;
	int n;

	_snprintf(path, sizeof(path) - 1, "%s\\mocolinux.ini", dir_linux);
	path[sizeof(path) - 1] = 0;

	h = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
		       FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return;

	n = _snprintf(text, sizeof(text) - 1,
		      "[mocolinux]\r\n"
		      "program=%s\r\n"
		      "linux=%s\r\n"
		      "cobd0=%s\\%s\r\n"
		      "cobd1=%s\\%s\r\n"
		      "xserver=%d\r\n",
		      dir_program, dir_linux,
		      dir_linux, IMAGE_BUILT,
		      dir_linux, IMAGE_BASE,
		      want_xserver);
	if (n > 0)
		WriteFile(h, text, n, &wrote, NULL);
	CloseHandle(h);
}

/*
 * Whether the host half is already done.
 *
 * A file beside the images rather than a registry value, because it belongs with
 * the thing it describes and because a user who deletes the Linux directory has
 * genuinely undone the install. It carries the choices too, so the second run
 * does not have to ask again.
 */
static void resume_path(char *out, int n)
{
	_snprintf(out, n - 1, "%s\\setup-resume.txt", dir_linux);
	out[n - 1] = 0;
}

/*
 * Come back after the restart, on our own.
 *
 * The host half deliberately stops before the driver is loaded, so the install
 * is only half done across a reboot -- and an installer that leaves a machine in
 * that state and says nothing more is an installer that did not finish. So it
 * arranges to be run again.
 *
 * HKLM Run rather than RunOnce. RunOnce is tidier, because Windows deletes the
 * value before executing it, but that is exactly wrong here: if the second half
 * fails, or the user closes the window, the entry is already gone and nothing
 * ever finishes. This one is removed by Setup itself, and only once the Linux
 * system is actually built -- so a failed attempt is retried at the next logon,
 * which is the behaviour a twenty-minute network install needs.
 *
 * Pointed at a copy of Setup inside the program directory, not at wherever it
 * was launched from. The original may well be in a downloads folder, on a USB
 * stick, or on a share that is not there next time.
 */
#define AUTORUN_KEY   "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run"
#define AUTORUN_VALUE "MoCoLinuxSetup"

/*
 * On NT 6+ the Run key is the wrong tool: UAC launches Run-key entries with
 * the filtered token and never elevates them, so the --finish half would come
 * up unable to start a kernel service and refuse itself -- or, with a manifest
 * demanding elevation, not be launched at all. A scheduled task carries its
 * own run level; /RL HIGHEST is exactly "run this elevated at logon without
 * asking". XP's schtasks has no /RL and XP has no UAC, so 5.2 keeps the Run
 * key that has already worked there end to end.
 */
#define AUTORUN_TASK "MoCoLinuxSetup"

static void autostart_set(void)
{
	char self[MAX_PATH], copy[MAX_PATH], cmd[MAX_PATH * 2];
	HKEY key;

	GetModuleFileName(NULL, self, sizeof(self));
	_snprintf(copy, sizeof(copy) - 1, "%s\\mocolinux-setup.exe", dir_program);
	copy[sizeof(copy) - 1] = 0;

	/* Ignore failure: if it is already this file, CopyFile refuses and the
	 * path is right anyway. */
	CopyFile(self, copy, FALSE);

	if (host_is_nt6()) {
		/*
		 * \" inside the /tr value: the task's action is itself a command
		 * line, and the path has spaces. schtasks wants the whole /tr
		 * argument quoted and the embedded quotes escaped.
		 */
		_snprintf(cmd, sizeof(cmd) - 1,
			  "schtasks /create /f /tn " AUTORUN_TASK
			  " /sc onlogon /rl highest /tr \"\\\"%s\\\" --finish\"",
			  copy);
		cmd[sizeof(cmd) - 1] = 0;

		if (run_tool(cmd, 30000) == 0)
			return;

		glog("schtasks refused; falling back to the Run key");
		/* fall through: an unelevated resume that at least says what it
		 * needs beats no resume at all */
	}

	_snprintf(cmd, sizeof(cmd) - 1, "\"%s\" --finish", copy);
	cmd[sizeof(cmd) - 1] = 0;

	if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, AUTORUN_KEY, 0, NULL, 0,
			   KEY_SET_VALUE, NULL, &key, NULL) == ERROR_SUCCESS) {
		RegSetValueEx(key, AUTORUN_VALUE, 0, REG_SZ,
			      (const BYTE *)cmd, lstrlen(cmd) + 1);
		RegCloseKey(key);
	}
}

static void autostart_clear(void)
{
	HKEY key;

	/* Both mechanisms, unconditionally: the set path can fall back, so the
	 * clear path cannot afford to guess which one is in place. */
	if (host_is_nt6())
		run_tool("schtasks /delete /f /tn " AUTORUN_TASK, 30000);

	if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, AUTORUN_KEY, 0, KEY_SET_VALUE,
			 &key) == ERROR_SUCCESS) {
		RegDeleteValue(key, AUTORUN_VALUE);
		RegCloseKey(key);
	}
}

static BOOL resume_pending(void)
{
	char path[MAX_PATH];

	resume_path(path, sizeof(path));
	return GetFileAttributes(path) != INVALID_FILE_ATTRIBUTES;
}

static void resume_write(void)
{
	char path[MAX_PATH], text[256];
	HANDLE h;
	DWORD wrote;
	int n;

	resume_path(path, sizeof(path));
	h = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
		       FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return;

	n = _snprintf(text, sizeof(text) - 1,
		      "desktop=%d\r\nxserver=%d\r\ndisk_gb=%d\r\n",
		      want_desktop, want_xserver, disk_gb);
	if (n > 0)
		WriteFile(h, text, n, &wrote, NULL);
	CloseHandle(h);
}

/*
 * The choices, read back on the second run. Without this the resumed half would
 * build a desktop for somebody who asked for the console system.
 */
static void resume_read(void)
{
	char path[MAX_PATH], text[256] = {0};
	HANDLE h;
	DWORD got = 0;
	char *p;

	resume_path(path, sizeof(path));
	h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
		       FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return;

	if (ReadFile(h, text, sizeof(text) - 1, &got, NULL))
		text[got] = 0;
	CloseHandle(h);

	p = StrStrA(text, "desktop=");
	if (p)
		want_desktop = (p[8] != '0');
	p = StrStrA(text, "xserver=");
	if (p)
		want_xserver = (p[8] != '0');
	p = StrStrA(text, "disk_gb=");
	if (p) {
		int v = 0;

		for (p += 8; *p >= '0' && *p <= '9'; p++)
			v = v * 10 + (*p - '0');
		if (v > 0)
			disk_gb = v;
	}
}

static void resume_clear(void)
{
	char path[MAX_PATH];

	resume_path(path, sizeof(path));
	DeleteFile(path);
}

static DWORD WINAPI worker(LPVOID unused)
{
	char why[240];

	(void)unused;

	work_at(1, -1, "Checking this machine");

	if (!check_x64(why, sizeof(why)) ||
	    !check_version(why, sizeof(why)) ||
	    !check_secure_boot(why, sizeof(why)) ||
	    !check_hypervisor(why, sizeof(why)) ||
	    !check_admin(why, sizeof(why)) ||
	    !check_no_foreign_driver(why, sizeof(why)) ||
	    !check_nothing_running(why, sizeof(why)) ||
	    !check_fs(dir_linux, why, sizeof(why)) ||
	    !check_space(dir_linux, disk_gb, why, sizeof(why))) {
		work_fatal("%s", why);
		return 1;
	}
	work_say("this machine is suitable");

	if (!CreateDirectory(dir_program, NULL) &&
	    GetLastError() != ERROR_ALREADY_EXISTS) {
		work_fatal("Could not create %s.", dir_program);
		return 1;
	}
	if (!CreateDirectory(dir_linux, NULL) &&
	    GetLastError() != ERROR_ALREADY_EXISTS) {
		work_fatal("Could not create %s.", dir_linux);
		return 1;
	}

	if (resume_pending()) {
		/*
		 * Second run, after the restart. Everything below the copies is
		 * already done, so go straight to the guest.
		 */
		work_say("continuing after the restart");
		goto linux_half;
	}

	if (!copy_payload())
		return 1;
	enable_testsigning();		/* NT 6+ only; never fatal */

	/*
	 * The driver service is NOT created here. See linux_half.
	 */
	install_xserver();		/* never fatal; see the function */
	if (!copy_image())
		return 1;
	if (!create_image())
		return 1;

	/*
	 * Stop here and ask for a restart, before the driver is ever loaded.
	 *
	 * The driver takes its guest RAM as unbroken 32 MB physical runs, and it
	 * has to find those in whatever state the host's physical memory is in at
	 * that moment. Setup has just written well over a gigabyte to disk, so
	 * asking for them in the same session is asking at the worst possible
	 * time.
	 *
	 * doc/installer already said an installer ending in a restart is
	 * unremarkable, and then this one booted a guest inline anyway. It does
	 * not any more: the files are laid down, the restart happens, and the
	 * Linux half runs on a machine that has just started.
	 */
	resume_write();
	autostart_set();
	work_at(6, 100, "Restart Windows -- Setup finishes by itself");
	work_say("it will continue automatically at the next logon");
	EnterCriticalSection(&work_lock);
	work_done = 1;
	LeaveCriticalSection(&work_lock);

	/*
	 * Ask on the UI thread, not here. A worker thread can put up a MessageBox
	 * and it is the wrong place for it: the box pumps messages for whichever
	 * thread created it, so a modal dialog owned by this one leaves the real
	 * window unable to repaint behind it.
	 */
	if (main_wnd)
		PostMessage(main_wnd, WM_ASK_REBOOT, 0, 0);
	return 0;

linux_half:
	/*
	 * The driver service, created after the restart and not before it.
	 *
	 * enable_testsigning() ran in the first half, and bcdedit's setting does
	 * not exist until the machine has booted with it. Registering a kernel
	 * service against a test-signed binary while the loader is still
	 * enforcing full code integrity is asking the question a restart is
	 * about to answer, and on the versions that check at registration rather
	 * than at load it is asking it in the one state where the answer is no.
	 *
	 * Doing it here also means the same is true of Secure Boot and of a
	 * hypervisor: whatever the user had to change, they changed it, restarted,
	 * and only then does anything of ours go near the service manager.
	 *
	 * Idempotent, and reached on the resume path only, so an install that is
	 * re-run finds the service already correct and updates its ImagePath.
	 */
	if (!install_driver())
		return 1;

	if (!build_linux()) {
		finish_guest();
		return 1;
	}

	finish_guest();
	write_ini();

	/*
	 * The shortcuts and the logon entry, only now.
	 *
	 * They used to be made in the first half, which put moco-boot.vbs in the
	 * All Users Startup folder before the restart -- so the logon that was
	 * supposed to resume Setup booted a guest of its own first, and Setup's
	 * boot daemon then refused with "a guest is already running" (the
	 * single-instance guard doing its job). The bridge and console server
	 * refused for the same reason, and the install stalled waiting for a
	 * console that belonged to somebody else's guest.
	 *
	 * Nothing before this point needs them, and after it the system they
	 * start actually exists.
	 */
	install_shortcuts();		/* never fatal; see the function */

	/*
	 * Only here. Both of these are what make the install retry, so they come
	 * off only once there is a built Linux system to show for it.
	 */
	resume_clear();
	autostart_clear();

	work_at(8, 100, "Installed");
	EnterCriticalSection(&work_lock);
	work_done = 1;
	LeaveCriticalSection(&work_lock);
	if (main_wnd)
		PostMessage(main_wnd, WM_WORK, 0, 0);

	return 0;
}


/* ------------------------------------------------------------------ look */

/*
 * Flat, light, one accent. Deliberately not a gradient and not a photograph:
 * the two decorations an installer of this era reached for, and the two things
 * that make a window look its age.
 */
#define COL_BG		RGB(0xfa, 0xfa, 0xfb)
#define COL_RAIL	RGB(0xf1, 0xf2, 0xf4)
#define COL_LINE	RGB(0xdf, 0xe1, 0xe5)
#define COL_TEXT	RGB(0x1a, 0x1c, 0x20)
#define COL_DIM		RGB(0x6a, 0x6f, 0x78)
#define COL_ACCENT	RGB(0x1c, 0x6f, 0xb8)
#define COL_ACCENT_HOT	RGB(0x24, 0x84, 0xd6)
#define COL_ON_ACCENT	RGB(0xff, 0xff, 0xff)
#define COL_OK		RGB(0x2e, 0x8b, 0x3f)

/* Logical layout, scaled by DPI at startup. */
#define W_BASE		660
#define H_BASE		460
#define RAIL_BASE	196
#define PAD_BASE	26
#define BTNH_BASE	30
#define BTNW_BASE	96

static int scale_num = 96;	/* device DPI; 96 means no scaling */

static int S(int v)
{
	return MulDiv(v, scale_num, 96);
}

static HFONT f_h1, f_body, f_small, f_stage, f_stage_on;

/*
 * Segoe UI where it exists, Tahoma where it does not.
 *
 * The default GUI font is what makes a window look like a dialog box from 1995,
 * and it is worth the two lines to avoid it. Segoe UI arrived with Vista, so on
 * XP the honest best is Tahoma -- which is a perfectly good face and is what
 * XP's own shell uses. CreateFont falls back on its own if a name is missing,
 * but it falls back to something arbitrary, so the choice is made here instead.
 *
 * CLEARTYPE_QUALITY is understood by XP and ignored by anything older, and it is
 * the difference between text that looks drawn and text that looks rendered.
 */
static HFONT mk_font(int pt, int weight)
{
	static const char *face;
	HDC screen;
	int height;

	if (!face) {
		LOGFONT probe;
		HFONT probe_font;

		ZeroMemory(&probe, sizeof(probe));
		lstrcpyn(probe.lfFaceName, "Segoe UI", LF_FACESIZE);
		probe.lfHeight = -12;
		probe_font = CreateFontIndirect(&probe);

		face = "Tahoma";
		if (probe_font) {
			HDC dc = GetDC(NULL);
			HFONT old = (HFONT)SelectObject(dc, probe_font);
			char got[LF_FACESIZE] = {0};

			GetTextFace(dc, sizeof(got), got);
			if (lstrcmpi(got, "Segoe UI") == 0)
				face = "Segoe UI";
			SelectObject(dc, old);
			ReleaseDC(NULL, dc);
			DeleteObject(probe_font);
		}
	}

	screen = GetDC(NULL);
	height = -MulDiv(pt, GetDeviceCaps(screen, LOGPIXELSY), 72);
	ReleaseDC(NULL, screen);

	return CreateFont(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
			  DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
			  CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

/* Defined with the rest of the drawing further down; the licence box needs them
 * first, and moving the whole drawing section above the work would put the
 * paint code before the state it paints. */
static void draw_text(HDC dc, HFONT font, COLORREF colour, RECT *r,
		      const char *s, UINT flags);
static void fill(HDC dc, RECT *r, COLORREF colour);

/* ------------------------------------------------------------- the licence */

/*
 * The full text, in a box the user can scroll.
 *
 * Read from COPYING beside the executable rather than compiled in. Eighteen
 * kilobytes of licence baked into the binary would be eighteen kilobytes that
 * can drift from the file the project actually ships, and the GPL is a document
 * whose exact wording is the point. If it is missing, the page says so plainly
 * instead of showing a summary and calling it a licence.
 *
 * Drawn with the OS's own text layout but not with its widgets. An EDIT control
 * with ES_MULTILINE would come with wrapping, selection and a scrollbar for
 * free, and would also come with a themed border and a themed scrollbar that
 * look different on each Windows this may run on -- which is the whole thing
 * this file avoids. The clipping trick below gets the layout for free anyway:
 * DrawText wraps the entire text into a rectangle positioned above the visible
 * box, and the clip region hides everything scrolled past. No manual word
 * breaking, no cached line table.
 */
static char *licence_text;
static int lic_scroll;		/* pixels from the top of the wrapped text */
static int lic_total;		/* full wrapped height, measured while painting */
static RECT lic_box;		/* where it landed, for hit testing */
static RECT lic_bar;
static int lic_dragging;
static int lic_drag_from;
static int lic_drag_scroll;

static void licence_load(void)
{
	char path[MAX_PATH];
	HANDLE h;
	DWORD size, got = 0;
	int i;
	static const char *names[] = { "COPYING", "LICENSE", "LICENSE.txt", NULL };

	for (i = 0; names[i]; i++) {
		_snprintf(path, sizeof(path) - 1, "%s\\%s", self_dir, names[i]);
		path[sizeof(path) - 1] = 0;

		h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL,
			       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (h == INVALID_HANDLE_VALUE)
			continue;

		size = GetFileSize(h, NULL);
		/*
		 * A megabyte is not a licence. The bound is here because this path
		 * reads a file named by whatever sits next to the executable, and an
		 * installer should not be the thing that tries to allocate a DVD.
		 */
		if (size == INVALID_FILE_SIZE || size > 1024 * 1024) {
			CloseHandle(h);
			continue;
		}

		licence_text = (char *)HeapAlloc(GetProcessHeap(), 0, size + 1);
		if (!licence_text) {
			CloseHandle(h);
			break;
		}

		if (ReadFile(h, licence_text, size, &got, NULL))
			licence_text[got] = 0;
		else
			licence_text[0] = 0;
		CloseHandle(h);

		if (licence_text[0])
			return;

		HeapFree(GetProcessHeap(), 0, licence_text);
		licence_text = NULL;
	}
}

static void draw_licence_box(HDC dc, RECT *client, int x, int y_top)
{
	RECT box, inner, text, measure, line;
	HRGN clip;
	const char *body = licence_text ? licence_text :
		"COPYING was not found beside Setup.\r\n\r\n"
		"MoCoLinux is free software under the GNU General Public Licence, "
		"version 2. The full text should have been included with this "
		"download; if it was not, the download is incomplete and the terms "
		"can be read at https://www.gnu.org/licenses/gpl-2.0.html";

	box.left = x;
	box.right = client->right - S(PAD_BASE);
	box.top = y_top;
	box.bottom = client->bottom - S(96);
	lic_box = box;

	fill(dc, &box, RGB(0xff, 0xff, 0xff));

	/* A one-pixel rule rather than a sunken 3D frame, which is the single
	 * most 1995 decoration Windows offers. */
	line = box;
	line.bottom = line.top + 1;
	fill(dc, &line, COL_LINE);
	line = box;
	line.top = line.bottom - 1;
	fill(dc, &line, COL_LINE);
	line = box;
	line.right = line.left + 1;
	fill(dc, &line, COL_LINE);
	line = box;
	line.left = line.right - 1;
	fill(dc, &line, COL_LINE);

	/* The scrollbar's lane is reserved whether or not a thumb is needed, so
	 * the text does not reflow the moment the box becomes scrollable. */
	lic_bar = box;
	lic_bar.left = box.right - S(10);
	lic_bar.top = box.top + 1;
	lic_bar.bottom = box.bottom - 1;

	inner = box;
	inner.left += S(10);
	inner.right = lic_bar.left - S(6);
	inner.top += S(8);
	inner.bottom -= S(8);

	/* Measure the whole thing once per paint. Cheap next to laying it out. */
	measure = inner;
	{
		HFONT old = (HFONT)SelectObject(dc, f_small);

		DrawText(dc, body, -1, &measure,
			 DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
		SelectObject(dc, old);
	}
	lic_total = measure.bottom - measure.top;

	{
		int visible = inner.bottom - inner.top;
		int max = lic_total - visible;

		if (max < 0)
			max = 0;
		if (lic_scroll > max)
			lic_scroll = max;
		if (lic_scroll < 0)
			lic_scroll = 0;

		clip = CreateRectRgn(inner.left, inner.top, inner.right, inner.bottom);
		SelectClipRgn(dc, clip);

		text = inner;
		text.top -= lic_scroll;
		text.bottom = text.top + lic_total;
		draw_text(dc, f_small, COL_TEXT, &text, body,
			  DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);

		SelectClipRgn(dc, NULL);
		DeleteObject(clip);

		if (max > 0) {
			RECT thumb = lic_bar;
			int lane = lic_bar.bottom - lic_bar.top;
			int th = lane * visible / lic_total;

			if (th < S(24))
				th = S(24);
			thumb.top = lic_bar.top +
				    (lane - th) * lic_scroll / max;
			thumb.bottom = thumb.top + th;
			thumb.left += S(3);
			thumb.right -= S(3);
			fill(dc, &lic_bar, RGB(0xf4, 0xf5, 0xf7));
			fill(dc, &thumb, RGB(0xb4, 0xb9, 0xc2));
		}
	}
}

static void lic_scroll_by(int dy)
{
	lic_scroll += dy;
	if (lic_scroll < 0)
		lic_scroll = 0;
	if (main_wnd)
		InvalidateRect(main_wnd, NULL, FALSE);
}

/* ------------------------------------------------------- clickable regions */

/*
 * Hit regions, rebuilt on every paint.
 *
 * Nothing here is a child window. A real control per choice would mean a themed
 * button or radio on each one, which is the appearance this file exists to avoid,
 * and it would mean creating and moving child windows every time the page
 * changes. Drawing them and remembering where they landed is less code and looks
 * identical on every Windows.
 *
 * Rebuilt rather than computed once, because the page decides what exists: there
 * is no browse button on the welcome page, and a stale rectangle is a click that
 * does something invisible.
 */
enum {
	H_NONE = 0,
	H_BROWSE_PROGRAM,
	H_BROWSE_LINUX,
	H_SIZE_8, H_SIZE_16, H_SIZE_32, H_SIZE_64,
	H_DESKTOP, H_CONSOLE, H_XSERVER
};

#define HIT_MAX 16
static struct { RECT r; int id; } hits[HIT_MAX];
static int hit_n;
static int hit_over;

static void add_hit(RECT r, int id)
{
	if (hit_n >= HIT_MAX)
		return;
	hits[hit_n].r = r;
	hits[hit_n].id = id;
	hit_n++;
}

static int hit_at(int x, int y)
{
	POINT p;
	int i;

	p.x = x;
	p.y = y;
	for (i = 0; i < hit_n; i++)
		if (PtInRect(&hits[i].r, p))
			return hits[i].id;
	return H_NONE;
}

/* ------------------------------------------------------------- validation */

/*
 * Checked as it changes, not when Next is pressed.
 *
 * FAT32 and a full disk are both things the user can fix in a minute, and both
 * otherwise surface as a write error a long way into a package install -- which
 * reads as a corrupt download rather than as a choice made on this page.
 *
 * Recomputed on change rather than inside the painter, because it touches the
 * filesystem and a paint handler must not.
 */
static int where_ok = 1;
static char where_why[240];

static void revalidate(void)
{
	where_why[0] = 0;

	if (!check_fs(dir_linux, where_why, sizeof(where_why)) ||
	    !check_space(dir_linux, disk_gb, where_why, sizeof(where_why))) {
		where_ok = 0;
		return;
	}

	where_ok = 1;
	{
		ULARGE_INTEGER freeb, total, avail;
		char root[MAX_PATH];

		lstrcpyn(root, dir_linux, sizeof(root));
		GetVolumePathName(dir_linux, root, sizeof(root));
		if (GetDiskFreeSpaceEx(root, &freeb, &total, &avail))
			_snprintf(where_why, sizeof(where_why) - 1,
				  "NTFS, %.0f GB free \x97 room for a %d GB image.",
				  (double)freeb.QuadPart / 1073741824.0, disk_gb);
		where_why[sizeof(where_why) - 1] = 0;
	}
}

/*
 * The folder picker.
 *
 * SHBrowseForFolder, because this is the one place where matching the platform
 * beats matching our own look: it is the dialog the user already knows, and a
 * hand-drawn directory tree would be both more code and worse at the job.
 * BIF_NEWDIALOGSTYLE asks for the resizable version with a New Folder button,
 * which XP has and anything older ignores.
 */
static void pick_folder(char *into, const char *prompt)
{
	BROWSEINFO bi;
	LPITEMIDLIST id;
	char picked[MAX_PATH] = {0};

	ZeroMemory(&bi, sizeof(bi));
	bi.hwndOwner = main_wnd;
	bi.lpszTitle = prompt;
	bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

	id = SHBrowseForFolder(&bi);
	if (!id)
		return;

	if (SHGetPathFromIDList(id, picked) && picked[0]) {
		/*
		 * A MoCoLinux folder inside what they picked, not the folder
		 * itself. Choosing D:\ means the image should live in
		 * D:\MoCoLinux, not that the volume root is the install
		 * directory -- and an installer that writes its files loose into
		 * whatever was selected is how a Documents folder gets ruined.
		 * If they picked something already called MoCoLinux, take it as
		 * given rather than nesting a second one inside it.
		 */
		const char *tail = picked;
		const char *slash;
		int len = lstrlen(picked);

		for (slash = picked; *slash; slash++)
			if (*slash == '\\' && slash[1])
				tail = slash + 1;

		if (lstrcmpi(tail, "MoCoLinux") == 0)
			lstrcpyn(into, picked, MAX_PATH);
		else
			_snprintf(into, MAX_PATH - 1, "%s%sMoCoLinux", picked,
				  (len && picked[len - 1] == '\\') ? "" : "\\");
		into[MAX_PATH - 1] = 0;
		revalidate();
	}

	CoTaskMemFree(id);
}

/* --------------------------------------------------------- drawn widgets */

/* A path in a light well with a Change button beside it. */
static void draw_path_row(HDC dc, RECT area, const char *label,
			  const char *value, int hit_id)
{
	RECT lab = area, well = area, btn = area, edge;

	lab.bottom = lab.top + S(16);
	draw_text(dc, f_small, COL_DIM, &lab, label,
		  DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

	well.top = lab.bottom + S(2);
	well.bottom = well.top + S(26);
	btn.top = well.top;
	btn.bottom = well.bottom;
	btn.right = area.right;
	btn.left = btn.right - S(84);
	well.right = btn.left - S(8);

	fill(dc, &well, RGB(0xff, 0xff, 0xff));
	edge = well;
	edge.bottom = edge.top + 1; fill(dc, &edge, COL_LINE);
	edge = well; edge.top = edge.bottom - 1; fill(dc, &edge, COL_LINE);
	edge = well; edge.right = edge.left + 1; fill(dc, &edge, COL_LINE);
	edge = well; edge.left = edge.right - 1; fill(dc, &edge, COL_LINE);

	{
		RECT t = well;

		t.left += S(8);
		t.right -= S(8);
		draw_text(dc, f_small, COL_TEXT, &t, value,
			  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
			  DT_PATH_ELLIPSIS);
	}

	{
		COLORREF face = (hit_over == hit_id) ? RGB(0xf4, 0xf5, 0xf7) : COL_BG;
		HBRUSH br = CreateSolidBrush(face);
		HPEN pen = CreatePen(PS_SOLID, 1, COL_LINE);
		HBRUSH oldbr = (HBRUSH)SelectObject(dc, br);
		HPEN oldpen = (HPEN)SelectObject(dc, pen);

		Rectangle(dc, btn.left, btn.top, btn.right, btn.bottom);
		SelectObject(dc, oldbr);
		SelectObject(dc, oldpen);
		DeleteObject(br);
		DeleteObject(pen);

		draw_text(dc, f_small, COL_TEXT, &btn, "Change\x85",
			  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
		add_hit(btn, hit_id);
	}
}

/* One of a set: a filled dot when chosen, a ring when not. */
static void draw_choice(HDC dc, RECT area, const char *text, const char *note,
			int chosen, int hit_id, int radio)
{
	int cy = area.top + S(9);
	int cx = area.left + S(7);
	int rad = S(6);
	RECT t;
	HBRUSH br, oldbr;
	HPEN pen, oldpen;

	br = CreateSolidBrush(chosen ? COL_ACCENT :
			      (hit_over == hit_id ? RGB(0xff, 0xff, 0xff) : COL_BG));
	pen = CreatePen(PS_SOLID, 1, chosen ? COL_ACCENT : RGB(0xb0, 0xb5, 0xbd));
	oldbr = (HBRUSH)SelectObject(dc, br);
	oldpen = (HPEN)SelectObject(dc, pen);
	if (radio)
		Ellipse(dc, cx - rad, cy - rad, cx + rad, cy + rad);
	else
		Rectangle(dc, cx - rad, cy - rad, cx + rad, cy + rad);
	SelectObject(dc, oldbr);
	SelectObject(dc, oldpen);
	DeleteObject(br);
	DeleteObject(pen);

	if (chosen) {
		/* A white pip, rather than a tick glyph: a drawn tick at this size
		 * is mush, and the filled shape already carries the state. */
		RECT pip;

		pip.left = cx - S(2);
		pip.right = cx + S(2);
		pip.top = cy - S(2);
		pip.bottom = cy + S(2);
		fill(dc, &pip, RGB(0xff, 0xff, 0xff));
	}

	t = area;
	t.left = cx + S(16);
	t.bottom = t.top + S(18);
	draw_text(dc, f_body, COL_TEXT, &t, text,
		  DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

	if (note) {
		t.top += S(17);
		t.bottom = t.top + S(16);
		draw_text(dc, f_small, COL_DIM, &t, note,
			  DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
	}

	{
		RECT click = area;

		click.bottom = area.top + (note ? S(34) : S(20));
		add_hit(click, hit_id);
	}
}

/* The disk size, as four sizes rather than a spinner nobody reads. */
static void draw_sizes(HDC dc, RECT area)
{
	static const int gb[4] = { 8, 16, 32, 64 };
	static const int id[4] = { H_SIZE_8, H_SIZE_16, H_SIZE_32, H_SIZE_64 };
	int i;
	int w = S(60), gap = S(8);

	for (i = 0; i < 4; i++) {
		RECT b;
		char label[16];
		int chosen = (disk_gb == gb[i]);
		HBRUSH br, oldbr;
		HPEN pen, oldpen;

		b.left = area.left + i * (w + gap);
		b.right = b.left + w;
		b.top = area.top;
		b.bottom = b.top + S(26);

		br = CreateSolidBrush(chosen ? COL_ACCENT :
				      (hit_over == id[i] ? RGB(0xf4, 0xf5, 0xf7)
							 : COL_BG));
		pen = CreatePen(PS_SOLID, 1, chosen ? COL_ACCENT : COL_LINE);
		oldbr = (HBRUSH)SelectObject(dc, br);
		oldpen = (HPEN)SelectObject(dc, pen);
		Rectangle(dc, b.left, b.top, b.right, b.bottom);
		SelectObject(dc, oldbr);
		SelectObject(dc, oldpen);
		DeleteObject(br);
		DeleteObject(pen);

		_snprintf(label, sizeof(label) - 1, "%d GB", gb[i]);
		label[sizeof(label) - 1] = 0;
		draw_text(dc, f_small, chosen ? COL_ON_ACCENT : COL_TEXT, &b, label,
			  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
		add_hit(b, id[i]);
	}
}

/* ------------------------------------------------------------------ pages */

/*
 * The stage list in the rail is the page list, so the user can see the whole
 * shape of what is about to happen rather than discovering it one Next at a
 * time. doc/installer section 3 names these.
 */
enum {
	PG_WELCOME,
	PG_LICENCE,
	PG_WHERE,
	PG_WHAT,
	PG_INSTALL,
	PG_SETUP,
	PG_DONE,
	PG_COUNT
};

static const char *page_name[PG_COUNT] = {
	"Welcome",
	"Licence",
	"Where to put it",
	"What to install",
	"Installing",
	"Setting up Linux",
	"Finished",
};

static int page = PG_WELCOME;

/* ------------------------------------------------------------------ buttons */

enum { B_BACK, B_NEXT, B_CANCEL, B_COUNT };

static const char *btn_label[B_COUNT] = { "Back", "Next", "Cancel" };
static RECT btn_rect[B_COUNT];
static int btn_hot = -1, btn_down = -1;

/*
 * "Next" on the last page is a Finish, and saying so matters: a button labelled
 * Next on a page that says "Finished" reads as though there is more to come.
 */
static const char *btn_text(int b)
{
	if (b == B_NEXT && page == PG_DONE)
		return "Finish";
	return btn_label[b];
}

static BOOL btn_enabled(int b)
{
	if (b == B_BACK)
		return page > PG_WELCOME && page < PG_INSTALL;
	if (b == B_NEXT) {
		/*
		 * Greyed rather than complained about. The page already says what
		 * is wrong and in red; a dialog on top of that would only be the
		 * same sentence again with an OK button.
		 */
		if (page == PG_WHERE && !where_ok)
			return FALSE;

		/*
		 * The install page holds the user until the work is finished, and
		 * then must let go.
		 *
		 * This used to be `page < PG_INSTALL` and nothing else, which
		 * disabled Next for the whole second half of the wizard: once the
		 * install completed there was no way to reach "Setting up Linux",
		 * and on the last page Next, Back and Cancel were all dead at
		 * once -- the wizard could finish its work and then not be
		 * finished. A successful install is precisely the moment Next has
		 * to become available.
		 *
		 * On failure it stays disabled: the page says Setup cannot
		 * continue, and Cancel is the only honest way out of that.
		 */
		if (page == PG_INSTALL) {
			int done, failed;

			EnterCriticalSection(&work_lock);
			done   = work_done;
			failed = work_failed;
			LeaveCriticalSection(&work_lock);

			return done && !failed;
		}

		return TRUE;	/* PG_SETUP onward, and PG_DONE as Finish */
	}

	/* Cancel is meaningless once everything is installed; Finish is the way out. */
	return page != PG_DONE;
}

static void start_work(void);
static void resume_read(void);

static void start_work(void)
{
	HANDLE t;

	/*
	 * On its own thread, because everything it does blocks: copying fifty
	 * megabytes, asking the service manager for a kernel driver, reserving
	 * sixteen gigabytes. Doing any of that on the message loop gives the
	 * window the one behaviour it must never have -- refusing to repaint --
	 * which Windows then paints over with "Not Responding".
	 */
	t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
	if (t)
		CloseHandle(t);
	else
		work_fatal("Setup could not start its installer thread.");
}

/*
 * Where things go by default.
 *
 * Two directories because they are wildly different sizes and people put them on
 * different disks: 60 MB of program, and an image measured in tens of gigabytes.
 * doc/installer section 8 has the reasoning, including why the ini lives with the
 * image rather than the program -- it names the disks.
 */
static void default_paths(void)
{
	char progfiles[MAX_PATH] = {0};

	GetModuleFileName(NULL, self_dir, sizeof(self_dir));
	{
		char *slash = self_dir;
		char *last = NULL;

		for (; *slash; slash++)
			if (*slash == '\\')
				last = slash;
		if (last)
			*last = 0;
	}

	if (SHGetSpecialFolderPath(NULL, progfiles, CSIDL_PROGRAM_FILES, FALSE))
		_snprintf(dir_program, sizeof(dir_program) - 1,
			  "%s\\MoCoLinux", progfiles);
	else
		lstrcpyn(dir_program, "C:\\Program Files\\MoCoLinux",
			 sizeof(dir_program));
	dir_program[sizeof(dir_program) - 1] = 0;

	/*
	 * The image goes to the root of the system drive, not under Program
	 * Files. It is data the user may want to move, back up or delete, and
	 * burying tens of gigabytes inside a program directory makes all three
	 * harder for no benefit.
	 */
	{
		char win[MAX_PATH] = {0};

		GetWindowsDirectory(win, sizeof(win));
		if (win[1] == ':')
			_snprintf(dir_linux, sizeof(dir_linux) - 1,
				  "%c:\\MoCoLinux", win[0]);
		else
			lstrcpyn(dir_linux, "C:\\MoCoLinux", sizeof(dir_linux));
		dir_linux[sizeof(dir_linux) - 1] = 0;
	}
}

/* ------------------------------------------------------------------ drawing */

static void draw_text(HDC dc, HFONT font, COLORREF colour, RECT *r,
		      const char *s, UINT flags)
{
	HFONT old = (HFONT)SelectObject(dc, font);

	SetTextColor(dc, colour);
	SetBkMode(dc, TRANSPARENT);
	DrawText(dc, s, -1, r, flags);
	SelectObject(dc, old);
}

static void fill(HDC dc, RECT *r, COLORREF colour)
{
	HBRUSH b = CreateSolidBrush(colour);

	FillRect(dc, r, b);
	DeleteObject(b);
}

/*
 * A dot per stage, filled when done, ringed when current, hollow when ahead.
 *
 * Not a numbered list: the numbers in doc/installer's stage table are for the
 * marker protocol, and showing them here would invite the reader to check them
 * against a progress bar that counts something else.
 */
static void draw_rail(HDC dc, RECT *client)
{
	RECT rail = *client;
	RECT line;
	int y = S(PAD_BASE) + S(46);
	int i;

	rail.right = S(RAIL_BASE);
	fill(dc, &rail, COL_RAIL);

	line = rail;
	line.left = line.right - 1;
	fill(dc, &line, COL_LINE);

	{
		RECT t = rail;

		t.left = S(PAD_BASE);
		t.top = S(PAD_BASE);
		t.right = rail.right - S(12);
		t.bottom = t.top + S(28);
		draw_text(dc, f_h1, COL_TEXT, &t, "MoCoLinux " MOCO_VERSION,
			  DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
	}

	for (i = 0; i < PG_COUNT; i++) {
		RECT t;
		int cy = y + S(7);
		int cx = S(PAD_BASE) + S(4);
		int rad = S(4);
		HBRUSH b;
		HPEN pen, oldpen;
		HBRUSH oldb;

		if (i < page) {
			b = CreateSolidBrush(COL_OK);
			pen = CreatePen(PS_SOLID, 1, COL_OK);
		} else if (i == page) {
			b = CreateSolidBrush(COL_ACCENT);
			pen = CreatePen(PS_SOLID, S(1), COL_ACCENT);
			rad = S(5);
		} else {
			b = CreateSolidBrush(COL_RAIL);
			pen = CreatePen(PS_SOLID, 1, RGB(0xc2, 0xc6, 0xcc));
		}

		oldb = (HBRUSH)SelectObject(dc, b);
		oldpen = (HPEN)SelectObject(dc, pen);
		Ellipse(dc, cx - rad, cy - rad, cx + rad, cy + rad);
		SelectObject(dc, oldb);
		SelectObject(dc, oldpen);
		DeleteObject(b);
		DeleteObject(pen);

		t.left = cx + S(14);
		t.top = y;
		t.right = rail.right - S(10);
		t.bottom = y + S(20);
		draw_text(dc, i == page ? f_stage_on : f_stage,
			  i == page ? COL_TEXT : COL_DIM, &t,
			  page_name[i], DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

		y += S(26);
	}
}

static void draw_button(HDC dc, int b)
{
	RECT r = btn_rect[b];
	BOOL on = btn_enabled(b);
	BOOL primary = (b == B_NEXT);
	COLORREF face, text;
	HPEN pen, oldpen;
	HBRUSH br, oldbr;

	if (primary) {
		face = !on ? RGB(0xc8, 0xd4, 0xe0)
			   : (btn_down == b ? COL_ACCENT
					    : (btn_hot == b ? COL_ACCENT_HOT
							    : COL_ACCENT));
		text = COL_ON_ACCENT;
	} else {
		face = btn_down == b ? RGB(0xe4, 0xe6, 0xea)
				     : (btn_hot == b && on ? RGB(0xf4, 0xf5, 0xf7)
							   : COL_BG);
		text = on ? COL_TEXT : RGB(0xa8, 0xad, 0xb5);
	}

	br = CreateSolidBrush(face);
	pen = CreatePen(PS_SOLID, 1, primary ? face : COL_LINE);
	oldbr = (HBRUSH)SelectObject(dc, br);
	oldpen = (HPEN)SelectObject(dc, pen);
	/* Square. Rounded corners were a Vista mannerism and read as dated now. */
	Rectangle(dc, r.left, r.top, r.right, r.bottom);
	SelectObject(dc, oldbr);
	SelectObject(dc, oldpen);
	DeleteObject(br);
	DeleteObject(pen);

	draw_text(dc, f_body, text, &r, btn_text(b),
		  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

/*
 * Page bodies.
 *
 * Text only for now, and honest about it: the controls these pages need --
 * directory pickers with live validation, the install tickboxes, the disk-size
 * choice, a progress bar fed by MOCO: markers -- are the next piece of work.
 * What this establishes is the frame, the type and the geometry, because those
 * are what decide whether the thing looks current, and they are cheaper to get
 * right before there are widgets sitting on top of them.
 */
static void draw_page(HDC dc, RECT *client)
{
	RECT r;
	int x = S(RAIL_BASE) + S(PAD_BASE);
	int top = S(PAD_BASE);
	const char *head, *body;

	switch (page) {
	case PG_WELCOME:
		head = "Linux, on the processor Windows is using";
		body = "MoCoLinux runs a Linux kernel cooperatively with Windows on "
		       "the same processor \x97 not in a virtual machine. Linux "
		       "applications appear as ordinary Windows windows.\n\n"
		       "Setup needs about 15 minutes and downloads roughly 2.5 GB, "
		       "or about two minutes and 250 MB if you choose the console "
		       "system.";
		break;
	case PG_LICENCE: {
		RECT after;

		r.left = x;
		r.top = top;
		r.right = client->right - S(PAD_BASE);
		r.bottom = top + S(30);
		draw_text(dc, f_h1, COL_TEXT, &r, "Licence",
			  DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

		r.top = top + S(38);
		r.bottom = r.top + S(20);
		draw_text(dc, f_body, COL_DIM, &r,
			  "MoCoLinux is free software under the GNU General Public "
			  "Licence, version 2.",
			  DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

		draw_licence_box(dc, client, x, top + S(64));

		after.left = x;
		after.right = client->right - S(PAD_BASE);
		after.top = lic_box.bottom + S(8);
		after.bottom = after.top + S(20);
		draw_text(dc, f_small, COL_DIM, &after,
			  "A port of Cooperative Linux by Dan Aloni and contributors. "
			  "Continuing accepts these terms.",
			  DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
		return;
	}
	case PG_WHERE: {
		RECT row;

		r.left = x;
		r.top = top;
		r.right = client->right - S(PAD_BASE);
		r.bottom = top + S(30);
		draw_text(dc, f_h1, COL_TEXT, &r, "Where to put it",
			  DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

		r.top = top + S(36);
		r.bottom = r.top + S(18);
		draw_text(dc, f_small, COL_DIM, &r,
			  "Two directories: 60 MB of program, and an image measured in "
			  "gigabytes.",
			  DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

		row = r;
		row.top = top + S(62);
		row.bottom = row.top + S(46);
		draw_path_row(dc, row, "Program", dir_program, H_BROWSE_PROGRAM);

		row.top = row.top + S(56);
		row.bottom = row.top + S(46);
		draw_path_row(dc, row, "Linux system", dir_linux, H_BROWSE_LINUX);

		row.top = row.top + S(60);
		row.bottom = row.top + S(16);
		draw_text(dc, f_small, COL_DIM, &row, "Disk image size",
			  DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

		row.top += S(18);
		draw_sizes(dc, row);

		/*
		 * The verdict, in the colour of the answer. Reserved space is
		 * committed at creation, not sparse, so "enough room" has to be
		 * true now rather than at the moment pacman fills the volume.
		 */
		row.top += S(38);
		row.bottom = row.top + S(34);
		draw_text(dc, f_small, where_ok ? COL_OK : RGB(0xc0, 0x39, 0x2b),
			  &row, where_why,
			  DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
		return;
	}
	case PG_WHAT: {
		RECT row;

		r.left = x;
		r.top = top;
		r.right = client->right - S(PAD_BASE);
		r.bottom = top + S(30);
		draw_text(dc, f_h1, COL_TEXT, &r, "What to install",
			  DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

		r.top = top + S(36);
		r.bottom = r.top + S(18);
		draw_text(dc, f_small, COL_DIM, &r,
			  "Both boot the same Linux. The difference is what gets "
			  "downloaded.",
			  DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

		row = r;
		row.top = top + S(64);
		row.bottom = row.top + S(40);
		draw_choice(dc, row, "Desktop",
			    "KDE Plasma, browser, applications \x97 about 2.5 GB",
			    want_desktop, H_DESKTOP, 1);

		row.top += S(44);
		row.bottom = row.top + S(40);
		draw_choice(dc, row, "Console only",
			    "A shell and a network \x97 ready in about two minutes",
			    !want_desktop, H_CONSOLE, 1);

		row.top += S(56);
		row.bottom = row.top + S(40);
		draw_choice(dc, row, "Install the X server",
			    "VcXsrv 1.14.2.1, so Linux windows appear on this"
			    " desktop as ordinary windows",
			    want_xserver, H_XSERVER, 0);
		return;
	}
	case PG_INSTALL: {
		/*
		 * The one page that draws live state, so it is built here rather
		 * than described by two strings like the others.
		 */
		static char shown[160], failtext[240];
		static char lines[8][160];
		int n, i, stage, pct, failed, done;
		RECT b;

		EnterCriticalSection(&work_lock);
		stage = work_stage;
		pct = work_pct;
		failed = work_failed;
		done = work_done;
		lstrcpyn(shown, work_what, sizeof(shown));
		lstrcpyn(failtext, work_fail, sizeof(failtext));
		n = work_log_n;
		for (i = 0; i < n; i++)
			lstrcpyn(lines[i], work_log[i], sizeof(lines[0]));
		LeaveCriticalSection(&work_lock);

		r.left = x;
		r.top = top;
		r.right = client->right - S(PAD_BASE);
		r.bottom = top + S(30);
		draw_text(dc, f_h1, COL_TEXT, &r,
			  failed ? "Setup cannot continue" :
			  done ? "Installed" : "Installing",
			  DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

		r.top = top + S(40);
		r.bottom = r.top + S(20);
		draw_text(dc, f_body, COL_DIM, &r,
			  failed ? failtext : (shown[0] ? shown : "Starting"),
			  DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);

		/*
		 * The bar. Drawn, not a comctl32 PROGRESS_CLASS: that control looks
		 * different on each of the three Windows versions this may run on,
		 * and on XP with themes it is the green striped one, which is the
		 * single most dated pixel available.
		 *
		 * Indeterminate is shown as a full-width dim track rather than a
		 * crawling barber pole, because most of what this installer waits
		 * for genuinely has no percentage and pretending otherwise is how
		 * progress bars come to mean nothing.
		 */
		if (!failed) {
			b.left = x;
			b.right = client->right - S(PAD_BASE);
			b.top = top + S(78);
			b.bottom = b.top + S(6);
			fill(dc, &b, RGB(0xe4, 0xe6, 0xea));

			if (pct >= 0) {
				b.right = b.left + (b.right - b.left) * pct / 100;
				fill(dc, &b, COL_ACCENT);
			} else if (stage > 0) {
				RECT t = b;

				t.right = t.left + (t.right - t.left) *
					  (stage < work_total ? stage : work_total) /
					  work_total;
				fill(dc, &t, RGB(0xa9, 0xc6, 0xe2));
			}

			r.left = x;
			r.top = top + S(92);
			r.right = client->right - S(PAD_BASE);
			r.bottom = r.top + S(18);
			{
				char step[64];

				_snprintf(step, sizeof(step) - 1, "Step %d of %d",
					  stage > 0 ? stage : 1, work_total);
				step[sizeof(step) - 1] = 0;
				draw_text(dc, f_small, COL_DIM, &r, step,
					  DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
			}
		}

		/* The log, always visible here rather than folded away: this is
		 * the stretch where something goes wrong, and hiding the only
		 * evidence behind a disclosure triangle serves nobody. */
		r.left = x;
		r.top = top + S(124);
		r.right = client->right - S(PAD_BASE);
		r.bottom = r.top + S(16);
		for (i = 0; i < n; i++) {
			draw_text(dc, f_small,
				  i == n - 1 ? COL_DIM : RGB(0x9a, 0x9f, 0xa8),
				  &r, lines[i],
				  DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
			r.top += S(15);
			r.bottom += S(15);
		}
		return;
	}
	case PG_SETUP:
		head = "Setting up Linux";
		body = "Linux is building its own system on the disk image now, and "
		       "reporting progress as it goes. This is the long part.\n\n"
		       "You can leave it; it does not need anything from you.";
		break;
	default:
		head = "Finished";
		body = "MoCoLinux is installed.\n\nWindows should be restarted "
		       "before Linux is started for the first time \x97 the driver "
		       "reserves its memory when it loads, and it finds it "
		       "reliably on a machine that has just started. Finish "
		       "offers to do it.\n\nAfter that MoCoLinux starts by itself "
		       "at logon, and there are shortcuts on the desktop.";
		break;
	}

	r.left = x;
	r.top = top;
	r.right = client->right - S(PAD_BASE);
	r.bottom = top + S(120);

	/*
	 * Measure the heading, then put the body under it.
	 *
	 * A fixed offset was wrong the moment a heading wrapped: "Linux, on the
	 * processor Windows is using" takes two lines at this width, and the body
	 * was drawn straight through its second line. DT_CALCRECT reports the
	 * height the text will actually occupy without drawing anything, which is
	 * the only way to lay out a page whose strings are not fixed.
	 */
	{
		RECT measure = r;
		HFONT old = (HFONT)SelectObject(dc, f_h1);

		DrawText(dc, head, -1, &measure,
			 DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
		SelectObject(dc, old);

		draw_text(dc, f_h1, COL_TEXT, &r, head,
			  DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);

		r.top = measure.bottom + S(14);
	}

	r.bottom = client->bottom - S(70);
	draw_text(dc, f_body, COL_DIM, &r, body,
		  DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
}

static void paint(HWND wnd)
{
	PAINTSTRUCT ps;
	HDC dc = BeginPaint(wnd, &ps);
	RECT client;
	HDC mem;
	HBITMAP bmp, oldbmp;
	RECT bar, line;
	int i;

	GetClientRect(wnd, &client);

	/*
	 * Double buffered. Drawing a whole window straight to the screen DC
	 * flickers, and flicker is the other thing that makes software look old.
	 */
	mem = CreateCompatibleDC(dc);
	bmp = CreateCompatibleBitmap(dc, client.right, client.bottom);
	oldbmp = (HBITMAP)SelectObject(mem, bmp);

	/* Cleared before the page draws, because the page is what declares which
	 * regions exist this frame. */
	hit_n = 0;

	fill(mem, &client, COL_BG);
	draw_rail(mem, &client);
	draw_page(mem, &client);

	bar = client;
	bar.top = client.bottom - S(56);
	bar.left = S(RAIL_BASE);
	line = bar;
	line.bottom = line.top + 1;
	fill(mem, &line, COL_LINE);

	for (i = 0; i < B_COUNT; i++)
		draw_button(mem, i);

	BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);

	SelectObject(mem, oldbmp);
	DeleteObject(bmp);
	DeleteDC(mem);
	EndPaint(wnd, &ps);
}

static void layout(HWND wnd)
{
	RECT client;
	int y, right, i;

	GetClientRect(wnd, &client);
	y = client.bottom - S(56) + (S(56) - S(BTNH_BASE)) / 2;
	right = client.right - S(PAD_BASE);

	/* Cancel, Next, Back from the right, which is the platform's order. */
	for (i = 0; i < B_COUNT; i++) {
		int b = (i == 0) ? B_CANCEL : (i == 1 ? B_NEXT : B_BACK);

		btn_rect[b].right = right;
		btn_rect[b].left = right - S(BTNW_BASE);
		btn_rect[b].top = y;
		btn_rect[b].bottom = y + S(BTNH_BASE);
		right = btn_rect[b].left - S(8);
	}
}

static int hit_button(int x, int y)
{
	POINT p;
	int i;

	p.x = x;
	p.y = y;
	for (i = 0; i < B_COUNT; i++)
		if (PtInRect(&btn_rect[i], p) && btn_enabled(i))
			return i;
	return -1;
}

/*
 * Restart Windows.
 *
 * The privilege has to be enabled first: SeShutdownPrivilege is present in an
 * administrator's token but disabled, and ExitWindowsEx just fails without
 * saying why.
 *
 * EWX_FORCEIFHUNG, not EWX_FORCE: a hung application should not be able to
 * block the restart, but nobody else's unsaved work goes with it either.
 */
static BOOL reboot_windows(void)
{
	HANDLE tok;
	TOKEN_PRIVILEGES tp;

	if (OpenProcessToken(GetCurrentProcess(),
			     TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
		LookupPrivilegeValue(NULL, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
		tp.PrivilegeCount = 1;
		tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
		AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL);
		CloseHandle(tok);
	}

	return ExitWindowsEx(EWX_REBOOT | EWX_FORCEIFHUNG, 0);
}

/*
 * Finish, and offer the restart that should come before the first launch.
 *
 * Asked rather than stated. The last page has said "please restart" in prose
 * since the beginning, which is precisely the kind of instruction a user
 * clicks past on their way out of a wizard -- and the cost of missing it is
 * not cosmetic. The driver reserves its guest RAM as unbroken 32 MB physical
 * runs when it loads, and this Setup has just written a couple of gigabytes
 * through the cache and run a guest for ten minutes, so the host's physical
 * memory is as fragmented as it ever gets. A first launch on a fresh boot is
 * the difference between a guest that starts and one that grinds the machine
 * hunting for runs that no longer exist in one piece.
 *
 * Only offered when there is something installed to launch: a failed or
 * abandoned run has nothing to gain from a restart, and asking anyway would
 * read as the installer trying to hide a failure behind a reboot.
 */
static void finish_setup(HWND wnd)
{
	int done, failed;

	EnterCriticalSection(&work_lock);
	done   = work_done;
	failed = work_failed;
	LeaveCriticalSection(&work_lock);

	if (done && !failed) {
		if (MessageBox(wnd,
			"MoCoLinux is installed.\n\n"
			"Please restart Windows before starting Linux for the "
			"first time. The driver reserves its memory when it "
			"loads, and it finds it reliably on a machine that has "
			"just started.\n\n"
			"MoCoLinux starts by itself when you log back in.\n\n"
			"Restart now?",
			"MoCoLinux Setup", MB_YESNO | MB_ICONQUESTION) == IDYES) {
			if (!reboot_windows())
				MessageBox(wnd,
					"Windows would not restart. Please "
					"restart it yourself before starting "
					"Linux for the first time.",
					"MoCoLinux Setup", MB_OK | MB_ICONWARNING);
		}
	}

	DestroyWindow(wnd);
}

static LRESULT CALLBACK proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_CREATE:
		layout(wnd);
		return 0;

	case WM_SIZE:
		layout(wnd);
		InvalidateRect(wnd, NULL, FALSE);
		return 0;

	case WM_WORK:
		InvalidateRect(wnd, NULL, FALSE);
		return 0;

	case WM_ASK_REBOOT: {
		int answer;

		/* Let the finished state paint before the dialog covers it. */
		InvalidateRect(wnd, NULL, FALSE);
		UpdateWindow(wnd);

		answer = MessageBox(wnd,
			"The Windows part of MoCoLinux is installed.\n\n"
			"Setup needs to restart Windows to finish. The Linux system "
			"is built after the restart, and Setup continues by itself "
			"when you log back in.\n\n"
			"Restart now?",
			"MoCoLinux Setup", MB_YESNO | MB_ICONQUESTION);

		if (answer == IDYES) {
			if (!reboot_windows())
				MessageBox(wnd,
					"Windows would not restart. Please restart "
					"it yourself; Setup will continue when you "
					"log back in.",
					"MoCoLinux Setup", MB_OK | MB_ICONWARNING);
		}

		/*
		 * Quit either way. There is nothing left for this run to do, and a
		 * window sitting on a finished page invites a second click at
		 * something already done.
		 */
		DestroyWindow(wnd);
		return 0;
	}

	case WM_ERASEBKGND:
		return 1;	/* paint() covers every pixel */

	case WM_PAINT:
		paint(wnd);
		return 0;

	case WM_MOUSEWHEEL:
		if (page == PG_LICENCE) {
			/* Three lines a notch, the platform convention, measured
			 * from the small font rather than guessed at. */
			int notch = (short)HIWORD(wp) / WHEEL_DELTA;

			lic_scroll_by(-notch * S(14) * 3);
		}
		return 0;

	case WM_MOUSEMOVE: {
		int was = btn_hot;

		if (lic_dragging && page == PG_LICENCE) {
			int lane = lic_bar.bottom - lic_bar.top;
			int visible = lic_box.bottom - lic_box.top - S(16);
			int max = lic_total - visible;

			if (max > 0 && lane > 0) {
				int dy = (short)HIWORD(lp) - lic_drag_from;

				lic_scroll = lic_drag_scroll + dy * max / lane;
				lic_scroll_by(0);
			}
			return 0;
		}

		btn_hot = hit_button(LOWORD(lp), HIWORD(lp));
		{
			int over_was = hit_over;

			hit_over = hit_at((short)LOWORD(lp), (short)HIWORD(lp));
			if (was != btn_hot || over_was != hit_over)
				InvalidateRect(wnd, NULL, FALSE);
		}
		return 0;
	}

	case WM_LBUTTONDOWN: {
		POINT p;

		p.x = (short)LOWORD(lp);
		p.y = (short)HIWORD(lp);

		if (page == PG_LICENCE && PtInRect(&lic_bar, p)) {
			lic_dragging = 1;
			lic_drag_from = p.y;
			lic_drag_scroll = lic_scroll;
			SetCapture(wnd);
			return 0;
		}

		btn_down = hit_button(p.x, p.y);
		InvalidateRect(wnd, NULL, FALSE);
		return 0;
	}

	case WM_LBUTTONUP: {
		int up;

		if (lic_dragging) {
			lic_dragging = 0;
			ReleaseCapture();
			return 0;
		}

		{
			int what = hit_at((short)LOWORD(lp), (short)HIWORD(lp));

			switch (what) {
			case H_BROWSE_PROGRAM:
				pick_folder(dir_program,
					    "Where should the program go?");
				break;
			case H_BROWSE_LINUX:
				pick_folder(dir_linux,
					    "Where should the Linux system go?");
				break;
			case H_SIZE_8:  disk_gb = 8;  revalidate(); break;
			case H_SIZE_16: disk_gb = 16; revalidate(); break;
			case H_SIZE_32: disk_gb = 32; revalidate(); break;
			case H_SIZE_64: disk_gb = 64; revalidate(); break;
			case H_DESKTOP: want_desktop = 1; break;
			case H_CONSOLE: want_desktop = 0; break;
			case H_XSERVER: want_xserver = !want_xserver; break;
			default:
				what = H_NONE;
			}

			if (what != H_NONE) {
				InvalidateRect(wnd, NULL, FALSE);
				return 0;
			}
		}

		up = hit_button(LOWORD(lp), HIWORD(lp));

		if (up >= 0 && up == btn_down) {
			if (up == B_CANCEL)
				DestroyWindow(wnd);
			else if (up == B_NEXT && btn_enabled(B_NEXT)) {
				/*
				 * One rule for both halves of the wizard: Next
				 * advances whenever it is enabled, and on the last
				 * page it is the Finish. Gating this on
				 * `page < PG_INSTALL` separately from btn_enabled
				 * is how the button came to be drawn dead and
				 * behave dead for the entire second half.
				 */
				if (page == PG_DONE)
					finish_setup(wnd);
				else {
					page++;
					if (page == PG_INSTALL)
						start_work();
				}
			}
			else if (up == B_BACK && btn_enabled(B_BACK))
				page--;
		}
		btn_down = -1;
		InvalidateRect(wnd, NULL, FALSE);
		return 0;
	}

	case WM_KEYDOWN:
		/* A wizard is a keyboard thing as much as a mouse thing. */
		if (page == PG_LICENCE &&
		    (wp == VK_PRIOR || wp == VK_NEXT || wp == VK_UP ||
		     wp == VK_DOWN || wp == VK_HOME || wp == VK_END)) {
			int pageh = lic_box.bottom - lic_box.top - S(24);

			if (wp == VK_PRIOR)
				lic_scroll_by(-pageh);
			else if (wp == VK_NEXT)
				lic_scroll_by(pageh);
			else if (wp == VK_UP)
				lic_scroll_by(-S(14));
			else if (wp == VK_DOWN)
				lic_scroll_by(S(14));
			else if (wp == VK_HOME) {
				lic_scroll = 0;
				lic_scroll_by(0);
			} else {
				lic_scroll = lic_total;
				lic_scroll_by(0);
			}
			return 0;
		}

		if (wp == VK_ESCAPE)
			DestroyWindow(wnd);
		else if ((wp == VK_RETURN || wp == VK_RIGHT) && btn_enabled(B_NEXT)) {
			/* Same rule as the button; see WM_LBUTTONUP. */
			if (page == PG_DONE)
				finish_setup(wnd);
			else {
				page++;
				if (page == PG_INSTALL)
					start_work();
			}
		}
		else if (wp == VK_LEFT && btn_enabled(B_BACK))
			page--;
		else
			return 0;
		InvalidateRect(wnd, NULL, FALSE);
		return 0;

	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}

	return DefWindowProc(wnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
	WNDCLASSEX cls;
	HWND wnd;
	MSG msg;
	HDC screen;
	int w, h;
	RECT want;

	(void)prev; (void)cmd;

	/*
	 * DPI awareness, if the OS has any. XP does not, and GetProcAddress
	 * rather than a link-time import is what keeps this binary loadable
	 * there -- an unresolved user32 import would stop it starting at all,
	 * which is the failure mode this whole file exists to avoid.
	 */
	{
		HMODULE u32 = GetModuleHandle("user32.dll");
		BOOL (WINAPI *set_aware)(void) = NULL;

		if (u32)
			set_aware = (BOOL (WINAPI *)(void))
				GetProcAddress(u32, "SetProcessDPIAware");
		if (set_aware)
			set_aware();
	}

	{
		WSADATA wsa;

		/* Needed for the one conversation Setup has with the guest. */
		WSAStartup(MAKEWORD(2, 2), &wsa);
	}

	InitializeCriticalSection(&work_lock);
	default_paths();
	licence_load();
	revalidate();

	/*
	 * --finish is how the autostart re-enters: the pages have already been
	 * answered, so it opens on the progress page and gets on with it rather
	 * than asking a user who is watching their machine come up to click
	 * through a wizard they have already completed.
	 */
	if (cmd && StrStrIA(cmd, "--finish")) {
		resume_read();
		page = PG_INSTALL;
		auto_finish = 1;
	}

	screen = GetDC(NULL);
	scale_num = GetDeviceCaps(screen, LOGPIXELSX);
	ReleaseDC(NULL, screen);

	f_h1 = mk_font(16, FW_SEMIBOLD);
	f_body = mk_font(10, FW_NORMAL);
	f_small = mk_font(8, FW_NORMAL);
	f_stage = mk_font(9, FW_NORMAL);
	f_stage_on = mk_font(9, FW_SEMIBOLD);

	ZeroMemory(&cls, sizeof(cls));
	cls.cbSize = sizeof(cls);
	cls.lpfnWndProc = proc;
	cls.hInstance = inst;
	cls.hCursor = LoadCursor(NULL, IDC_ARROW);
	cls.hIcon = LoadIcon(NULL, IDI_APPLICATION);
	cls.lpszClassName = "MoCoLinuxSetup";
	cls.style = CS_HREDRAW | CS_VREDRAW;
	if (!RegisterClassEx(&cls))
		return 1;

	w = S(W_BASE);
	h = S(H_BASE);

	/*
	 * A fixed size, centred. Not resizable: every dimension here is chosen
	 * against the text, and a wizard that can be dragged to 200 pixels wide
	 * only offers the user a way to make it look broken.
	 */
	want.left = 0;
	want.top = 0;
	want.right = w;
	want.bottom = h;
	AdjustWindowRect(&want, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);

	wnd = CreateWindowEx(0, cls.lpszClassName, "MoCoLinux Setup",
			     WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
			     (GetSystemMetrics(SM_CXSCREEN) - (want.right - want.left)) / 2,
			     (GetSystemMetrics(SM_CYSCREEN) - (want.bottom - want.top)) / 2,
			     want.right - want.left, want.bottom - want.top,
			     NULL, NULL, inst, NULL);
	if (!wnd)
		return 1;

	main_wnd = wnd;
	ShowWindow(wnd, show ? show : SW_SHOW);
	UpdateWindow(wnd);

	if (auto_finish)
		start_work();
	UpdateWindow(wnd);

	while (GetMessage(&msg, NULL, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	return 0;
}
