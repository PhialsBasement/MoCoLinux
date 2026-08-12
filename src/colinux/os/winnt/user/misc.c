/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 * Service support by Jaroslaw Kowalski <jaak@zd.com.pl>, 2004 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <windows.h>
#include <tlhelp32.h>

#include <colinux/os/alloc.h>
#include <colinux/os/user/misc.h>

static co_terminal_print_hook_func_t terminal_print_hook;

/*
 * Everything this process says, appended to a file beside its own executable,
 * as it is said.
 *
 * The terminal has never been where this output is actually read: manual runs
 * capture it through the transfer agent, the installer redirects it to a log,
 * and a daemon launched detached prints into nothing at all. Two failures of
 * that arrangement cost tonight dearly. The CRT block-buffers stdout when it
 * is a file, so a wedged daemon's log read as zero bytes while the narration
 * sat in a buffer that only a clean exit would flush -- the report existed and
 * was unreadable at precisely the moment it was wanted. And a report that is
 * only produced post mortem is lost entirely when the process never dies.
 *
 * So every line lands here immediately: opened, appended, closed per call, so
 * a kill at any instant loses nothing already said. Timestamp and pid per
 * call, because two daemons from the same directory (a boot daemon and a
 * console server) share this file.
 */
static void co_terminal_file_tee(const char *text)
{
	static char path[512];
	static int  path_state;	/* 0 not yet resolved, 1 usable, -1 not */
	char head[48];
	SYSTEMTIME t;
	HANDLE h;
	DWORD n;

	if (path_state == 0) {
		DWORD len = GetModuleFileName(NULL, path, sizeof(path) - 40);
		SYSTEMTIME s;

		if (len == 0 || len >= sizeof(path) - 40) {
			path_state = -1;
			return;
		}
		/*
		 * One file per run, named for the moment the run started.
		 *
		 * A single shared log cannot answer the question that matters
		 * when a run takes the machine down: did this run say nothing,
		 * or is what I am reading the previous run's? Appending to one
		 * file makes those two indistinguishable, and that is exactly
		 * the confusion that cost several resets here -- a tail showing
		 * a healthy boot report that turned out to be from ten minutes
		 * earlier, while the run under test had written nothing at all.
		 *
		 * With the start time in the name, a run that dies before
		 * printing leaves an empty file with its own timestamp, which
		 * says "it started and got nowhere" rather than nothing.
		 */
		GetLocalTime(&s);
		snprintf(path + len, sizeof(path) - len,
			 ".%04u%02u%02u-%02u%02u%02u.log",
			 s.wYear, s.wMonth, s.wDay,
			 s.wHour, s.wMinute, s.wSecond);
		path_state = 1;
	}
	if (path_state < 0)
		return;

	/*
	 * FILE_FLAG_WRITE_THROUGH, and it is the whole point of this log.
	 *
	 * Opening, appending and closing per line is not enough on its own:
	 * the write lands in the file system cache and stays there. That is
	 * survivable for a process that is killed -- the cache outlives it --
	 * and useless for the failure this log exists to explain, where the
	 * machine resets. A triple fault takes the cache with it, so a run
	 * that printed twenty lines and then reset leaves a file with none of
	 * them, which reads exactly like a run that never started.
	 *
	 * That is not hypothetical: it is why several resets here produced an
	 * empty log and were misread as "the daemon never ran".
	 */
	h = CreateFile(path, FILE_APPEND_DATA,
		       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
		       OPEN_ALWAYS,
		       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return;

	GetLocalTime(&t);
	snprintf(head, sizeof(head), "[%02u:%02u:%02u.%03u %5u] ",
		 t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
		 (unsigned)GetCurrentProcessId());

	WriteFile(h, head, strlen(head), &n, NULL);
	WriteFile(h, text, strlen(text), &n, NULL);
	/* Belt as well as braces: push it past the disk's own write cache. */
	FlushFileBuffers(h);
	CloseHandle(h);
}

static void co_terminal_printv(const char *format, va_list ap)
{
	/*
	 * 4 KB, not 256 bytes: this prints kernel log records and the boot
	 * console's accumulated text, and a report that silently truncates is
	 * a report that lies by omission.
	 */
	char buf[0x1000];
	int len;

	vsnprintf(buf, sizeof(buf), format, ap);

	printf("%s", buf);
	/*
	 * Immediately, not at exit. Redirected stdout is block-buffered, and
	 * the readers of this stream are log files being tailed while the
	 * process is still running -- or still wedged.
	 */
	fflush(stdout);

	co_terminal_file_tee(buf);

	if (terminal_print_hook != NULL)
		terminal_print_hook(buf);

	len = strlen(buf);
	while (len > 0  &&  buf[len-1] == '\n')
		buf[len - 1] = '\0';

	co_debug_lvl(prints, 11, "prints \"%s\"\n", buf);
}

void co_terminal_print(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	co_terminal_printv(format, ap);
	va_end(ap);
}

void co_terminal_print_color(co_terminal_color_t color, const char *format, ...)
{
	HANDLE output;
	WORD wOldAttributes = 0;
	va_list ap;

	output = GetStdHandle(STD_OUTPUT_HANDLE);
	if (output != INVALID_HANDLE_VALUE) {
		WORD wAttributes;
		BOOL ret;
		CONSOLE_SCREEN_BUFFER_INFO ConsoleScreenBufferInfo;

		ret = GetConsoleScreenBufferInfo(output, &ConsoleScreenBufferInfo);
		if (!ret)
			return;

		wOldAttributes = ConsoleScreenBufferInfo.wAttributes;

		switch (color) {
		case CO_TERM_COLOR_YELLOW:
			wAttributes = FOREGROUND_RED | FOREGROUND_GREEN |  FOREGROUND_INTENSITY;
			break;
		case CO_TERM_COLOR_WHITE:
			wAttributes = FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_GREEN |  FOREGROUND_INTENSITY;
			break;
		default:
			wAttributes = wOldAttributes;
			break;
		}

		SetConsoleTextAttribute(output, wAttributes);
	}

	va_start(ap, format);
	co_terminal_printv(format, ap);
	va_end(ap);

	if (output != INVALID_HANDLE_VALUE)
		SetConsoleTextAttribute(output, wOldAttributes);
}

void co_set_terminal_print_hook(co_terminal_print_hook_func_t func)
{
	terminal_print_hook = func;
}

bool_t co_winnt_get_last_error(char *error_message, int buf_size)
{
	DWORD dwLastError = GetLastError();

	if (!FormatMessage(
		    FORMAT_MESSAGE_FROM_SYSTEM |
		    FORMAT_MESSAGE_ARGUMENT_ARRAY,
		    NULL,
		    dwLastError,
		    LANG_NEUTRAL,
		    error_message,
		    buf_size,
		    NULL))
	{
		co_snprintf(error_message, buf_size, "GetLastError() = 0x%lx\n", dwLastError);
	}

	return dwLastError != 0;
}

void co_terminal_print_last_error(const char *message)
{
	char last_error[0x200];

	if (co_winnt_get_last_error(last_error, sizeof(last_error))) {
		co_terminal_print("%s: %s", message, last_error);
	} else {
		co_terminal_print("%s: success\n", message);
	}
}

bool_t co_os_claim_single_instance(const char* name)
{
	static HANDLE held;
	char full[0x100];

	/*
	 * Global\ so the name is machine-wide rather than per session. These
	 * daemons get started from a console, from a batch file, and from the
	 * transfer agent's service, and two of those are not the same session.
	 */
	co_snprintf(full, sizeof(full), "Global\\mocolinux-%s", name);

	held = CreateMutex(NULL, TRUE, full);
	if (held == NULL)
		return PTRUE;	/* cannot tell; do not refuse to run */

	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		CloseHandle(held);
		held = NULL;
		return PFALSE;
	}

	/*
	 * Deliberately never released. The handle is closed by the kernel when
	 * this process ends, however it ends -- including a taskkill or a
	 * bugcheck -- which is the whole reason for using a mutex rather than
	 * anything this code would have to clean up itself.
	 */
	return PTRUE;
}

