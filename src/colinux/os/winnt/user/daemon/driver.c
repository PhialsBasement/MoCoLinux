/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * Service support by Jaroslaw Kowalski <jaak@zd.com.pl>, 2004 (c)
 * Driver service separation by Daniel R. Slater <dan_slater@yahoo.com>, 2004 (c)
 * Driver installation fixes by George Boutwell <george.boutwell@gmail.com>, 2004 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#include <stdio.h>
#include <windows.h>
#include <shlwapi.h>

#include <colinux/common/common.h>
#include <colinux/common/ioctl.h>
#include <colinux/user/daemon.h>
#include <colinux/user/manager.h>
#include <colinux/user/debug.h>
#include <colinux/os/user/misc.h>
#include <colinux/os/winnt/kernel/driver.h>
#include <colinux/os/winnt/user/misc.h>
#include <colinux/os/current/os.h>

#include "main.h"
#include "cmdline.h"
#include "misc.h"
#include "driver.h"
#include "service.h"

static co_rc_t co_winnt_driver_remove_lowlevel(void);
static co_rc_t co_winnt_driver_install_lowlevel(void);

static co_rc_t co_win32_manager_is_installed(bool_t *installed)
{
	co_rc_t rc;

	rc = co_os_manager_is_installed(installed);
	if (!CO_OK(rc)) {
		if (CO_RC_GET_CODE(rc) == CO_RC_ACCESS_DENIED)
			co_terminal_print("access denied, not enough privileges\n");
		else
			co_terminal_print("error, unable to determine if driver is installed (rc %x)\n", (int)rc);
	}

	return rc;
}

/*
 * This function makes sure the driver is installed and initializes it.
 */
co_rc_t co_winnt_install_driver(void)
{
	co_rc_t rc;
	bool_t installed = PFALSE;
	co_manager_handle_t handle;

	rc = co_win32_manager_is_installed(&installed);
	if (!CO_OK(rc)) {
		return rc;
	}

	if (installed) {
		co_terminal_print("driver already installed\n");
		return CO_RC(OK);
	}

	rc = co_winnt_driver_install_lowlevel();
	if (!CO_OK(rc)) {
		co_terminal_print("cannot install (rc %x)\n", (int)rc);
		return CO_RC(ERROR);
	}

	handle = co_os_manager_open();
	if (handle == NULL) {
		co_terminal_print("error opening kernel driver\n");
		return CO_RC(ERROR_ACCESSING_DRIVER);
	}

	return CO_RC(OK);
}

co_rc_t co_winnt_remove_driver(void)
{
	co_rc_t rc;
	bool_t installed = PFALSE;
	co_manager_handle_t handle;
	co_manager_ioctl_status_t status = {0, };

	rc = co_win32_manager_is_installed(&installed);
	if (!CO_OK(rc))
		return rc;

	if (!installed) {
		co_terminal_print("driver not installed\n");
		return CO_RC(ERROR_ACCESSING_DRIVER);
	}

	handle = co_os_manager_open();
	if (!handle) {
		co_terminal_print("couldn't get driver handle, removing anyway\n");
		co_winnt_driver_remove_lowlevel();
		return CO_RC(ERROR_ACCESSING_DRIVER);
	}
	rc = co_manager_status(handle, &status);
	if (!CO_OK(rc)) {
		if (CO_RC_GET_CODE(rc) == CO_RC_VERSION_MISMATCHED) {
			co_terminal_print("driver version is %d while expected version %d\n",
					 status.periphery_api_version, CO_LINUX_PERIPHERY_API_VERSION);
		} else {
			co_terminal_print("detected an old buggy driver version\n");
		}
		co_terminal_print("Remove driver anyway\n");
		rc = CO_RC(ERROR_ACCESSING_DRIVER);
	} else
	if (status.monitors_count != 0) {
		co_terminal_print("monitors are running, cannot remove driver\n");
		co_os_manager_close(handle);
		return CO_RC(ERROR);
	}

	co_os_manager_close(handle);
	co_winnt_driver_remove_lowlevel();
	return rc;
}

co_rc_t co_winnt_status_driver(int verbose)
{
	co_rc_t rc;
	bool_t installed = PFALSE;
	co_manager_handle_t handle;
	co_manager_ioctl_status_t status = {0, };

	if (verbose) {
		co_terminal_print("checking if the driver is installed\n");
		strcpy(status.compile_time, "<unknown>");
	}

	rc = co_win32_manager_is_installed(&installed);
	if (!CO_OK(rc))
		return rc;

	if (!installed) {
		co_terminal_print("driver not installed\n");
		return CO_RC(ERROR_ACCESSING_DRIVER);
	}

	handle = co_os_manager_open();
	if (!handle) {
		co_terminal_print("couldn't get driver handle\n");
		return CO_RC(ERROR_MONITOR_NOT_LOADED);
	}

	rc = co_manager_status(handle, &status);
	if (!CO_OK(rc)) {
		if (verbose)
			co_terminal_print("couldn't get driver status (rc %x)\n", (int)rc);
		co_os_manager_close(handle);
		return rc;
	}

	if (verbose) {
		co_terminal_print("current state: %ld%s\n", status.state,
				  (status.state >= CO_MANAGER_STATE_INITIALIZED) ?
				  " (fully initialized)" : "");
		co_terminal_print("current number of monitors: %d\n", status.monitors_count);
		co_terminal_print("current linux api version: %d\n", status.linux_api_version);
		co_terminal_print("current periphery api version: %d\n", status.periphery_api_version);
		co_terminal_print("Driver compiled on: %s\n", status.compile_time);
	}
	co_os_manager_close(handle);

	return rc;
}

