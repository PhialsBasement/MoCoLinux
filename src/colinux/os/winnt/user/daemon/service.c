/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * Service support by Jaroslaw Kowalski <jaak@zd.com.pl>, 2004 (c)
 * Driver service separation by Daniel R. Slater <dan_slater@yahoo.com>, 2004 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#include <windows.h>
#include <winsvc.h>
#include <shlwapi.h>

#include <colinux/user/daemon.h>
#include <colinux/user/manager.h>
#include <colinux/os/user/misc.h>
#include <colinux/os/winnt/kernel/driver.h>
#include <colinux/os/winnt/user/misc.h>

#include "main.h"
#include "cmdline.h"
#include "misc.h"
#include "driver.h"
#include "service.h"
#include "res/service-message.h"

#define MAX_CMD_LINE_PATCHED 1024

/*
 *  ChangeServiceConfig2 is only supported on Windows 2000
 *  and later. Will try to dynamically load the function pointer
 *  so we still can run under Windows NT 4.0
 */

void co_winnt_set_service_restart_options(SC_HANDLE schService)
{
	BOOL (WINAPI *ChangeServiceConfig2Ptr)(SC_HANDLE h, DWORD dw, LPVOID lp) = NULL;
	HINSTANCE hLib = NULL;

	hLib = LoadLibrary("Advapi32.dll");

	if (hLib != NULL) {
		/*
		 * GetProcAddress returns FARPROC; the cast is required because GCC 14
		 * and later reject the implicit conversion rather than warning.
		 */
		ChangeServiceConfig2Ptr = (BOOL (WINAPI *)(SC_HANDLE, DWORD, LPVOID))
			GetProcAddress(hLib, "ChangeServiceConfig2A");
		FreeLibrary(hLib);
	}

	if (ChangeServiceConfig2Ptr) {
		SC_ACTION actions[3] = { {0} };
		SERVICE_FAILURE_ACTIONS actions_info = {0};

		co_terminal_print("daemon: setting restart options\n");
		actions[0].Type = SC_ACTION_RESTART;  /* restart the service */
		actions[0].Delay = 1000;  /* wait 1 second */
		actions[1].Type = SC_ACTION_RESTART;  /* restart the service */
		actions[1].Delay = 1000;  /* wait 1 second */
		actions[2].Type = SC_ACTION_NONE;  /* abandon our efforts */
		actions[2].Delay = 0;  /* wait 1 second */
		actions_info.dwResetPeriod = 90;  /* if we run for 90 seconds, then reset the failure count */
		actions_info.cActions = 3;
		actions_info.lpsaActions = actions;

		ChangeServiceConfig2Ptr(schService,SERVICE_CONFIG_FAILURE_ACTIONS,&actions_info);
	}
}

static char * co_winnt_get_path_from_exe(void)
{
	static char exe_name[512];

	if (!GetModuleFileName(0, exe_name, sizeof(exe_name))) {
		co_debug_error("daemon: cannot determine exe name.");
		return NULL;
	}

	if (!PathRemoveFileSpec(exe_name)) {
		co_debug_error("daemon: cannot get path from exe name.");
		return NULL;
	}

	return exe_name;
}

void co_winnt_change_directory_for_service(int argc, char **argv)
{
	char *p;
	static const char run_service []= { "--run-service" };

	while (*argv && argc--) {
		p = *argv++;
		if (0 == strncmp(p, run_service, sizeof(run_service)-1)) {
			p += sizeof(run_service)-1;

			/* With path? */
			if (*p == ':') {
				/* remove the ':', let p point to path */
				*p++ = '\0';
			} else {
				p = co_winnt_get_path_from_exe();
			}

			co_debug("cd '%s'", p);
			if (SetCurrentDirectory(p) == 0)
				co_terminal_print_last_error("daemon: Set working directory for service failed");

			return;
		}
	}
}