struct co_os_thread_ctx {
	co_os_thread_func_t func;
	void*		    arg;
};

static DWORD WINAPI co_os_thread_trampoline(LPVOID p)
{
	struct co_os_thread_ctx ctx = *(struct co_os_thread_ctx*)p;

	co_os_free(p);
	ctx.func(ctx.arg);
	return 0;
}

void* co_os_thread_start(co_os_thread_func_t func, void* arg)
{
	struct co_os_thread_ctx* ctx;
	HANDLE h;

	ctx = co_os_malloc(sizeof(*ctx));
	if (!ctx)
		return NULL;
	ctx->func = func;
	ctx->arg  = arg;

	h = CreateThread(NULL, 0, co_os_thread_trampoline, ctx, 0, NULL);
	if (!h)
		co_os_free(ctx);

	return (void*)h;
}

void co_os_thread_join(void* thread)
{
	HANDLE h = (HANDLE)thread;

	if (!h)
		return;

	WaitForSingleObject(h, INFINITE);
	CloseHandle(h);
}

/*
 * Host processors, for the vCPU budget. GetSystemInfo rather than the
 * GetLogicalProcessorInformation family: this daemon still runs on XP x64,
 * where the newer calls are absent, and a processor group wider than 64 is
 * not a machine this port targets.
 */