/*
 * Ask the driver what the host has mapped at a virtual address.
 *
 * An x86-64 porting aid. The passage page has to be reachable from both address
 * spaces; if the guest's window is also unused by the host, it can sit at the
 * same virtual address in both, which makes other_map zero and means the switch
 * never relocates its own instruction pointer. See doc/porting-x86_64 4.1.
 */
co_rc_t co_winnt_probe_va(const char* arg)
{
	co_rc_t rc;
	bool_t installed = PFALSE;
	co_manager_handle_t handle;
	co_manager_ioctl_probe_va_t probe = {0, };
	unsigned long long va = CO_VPTR_BASE;
	int level;
	static const char* level_name[CO_PROBE_VA_LEVELS] = {
		"PML4", "PDPT", "PD  ", "PT  "
	};

	if (arg && arg[0]) {
		if (sscanf(arg, "%llx", &va) != 1 &&
		    sscanf(arg, "0x%llx", &va) != 1) {
			co_terminal_print("probe: cannot parse address '%s'\n", arg);
			return CO_RC(INVALID_PARAMETER);
		}
	}

	rc = co_win32_manager_is_installed(&installed);
	if (!CO_OK(rc))
		return rc;

	if (!installed) {
		co_terminal_print("driver not installed\n");
		return CO_RC(ERROR_ACCESSING_DRIVER);
	}

	handle = co_os_manager_open();
	if (!handle) {
		co_terminal_print("couldn't get driver handle\n");
		return CO_RC(ERROR_MONITOR_NOT_LOADED);
	}

	probe.va = va;
	rc = co_manager_probe_va(handle, &probe);
	co_os_manager_close(handle);

	if (!CO_OK(rc)) {
		co_terminal_print("probe: ioctl failed (rc %x)\n", (int)rc);
		return rc;
	}

	if (!probe.supported) {
		co_terminal_print("probe: not implemented for this architecture\n");
		return CO_RC(OK);
	}

	if (!CO_OK(probe.rc)) {
		co_terminal_print("probe: driver reported failure (rc %x)\n", (int)probe.rc);
		return probe.rc;
	}

	co_terminal_print("probing host mapping of 0x%016llx\n", va);
	co_terminal_print("  host cr3: 0x%016llx\n", probe.cr3);

	for (level = 0; level < probe.levels_walked; level++)
		co_terminal_print("  %s entry: 0x%016llx%s\n",
				  level_name[level], probe.entry[level],
				  (probe.entry[level] & 1) ? "" : "   (not present)");

	if (probe.present)
		co_terminal_print("  RESULT: mapped%s -- the host is using this range\n",
				  probe.large_page ? " (large page)" : "");
	else
		co_terminal_print("  RESULT: not mapped at the %s level -- free as far as the host\n"
				  "          is concerned, so the same-address trick is available\n",
				  level_name[probe.levels_walked - 1]);

	return CO_RC(OK);
}

static co_rc_t co_winnt_install_driver_lowlevel(IN SC_HANDLE SchSCManager, IN LPCTSTR  DriverName, IN LPCTSTR ServiceExe)
{
	SC_HANDLE  schService;

	/*
	 * SERVICE_DEMAND_START, not SERVICE_AUTO_START.
	 *
	 * An auto-start kernel driver loads on every boot, so a driver that
	 * bugchecks turns the machine into a boot loop: it dies during service
	 * startup, every time, before a desktop exists to uninstall it from.
	 * Recovering means Last Known Good, Safe Mode or a rescue disk.
	 *
	 * Nothing needs auto-start. co_winnt_load_driver_lowlevel_by_name() starts
	 * the service explicitly right after creating it, and the daemon loads the
	 * driver on demand, so demand-start behaves identically in use while
	 * leaving a failed driver harmless across a reboot.
	 */
	schService = CreateService(SchSCManager,
				   DriverName,
				   DriverName,
				   SERVICE_ALL_ACCESS,
				   SERVICE_KERNEL_DRIVER,
				   SERVICE_DEMAND_START,
				   SERVICE_ERROR_NORMAL,
				   ServiceExe,
				   NULL,
				   NULL,
				   NULL,
				   NULL,
				   NULL);

	if (schService == NULL) {
		co_terminal_print_last_error("Install lowlevel driver failed");
		return CO_RC(ERROR_INSTALLING_DRIVER);
	}

	/* Possible error: ERROR_SERVICE_EXISTS */

	CloseServiceHandle(schService);

	return CO_RC(OK);
}