static void patch_command_line_for_service(char *destbuf, const char *srcbuf)
{
	char *destmax = destbuf + MAX_CMD_LINE_PATCHED;

	while (*srcbuf && destbuf < destmax)
	{
		if (0 == strncmp(srcbuf, "--install-service", 17))
		{
			char path [destmax-(destbuf+16)];
			bool_t spaces;

			/* replace any instance of --install-service with --run-service */
			srcbuf += 17;

			/* Add current working directory for service run later */
			GetCurrentDirectory(sizeof(path)-1, path);

			if (0 == strcasecmp(path, co_winnt_get_path_from_exe())) {
				/* CWD is the path of exe. No need extra path. */
				strcpy(destbuf, "--run-service");
			} else {
				/* Spaces in path must be enclosed */
				spaces = (strchr(path, ' ') != NULL);
				if (spaces)
					*destbuf++ = '\"';

				strcpy(destbuf, "--run-service:");
				strcat(destbuf, path);

				if (spaces)
					strcat(destbuf, "\"");
			}
			destbuf += strlen(destbuf);

			continue;
		}

		*destbuf++ = *srcbuf++;
	}
	*destbuf = 0;
}

co_rc_t co_winnt_daemon_install_as_service(const char *service_name, const char *original_commandline, int network_types)
{
	SC_HANDLE schService;
	SC_HANDLE schSCManager;
	char exe_name[512];
	char command[1024];
	char patched_commandline[MAX_CMD_LINE_PATCHED];

	/*
	 * The coLinux driver that the colinux service depends on - needs
	 * to be double-null terminated
	 */

	char dependency_names[80] = CO_DRIVER_NAME "\0";
	int len = strlen (CO_DRIVER_NAME)+1;

	co_terminal_print("daemon: installing service '%s'\n", service_name);
	if (!GetModuleFileName(0, exe_name, sizeof(exe_name))) {
		co_terminal_print("Cannot determin exe name. Install failed.\n");
		return CO_RC(ERROR);
	}

	schSCManager = OpenSCManager(0, 0, SC_MANAGER_ALL_ACCESS);
	if (schSCManager == 0) {
		co_terminal_print_last_error("daemon: cannot open service control manager for install");
		return CO_RC(ERROR);
	}

	patch_command_line_for_service(patched_commandline, original_commandline);

	co_snprintf(command, sizeof(command), "\"%s\" %s", exe_name, patched_commandline);
	co_terminal_print("daemon: service command line: %s\n", command);

	if (network_types & (1<<CO_NETDEV_TYPE_TAP)) {
		memcpy(dependency_names + len, "tap0801co\0", 10);
		len += 10;
	}

	if (network_types & (1<<CO_NETDEV_TYPE_BRIDGED_PCAP)) {
		memcpy(dependency_names + len, "NPF\0", 4);
	}

	schService = CreateService(schSCManager, service_name, service_name,
				   SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START,
				   SERVICE_ERROR_NORMAL, command, NULL, NULL, dependency_names,
				   NULL, NULL);

	if (schService != 0) {
	        co_winnt_set_service_restart_options(schService);
		co_terminal_print("daemon: service installed.\n");
		CloseServiceHandle(schService);
		CloseServiceHandle(schSCManager);
		return CO_RC(OK);
	}

	co_terminal_print_last_error("daemon: failed to install service");
	CloseServiceHandle(schSCManager);

	return CO_RC(ERROR);
}

co_rc_t co_winnt_daemon_remove_service(const char *service_name)
{
	SC_HANDLE schService;
	SC_HANDLE schSCManager;
	co_rc_t rc;

	co_terminal_print("daemon: removing service '%s'\n", service_name);

	schSCManager = OpenSCManager(0, 0, SC_MANAGER_ALL_ACCESS);
	if (schSCManager) {
		schService = OpenService(schSCManager, service_name, SERVICE_ALL_ACCESS);
		if (schService) {
			if (DeleteService(schService))	{
				co_terminal_print("daemon: service '%s' removed successfully.\n", service_name);
				rc = CO_RC(OK);
			} else {
				co_terminal_print_last_error("daemon: failed to remove service. DeleteService failed");
				rc = CO_RC(ERROR);
			}

			CloseServiceHandle(schService);
		} else {
			co_terminal_print_last_error("daemon: failed to remove service. OpenService failed");
			rc = CO_RC(ERROR);
		}

		CloseServiceHandle(schSCManager);
	} else {
		co_terminal_print_last_error("daemon: failed to remove service. Open service control manager failed");
		rc = CO_RC(ERROR);
	}

	return rc;
}

