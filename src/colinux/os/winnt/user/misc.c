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
#include <windows.h>

#include <colinux/os/alloc.h>
#include <colinux/os/user/misc.h>

static co_terminal_print_hook_func_t terminal_print_hook;

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