static co_rc_t co_winnt_remove_driver_lowlevel(IN SC_HANDLE  SchSCManager, IN LPCTSTR DriverName)
{
	SC_HANDLE  schService;
	co_rc_t   rc;

	schService = OpenService (SchSCManager,
				  DriverName,
				  SERVICE_ALL_ACCESS);

	if (schService == NULL)
		return CO_RC(ERROR);

	if (DeleteService(schService))
		rc = CO_RC(OK);
	else
		rc = CO_RC(ERROR_REMOVING_DRIVER);

	CloseServiceHandle(schService);

	return rc;
}

static co_rc_t co_winnt_start_driver_lowlevel(IN SC_HANDLE SchSCManager, IN LPCTSTR DriverName)
{
	SC_HANDLE  schService;
	co_rc_t   ret;
	DWORD      err;

	schService = OpenService(SchSCManager, DriverName, SERVICE_ALL_ACCESS);

	if (schService == NULL) {
		return CO_RC(ERROR_ACCESSING_DRIVER);
	}

	if (StartService(schService, 0, NULL)) {
		ret = CO_RC(OK);
	} else {
		ret = CO_RC(ERROR_STARTING_DRIVER);

		err = GetLastError();
#if 1
		if (err == ERROR_SERVICE_ALREADY_RUNNING)
			co_terminal_print("failure: StartService, ERROR_SERVICE_ALREADY_RUNNING\n");
		else
			co_terminal_print("failure: StartService (0x%lx)\n", err);
#endif
	}

	CloseServiceHandle(schService);

	return ret;
}

static co_rc_t co_winnt_stop_driver_lowlevel(IN SC_HANDLE  SchSCManager, IN LPCTSTR DriverName)
{
	SC_HANDLE       schService;
	SERVICE_STATUS  serviceStatus;
	co_rc_t        rc;

	schService = OpenService(SchSCManager, DriverName, SERVICE_ALL_ACCESS);

	if (schService == NULL)
		return CO_RC(ERROR_ACCESSING_DRIVER);

	if (ControlService(schService, SERVICE_CONTROL_STOP, &serviceStatus))
		rc = CO_RC(OK);
	else {
		rc = CO_RC(ERROR_STOPPING_DRIVER);
	}

	CloseServiceHandle(schService);

	return rc;
}

static co_rc_t co_winnt_unload_driver_lowlevel_by_name(char *name)
{
	SC_HANDLE   schSCManager;
	co_rc_t rc;

	co_debug_end();

	schSCManager = OpenSCManager (NULL,                 // machine (NULL == local)
				      NULL,                 // database (NULL == default)
				      SC_MANAGER_ALL_ACCESS // access required
				     );
	if (schSCManager == NULL)
		return CO_RC(ERROR_ACCESSING_DRIVER);

	co_debug("stopping driver service");
	rc = co_winnt_stop_driver_lowlevel(schSCManager, name);

	co_debug("removing driver service");
	rc = co_winnt_remove_driver_lowlevel(schSCManager, name);

	CloseServiceHandle(schSCManager);

	/*
	 * Apparently this givens the service manager an opportunity to
	 * remove the service before we reinstall it.
	 */
	Sleep(100);

	return rc;
}

static co_rc_t co_winnt_load_driver_lowlevel_by_name(char *name, char *path)
{
	SC_HANDLE   schSCManager;
	char fullpath[0x100] = {0,};
	char driverfullpath[0x100] = {0,};
	co_rc_t rc;

	GetModuleFileName(NULL, fullpath, sizeof(fullpath));
	PathRemoveFileSpec(fullpath);
	PathCombine(driverfullpath, fullpath, path);

	co_terminal_print("loading %s\n", driverfullpath);

	schSCManager = OpenSCManager(NULL,                 // machine (NULL == local)
				     NULL,                 // database (NULL == default)
				     SC_MANAGER_ALL_ACCESS // access required
				     );
	if (schSCManager == NULL)
		return CO_RC(ERROR_ACCESSING_DRIVER);

	rc = co_winnt_install_driver_lowlevel(schSCManager, name, driverfullpath);
	if (!CO_OK(rc)) {
		CloseServiceHandle(schSCManager);
		return rc;
	}

	rc = co_winnt_start_driver_lowlevel(schSCManager, name);
	if (!CO_OK(rc)) {
		co_winnt_remove_driver_lowlevel(schSCManager, name);
		CloseServiceHandle(schSCManager);
		return rc;
	}

#if (0)
	rc = co_os_check_device(name);
	if (!CO_OK(rc)) {
		co_winnt_stop_driver_lowlevel(schSCManager, name);
		co_winnt_remove_driver_lowlevel(schSCManager, name);
		CloseServiceHandle(schSCManager);
		return rc;
	}
#endif

	CloseServiceHandle(schSCManager);

	return CO_RC(OK);
}

static co_rc_t co_winnt_driver_remove_lowlevel(void)
{
	return co_winnt_unload_driver_lowlevel_by_name(CO_DRIVER_NAME);
}

static co_rc_t co_winnt_driver_install_lowlevel(void)
{
	return co_winnt_load_driver_lowlevel_by_name(CO_DRIVER_NAME, COLINUX_DRIVER_FILE);
}