static co_start_parameters_t*	daemon_start_parameters;
static SERVICE_STATUS		ssStatus;
static SERVICE_STATUS_HANDLE   	sshStatusHandle;

void co_winnt_sc_report_status(DWORD dwCurrentState, DWORD dwWin32ExitCode, DWORD dwWaitHint)
{
	static DWORD dwCheckPoint = 1;

	if (dwCurrentState == SERVICE_START_PENDING)
		ssStatus.dwControlsAccepted = 0;
	else
		ssStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;

	ssStatus.dwCurrentState = dwCurrentState;
	ssStatus.dwWin32ExitCode = dwWin32ExitCode;
	ssStatus.dwWaitHint = dwWaitHint;

	if ((dwCurrentState == SERVICE_RUNNING) || (dwCurrentState == SERVICE_STOPPED))
		ssStatus.dwCheckPoint = 0;
	else
		ssStatus.dwCheckPoint = dwCheckPoint++;

	SetServiceStatus(sshStatusHandle, &ssStatus);
}

void WINAPI co_winnt_service_control_callback(DWORD dwCtrlCode)
{
	switch(dwCtrlCode)
	{
		case SERVICE_CONTROL_STOP:
		case SERVICE_CONTROL_SHUTDOWN:
			co_ntevent_print("daemon: service control %s.\n",
				(dwCtrlCode == SERVICE_CONTROL_STOP) ? "STOP" : "SHUTDOWN");
			co_winnt_sc_report_status(SERVICE_STOP_PENDING, NO_ERROR, 0);
			co_winnt_daemon_stop();
			return;

		case SERVICE_CONTROL_INTERROGATE:
			break;

		default:
			break;
	}

	co_winnt_sc_report_status(ssStatus.dwCurrentState, NO_ERROR, 0);
}

static void WINAPI service_main(int _argc, char** _argv)
{
	sshStatusHandle = RegisterServiceCtrlHandler("", co_winnt_service_control_callback);

	if (sshStatusHandle) {
		ssStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
		ssStatus.dwServiceSpecificExitCode = 0;

		co_winnt_sc_report_status(SERVICE_RUNNING, NO_ERROR, 3000);
		co_ntevent_print("daemon: service running.\n");
		co_winnt_daemon_main(daemon_start_parameters);
		co_ntevent_print("daemon: service stopped.\n");
		co_winnt_sc_report_status(SERVICE_STOPPED, 0, 0);
	}
}

bool_t co_winnt_daemon_initialize_service(co_start_parameters_t* start_parameters)
{
	SERVICE_TABLE_ENTRY dispatch_table[] = {
		{ "", (LPSERVICE_MAIN_FUNCTION)service_main },
		{ 0, 0 },
	};

	daemon_start_parameters = start_parameters;

	if (!StartServiceCtrlDispatcher(dispatch_table)) {
		co_terminal_print_last_error("service: Failed to initialize");
		return PFALSE;
	} else {
		return PTRUE;
	}
}

void co_ntevent_print(const char* format, ...)
{
	HANDLE	hEventLog;
	char	buf[0x100];
	va_list ap;

	va_start(ap, format);
	co_vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);

	co_terminal_print("%s", buf);

	if (co_running_as_service != PTRUE)
		return;

	hEventLog = RegisterEventSource(NULL, "coLinux");

	if (hEventLog == NULL) {
		co_terminal_print("Error registering event source.");
		return;
	}

	const char* szMsgs[] = { buf };

	if (ReportEvent(
		hEventLog,			// Event Log Handle
		EVENTLOG_INFORMATION_TYPE,	// Event type
		0,				// Event category
		MSG_SERVICE_INFO,		// Event ID
		NULL,				// User Security Identifier
		1,				// # of Strings
		0,				// Size of Data in Bytes
		szMsgs,				// Message Strings
		NULL) == 0) 			// Address of Data
		co_terminal_print_last_error("Error reporting to Event Log!");

	DeregisterEventSource(hEventLog);
}
