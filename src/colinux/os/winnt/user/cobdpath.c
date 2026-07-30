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
/*
 * The Rtl* string and path helpers below live in ntdll and are declared by
 * winternl.h. The kernel-mode DDK headers this used to include cannot be
 * combined with windows.h under mingw-w64: both define UNICODE_STRING,
 * EXCEPTION_RECORD and much else.
 */
#include <winternl.h>
#include <unistd.h>

#include <colinux/common/libc.h>
#include <colinux/common/common.h>
#include <colinux/user/cmdline.h>
#include <colinux/os/user/misc.h>
#include <colinux/os/user/cobdpath.h>

co_rc_t co_canonize_cobd_path(co_pathname_t *pathname)
{
	co_remove_quotation_marks(*pathname);

	if (!(co_strncmp(*pathname, "\\Device\\", 8) == 0  ||
	      co_strncmp(*pathname, "\\DosDevices\\", 12) == 0))
	{
		co_pathname_t copied_path;
		UNICODE_STRING dos_uni;
		UNICODE_STRING nt_uni;
		ANSI_STRING dos_ansi;
		ANSI_STRING nt_ansi;
		NTSTATUS status;
		int len;

		/* Convert relative path to absolute path with drive */
		if (!_fullpath(copied_path, *pathname, sizeof(copied_path))) {
			co_terminal_print("%s: Error in path, or to long\n", *pathname);
			return CO_RC(ERROR);
		}

		len = co_strlen(copied_path);

		/* Only root dir "C:\" should end with backslash */
		if (len > 3 && copied_path[len-1] == '\\') {
		    /* Remove tailing backslash */
		    copied_path[len--] = '\0';
		}

		dos_ansi.Buffer = copied_path;
		dos_ansi.Length = len;
		dos_ansi.MaximumLength = dos_ansi.Length + 1;

		status = RtlAnsiStringToUnicodeString(&dos_uni, &dos_ansi, TRUE);
		if (!NT_SUCCESS(status))
			return CO_RC(ERROR);

		RtlDosPathNameToNtPathName_U(dos_uni.Buffer, &nt_uni, NULL, NULL);
		RtlFreeUnicodeString(&dos_uni);

		status = RtlUnicodeStringToAnsiString(&nt_ansi, &nt_uni, TRUE);
		if (!NT_SUCCESS(status)) {
			RtlFreeUnicodeString(&nt_uni);
			return CO_RC(ERROR);
		}

		co_snprintf(*pathname, sizeof(*pathname), "%s", nt_ansi.Buffer);

		RtlFreeUnicodeString(&nt_uni);
		RtlFreeAnsiString(&nt_ansi);
	}

	return CO_RC(OK);
}

co_rc_t co_dirname (char *path)
{
	char *p = path + strlen(path);

	while (--p > path) {
		if (*p == '/' || *p == '\\')
			break;
		*p = 0;
	}

	if (p == path) {
		*p++ = '.';
		*p = '\0';
	}

	return CO_RC(OK);
}