unsigned long co_os_active_cpu_count(void)
{
	SYSTEM_INFO si;

	GetSystemInfo(&si);
	return si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
}

unsigned int co_os_terminate_process_by_name(const char* image_name)
{
	HANDLE snapshot;
	PROCESSENTRY32 entry;
	unsigned int killed = 0;

	snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE)
		return 0;

	memset(&entry, 0, sizeof(entry));
	entry.dwSize = sizeof(entry);
	if (Process32First(snapshot, &entry)) {
		do {
			HANDLE process;

			if (_stricmp(entry.szExeFile, image_name) != 0)
				continue;
			if (entry.th32ProcessID == GetCurrentProcessId())
				continue;
			process = OpenProcess(PROCESS_TERMINATE, FALSE,
					      entry.th32ProcessID);
			if (!process)
				continue;
			if (TerminateProcess(process, 1))
				killed++;
			CloseHandle(process);
		} while (Process32Next(snapshot, &entry));
	}
	CloseHandle(snapshot);
	return killed;
}

/*
 * Guest RAM as a named shared section. See the contract in os/user/misc.h.
 *
 * Global\ so a service-session daemon and a console-session daemon find the
 * same object; both run elevated, which carries SeCreateGlobalPrivilege. The
 * section is pagefile-backed and committed up front -- the commit charge is
 * the honest statement that this memory exists -- and both handles are
 * deliberately leaked so the object's lifetime is exactly the union of the
 * processes using it.
 */
#define CO_OS_GUEST_RAM_SECTION "Global\\MoCoLinuxGuestRAM"

void* co_os_guest_ram_section_create(unsigned long long bytes,
				     unsigned long long* actual_out)
{
	HANDLE section;
	void* view;
	MEMORY_BASIC_INFORMATION mbi;

	if (actual_out)
		*actual_out = 0;

	section = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL,
				     PAGE_READWRITE | SEC_COMMIT,
				     (DWORD)(bytes >> 32),
				     (DWORD)(bytes & 0xffffffffu),
				     CO_OS_GUEST_RAM_SECTION);
	if (section == NULL)
		return NULL;

	view = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, 0);
	if (view == NULL) {
		CloseHandle(section);
		return NULL;
	}

	/*
	 * The size the OBJECT actually has, not the size that was asked for.
	 *
	 * CreateFileMapping on an existing name returns the existing object
	 * "with its current size, not the specified size" (MSDN) -- and a
	 * stale GPU daemon from a crashed run can be holding last boot's
	 * object. If --mem grew between the runs, reporting the requested
	 * size would send the driver probing past the view's real end, and
	 * MmProbeAndLockPages answers that with a raise, not an error. The
	 * driver is told what VirtualQuery can prove instead.
	 */
	memset(&mbi, 0, sizeof(mbi));
	if (VirtualQuery(view, &mbi, sizeof(mbi)) == sizeof(mbi) && actual_out)
		*actual_out = (unsigned long long)mbi.RegionSize;
	return view;
}

void* co_os_guest_ram_section_open(unsigned long long* bytes_out)
{
	HANDLE section;
	void* view;
	MEMORY_BASIC_INFORMATION mbi;

	if (bytes_out)
		*bytes_out = 0;

	section = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE,
				   CO_OS_GUEST_RAM_SECTION);
	if (section == NULL)
		return NULL;

	view = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, 0);
	if (view == NULL) {
		CloseHandle(section);
		return NULL;
	}

	memset(&mbi, 0, sizeof(mbi));
	if (VirtualQuery(view, &mbi, sizeof(mbi)) == sizeof(mbi) && bytes_out)
		*bytes_out = (unsigned long long)mbi.RegionSize;
	return view;
}
