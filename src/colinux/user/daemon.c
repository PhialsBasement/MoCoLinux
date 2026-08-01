/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#include <colinux/common/common.h>
#include <colinux/common/version.h>
#include <colinux/common/libc.h>
#include <colinux/os/user/config.h>
#include <colinux/os/user/file.h>
#include <colinux/os/user/misc.h>
#include <colinux/os/user/exec.h>
#include <colinux/os/alloc.h>
#include <colinux/os/timer.h>
#include <colinux/user/macaddress.h>
#include <colinux/user/debug.h>

#include <memory.h>
#include <stdarg.h>
#include <stdlib.h>

#include "daemon.h"
#include "manager.h"
#include "monitor.h"
#include "config.h"
#include "reactor.h"

static
co_rc_t co_load_config_file(co_daemon_t* daemon)
{
	co_rc_t rc;

	if (daemon->start_parameters->cmdline_config) {
		daemon->config		  = daemon->start_parameters->config;
		daemon->config.magic_size = sizeof(co_config_t);
		return CO_RC(OK);
	}

	co_terminal_print_color(CO_TERM_COLOR_YELLOW,
				"XML configuration files are obsolete. Please"
				" pass command line\n");
	co_terminal_print_color(CO_TERM_COLOR_YELLOW,
				"options, or use '@' to pass a command-line"
				" compatible configuration\n");
	co_terminal_print_color(CO_TERM_COLOR_YELLOW, "file (e.g. @config.txt)\n\n");

	rc = CO_RC(ERROR);
	return rc;
}

void co_daemon_print_header(void)
{
	static int printed_already = 0;
	if (printed_already)
		return;

	co_terminal_print("Cooperative Linux Daemon, " COLINUX_VERSION "\n");
	co_terminal_print("Daemon compiled on %s\n", COLINUX_COMPILE_TIME);
	co_terminal_print("\n");
	printed_already = 1;
}

void co_daemon_syntax(void)
{
	co_daemon_print_header();
	co_terminal_print("syntax: \n");
	co_terminal_print("\n");
	co_terminal_print("    colinux-daemon [-d] [-h] [k] [-t name] [-v level] [configuration and boot parameter] @params.txt\n");
	co_terminal_print("\n");
	co_terminal_print("  -d             Don't launch and attach a coLinux console on startup\n");
	co_terminal_print("  -h             Show this help text\n");
	co_terminal_print("  -k             Suppress kernel messages\n");
	co_terminal_print("  -p pidfile     Write pid to file.\n");
	co_terminal_print("  -t name        When spawning a console, this is the type of console\n");
	co_terminal_print("                 (e.g, nt, fltk, etc...)\n");
	co_terminal_print("  -v level       Verbose messages, level 1 prints booting details, 2 or\n");
	co_terminal_print("                 more checks configs, 3 prints errors, default is 0 (off)\n");
	co_terminal_print("  @params.txt    Take more command line params from the given text file\n");
	co_terminal_print("                 (can be multi-line)\n");
	co_terminal_print("\n");
	co_terminal_print("Configuration and boot parameters:\n");
	co_terminal_print("\n");
	co_terminal_print("Params should start with kernel=vmlinux (is the kernel image file\n");
	co_terminal_print("the '@' option is not needed. Instead, you pass all configuration\n");
	co_terminal_print("via the command line, for example:\n");
	co_terminal_print("\n");
	co_terminal_print("  colinux-daemon kernel=vmlinux cobd0=root_fs hda1=:cobd0 root=/dev/cobd0 eth0=slirp\n");
	co_terminal_print(" \n");
	co_terminal_print("\n");
	co_terminal_print("Use of new aliases automatically allocates cobd(s), for example:\n");
	co_terminal_print("\n");
	co_terminal_print("  colinux-daemon mem=32 kernel=vmlinux hda1=root_fs root=/dev/hda1 \\\n");
	co_terminal_print("  eth0=pcap-bridge,\"Local Area Connection\"\n");
	co_terminal_print("\n");
	co_terminal_print("Unhandled parameters are forwarded to the kernel's boot parameters string.\n");
	co_terminal_print("See README.txt for more details about command-line usage.\n");
	co_terminal_print("See colinux-daemon's documentation for more options.\n");
	co_terminal_print("\n");
}

co_rc_t co_daemon_parse_args(co_command_line_params_t cmdline, co_start_parameters_t* start_parameters)
{
	co_rc_t      rc;
	bool_t 	     dont_launch_console = PFALSE;
	bool_t       verbose_specified   = PFALSE;
	unsigned int verbose_level       = 0;

	/*
	 * Default to whichever console was actually built. The FLTK console needs
	 * FLTK supplied by hand, and defaulting to a binary that is not there means
	 * a plain "colinux-daemon @foo.conf" dies at "error launching console".
	 * The build defines CO_DEFAULT_CONSOLE_FLTK when COLINUX_ENABLE_FLTK=yes,
	 * so this follows the build rather than needing to be flipped by hand.
	 */
	co_snprintf(start_parameters->console, sizeof(start_parameters->console),
		    CO_DEFAULT_CONSOLE);

	/* Parse arguments specific for command line only */

	rc = co_cmdline_params_one_arugment_parameter(cmdline, "-c",
						      &start_parameters->config_specified,
						      start_parameters->config_path,
						      sizeof(start_parameters->config_path));
	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_one_arugment_parameter(cmdline, "-p",
						      &start_parameters->pidfile_specified,
						      start_parameters->pidfile,
						      sizeof(start_parameters->pidfile));
	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_one_arugment_parameter(cmdline, "-t",
						      NULL,
						      start_parameters->console,
						      sizeof(start_parameters->console));
	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_argumentless_parameter(cmdline, "-d", &dont_launch_console);

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_argumentless_parameter(cmdline, "-k", &start_parameters->suppress_printk);

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_one_arugment_int_parameter(cmdline, "-v",
							  &verbose_specified, &verbose_level);

#ifdef COLINUX_DEBUG
	if (co_global_debug_levels.misc_level < verbose_level)
		co_global_debug_levels.misc_level = verbose_level;
#else
	co_terminal_print("daemon: \"-v\" ignored, COLINUX_DEBUG was not compiled in.\n");
#endif

	rc = co_cmdline_params_argumentless_parameter(cmdline, "-h", &start_parameters->show_help);

	if (!CO_OK(rc))
		return rc;

	start_parameters->launch_console = !dont_launch_console;

	/* Parse parameters, common for command line and config file */
	rc = co_parse_config_args(cmdline, start_parameters);

	if (!CO_OK(rc)) {
		co_terminal_print("daemon: error parsing configuration parameters and boot params\n");
		return rc;
	}

	return CO_RC(OK);
}

static void init_srand(void)
{
	co_timestamp_t t;

	co_os_get_timestamp(&t);

	srand(t.low ^ t.high);
}

co_rc_t co_daemon_create(co_start_parameters_t* start_parameters, co_daemon_t** co_daemon_out)
{
	co_rc_t	     rc;
	co_daemon_t* daemon;

	init_srand();

	daemon = co_os_malloc(sizeof(co_daemon_t));
	if (daemon == NULL) {
		rc = CO_RC(OUT_OF_MEMORY);
		goto out;
	}

	memset(daemon, 0, sizeof(*daemon));

	daemon->start_parameters = start_parameters;
	memcpy(daemon->config.config_path, start_parameters->config_path,
	       sizeof(start_parameters->config_path));

	rc = co_load_config_file(daemon);
	if (!CO_OK(rc)) {
		co_debug_error("error loading configuration");
		goto out_free;
	}

	*co_daemon_out = daemon;
	return rc;

out_free:
	co_os_free(daemon);

out:
	return rc;
}

void co_daemon_destroy(co_daemon_t* daemon)
{
	co_debug("daemon cleanup");
	co_os_free(daemon);
}

/* Remember: Strip tool scans only this file for needed symbols */
static
co_rc_t co_daemon_load_symbol_and_data(co_daemon_t*	daemon,
				       const char*	symbol_name,
				       unsigned long*	address_out,
				       void*		buffer,
				       unsigned long	buffer_size)
{
	co_rc_t		 rc = CO_RC_OK;
	co_elf_symbol_t* sym;
	void*		 data;

	sym = co_get_symbol_by_name(daemon->elf_data, symbol_name);
	if (sym)
		*address_out = co_elf_get_symbol_value(sym);
	else {
		co_debug_error("symbol %s not found", symbol_name);
		return CO_RC(ERROR);

	}

	data = co_elf_get_symbol_data(daemon->elf_data, sym);
	if (data == NULL) {
		co_debug_error("data of symbol %s not found", symbol_name);
		return CO_RC(ERROR);
	}

	memcpy(buffer, data, buffer_size);

	return rc;
}

/* Remember: Strip tool scans only this file for needed symbols */
static
co_rc_t co_daemon_load_symbol(co_daemon_t*	daemon,
			      const char*	symbol_name,
			      unsigned long*	address_out)
{
	co_rc_t		 rc = CO_RC_OK;
	co_elf_symbol_t* sym;

	sym = co_get_symbol_by_name(daemon->elf_data, symbol_name);
	if (sym)
		*address_out = co_elf_get_symbol_value(sym);
	else {
		co_debug_error("symbol %s not found", symbol_name);
		rc = CO_RC(ERROR);
	}

	return rc;
}

static
co_rc_t co_net_config_macs_read(int monitor_index, int device_index, unsigned char* binary)
{
	co_rc_t	rc;
	char	mac_string[32];

	rc = co_config_user_string_read(monitor_index,
					"eth",
					device_index,
					"mac",
					mac_string,
					sizeof(mac_string));
	if (!CO_OK(rc))
		return rc;

	co_debug_info("eth%d: MAC found in registry: %s", device_index, mac_string);
	return co_parse_mac_address(mac_string, binary);
}

static
void co_net_config_macs_write(int monitor_index, int device_index, const unsigned char* mac)
{
	co_rc_t rc;
	char	mac_string[32];

	co_debug_info("eth%d: MAC random generated, store into registry", device_index);
	co_build_mac_address(mac_string, sizeof(mac_string), mac);
	rc = co_config_user_string_write(monitor_index, "eth", device_index, "mac", mac_string);
	if (!CO_OK(rc))
		co_terminal_print("eth%d: Store random MAC into registry failed\n", device_index);
}

static
void co_daemon_prepare_net_macs(co_daemon_t* daemon, int monitor_index)
{
	co_rc_t rc;
	int	i;

	for (i=0; i < CO_MODULE_MAX_CONET; i++) {
		co_netdev_desc_t *net_dev;

		net_dev = &daemon->config.net_devs[i];
		if (net_dev->enabled == PFALSE)
			continue;

		if (net_dev->manual_mac_address != PFALSE)
			continue;

		rc = co_net_config_macs_read(monitor_index, i, net_dev->mac_address);
		if (!CO_OK(rc)) {
			unsigned long rand_mac = rand();
			int c;

			for (c=0; c < 10; c++)
				rand_mac *= rand() + 1234;

			net_dev->mac_address[0] = 0;
			net_dev->mac_address[1] = 0xFF;
			net_dev->mac_address[2] = rand_mac >> 030;
			net_dev->mac_address[3] = rand_mac >> 020;
			net_dev->mac_address[4] = rand_mac >> 010;
			net_dev->mac_address[5] = rand_mac >> 000;

			co_net_config_macs_write(monitor_index, i, net_dev->mac_address);
		}
	}
}

static
co_rc_t co_load_initrd(co_daemon_t* daemon)
{
	co_rc_t		rc;
	char*		initrd;
	unsigned long	initrd_size;

	if (!daemon->config.initrd_enabled)
		return CO_RC(OK);

	co_debug("reading initrd from (%s)", daemon->config.initrd_path);

	rc = co_os_file_load(daemon->config.initrd_path, &initrd, &initrd_size, 0);
	if (!CO_OK(rc)) {
		co_terminal_print("error loading initrd file\n");
		return rc;
	}

	co_debug("initrd size: %ld bytes", initrd_size);

	rc = co_user_monitor_load_initrd(daemon->monitor, initrd, initrd_size);

	co_os_free(initrd);

	return rc;
}

static void
memory_usage_limit_resached(co_manager_ioctl_create_t *create_params)
{
	co_manager_ioctl_info_t info = {0, };
	co_manager_handle_t	handle;
	co_rc_t			rc;

	co_terminal_print("colinux: memory size configuration for this VM: %ld MB\n",
			  create_params->actual_memsize_used / 0x100000);
	co_terminal_print("colinux: memory usage limit reached\n");
	co_terminal_print("colinux: try to decrease memory size configuration\n");

	handle = co_os_manager_open();
	if (!handle) {
		co_terminal_print("colinux: error opening manager\n");
		return;
	}

	rc = co_manager_info(handle, &info);
	if (!CO_OK(rc)) {
		co_terminal_print("colinux: erroneous manager info\n");
		co_os_manager_close(handle);
		return;
	}
	co_os_manager_close(handle);

	co_terminal_print("colinux: memory usage limit: %llu MB\n",
			  info.hostmem_usage_limit / 0x100000);
	co_terminal_print("colinux: current memory used by running VMs: %llu MB\n",
			  info.hostmem_used / 0x100000);
}

static
co_rc_t co_daemon_monitor_create(co_daemon_t* daemon)
{
	co_manager_ioctl_create_t 	create_params = {0, };
	co_symbols_import_t*		import;
	co_manager_handle_t		handle;
	co_manager_ioctl_status_t	status;
	co_rc_t				rc;

	import = &create_params.import;

	rc = co_daemon_load_symbol(daemon, "_kernel_start", &import->kernel_start);
	if (!CO_OK(rc))
		goto out;

	rc = co_daemon_load_symbol(daemon, "_end", &import->kernel_end);
	if (!CO_OK(rc))
		goto out;

	rc = co_daemon_load_symbol(daemon, "init_thread_union", &import->kernel_init_task_union);
	if (!CO_OK(rc)) {
		goto out;
	}

	rc = co_daemon_load_symbol(daemon, "co_start", &import->kernel_colinux_start);
	if (!CO_OK(rc))
		goto out;

	rc = co_daemon_load_symbol(daemon, "swapper_pg_dir", &import->kernel_swapper_pg_dir);
	if (!CO_OK(rc))
		goto out;

	rc = co_daemon_load_symbol(daemon, "idt_table", &import->kernel_idt_table);
	if (!CO_OK(rc))
		goto out;

	rc = co_daemon_load_symbol(daemon, "per_cpu__gdt_page", &import->kernel_gdt_table);
	if (!CO_OK(rc))
		goto out;

	rc = co_daemon_load_symbol_and_data(daemon,
					    "co_info",
					    &import->kernel_co_info,
					    &create_params.info,
					    sizeof(create_params.info));
	if (!CO_OK(rc))
		goto out;

	rc = co_daemon_load_symbol_and_data(daemon,
					    "co_arch_info",
					    &import->kernel_co_arch_info,
					    &create_params.arch_info,
					    sizeof(create_params.arch_info));
	if (!CO_OK(rc))
		goto out;

	if (create_params.info.api_version != CO_LINUX_API_VERSION) {
		co_terminal_print("colinux: error, expected kernel API version %d, got %ld\n",
				  CO_LINUX_API_VERSION, create_params.info.api_version);

		rc = CO_RC(VERSION_MISMATCHED);
		goto out;
	}

	if (create_params.info.compiler_abi != __GXX_ABI_VERSION) {
		co_terminal_print("colinux: error, expected gcc abi version %d, got %ld\n",
				  __GXX_ABI_VERSION, create_params.info.compiler_abi);
		co_terminal_print("colinux: Daemons gcc version %d.%d.x, "
				  "incompatible to kernel gcc version %ld.%ld.x\n",
				  __GNUC__, __GNUC_MINOR__,
				  create_params.info.compiler_major,
				  create_params.info.compiler_minor);

		rc = CO_RC(COMPILER_MISMATCHED);
		goto out;
	}

	handle = co_os_manager_open();
	if (!handle)
		return CO_RC(ERROR_ACCESSING_DRIVER);

	// Don't create monitor, if API_VERSION mismatch
	rc = co_manager_status(handle, &status);
	if (!CO_OK(rc))
		goto out_close;

	co_daemon_prepare_net_macs(daemon, status.monitors_count);

	create_params.config = daemon->config;

	rc = co_user_monitor_create(&daemon->monitor, &create_params, handle);
	if (!CO_OK(rc)) {
		if (CO_RC_GET_CODE(rc) == CO_RC_HOSTMEM_USE_LIMIT_REACHED)
			memory_usage_limit_resached(&create_params);
		goto out_close;
	}

	daemon->shared = (co_monitor_user_kernel_shared_t*)create_params.shared_user_address;
	if (!daemon->shared)
		return CO_RC(ERROR);

	daemon->shared->userspace_msgwait_count = 0;
	daemon->id = create_params.id;

	rc = co_load_initrd(daemon);

out:
	return rc;

out_close:
	co_os_manager_close(handle);
	return rc;
}

static
void co_daemon_monitor_destroy(co_daemon_t* daemon)
{
	co_user_monitor_close(daemon->monitor);
	daemon->monitor = NULL;
}

co_rc_t co_daemon_start_monitor(co_daemon_t* daemon)
{
	co_rc_t		rc;
	unsigned long	size;

	rc = co_os_file_load(daemon->config.vmlinux_path, &daemon->buf, &size, 0);
	if (!CO_OK(rc)) {
		co_terminal_print("error loading vmlinux file\n");
		goto out;
	}

	rc = co_elf_image_read(&daemon->elf_data, daemon->buf, size);
	if (!CO_OK(rc)) {
		co_terminal_print("%s: error reading image (%ld bytes)\n",
				  daemon->config.vmlinux_path, size);
		goto out_free_vmlinux;
	}

	co_debug("creating monitor");

	rc = co_daemon_monitor_create(daemon);
	if (!CO_OK(rc)) {
		co_debug_error("error initializing");
		goto out_free_vmlinux;
	}

	rc = co_elf_image_load(daemon);
	if (!CO_OK(rc)) {
		co_terminal_print("error loading image\n");
		goto out_destroy;
	}

	return rc;

out_destroy:
	co_daemon_monitor_destroy(daemon);

out_free_vmlinux:
	co_os_file_free(daemon->buf);

out:
	return rc;
}

void co_daemon_send_shutdown(co_daemon_t* daemon)
{
	struct {
		co_message_t		 message;
		co_linux_message_t	 linux_msg;
		co_linux_message_power_t data;
	} message;

	daemon->next_reboot_will_shutdown = PTRUE;
	co_terminal_print_color(CO_TERM_COLOR_YELLOW,
				"colinux: Linux VM goes shutdown, please wait!\n");

	message.message.from     = CO_MODULE_DAEMON;
	message.message.to       = CO_MODULE_LINUX;
	message.message.priority = CO_PRIORITY_IMPORTANT;
	message.message.type     = CO_MESSAGE_TYPE_OTHER;
	message.message.size     = sizeof(message.linux_msg) + sizeof(message.data);

	message.linux_msg.device = CO_DEVICE_POWER;
	message.linux_msg.unit   = 0;
	message.linux_msg.size   = sizeof(message.data);

	message.data.type	 = CO_LINUX_MESSAGE_POWER_SHUTDOWN;

	co_user_monitor_message_send(daemon->message_monitor, &message.message);
}

/* Handle linux kernel 'printk' */
static
co_rc_t co_daemon_handle_printk(co_daemon_t *daemon, co_message_t* message)
{
	if (message->type == CO_MESSAGE_TYPE_STRING) {
		char *string_start = (char *)message->data;

		if (string_start[0] == '<' && string_start[2] == '>' &&
		    ((string_start[1] >= '0' && string_start[1] <= '7') ||
		     string_start[1] == 'c' || string_start[1] == 'd'))
		{
			string_start += 3;
		}

		if (!daemon->start_parameters->suppress_printk) {
			co_terminal_print_color(CO_TERM_COLOR_WHITE,
						"%s", string_start);
		}
	}

	return CO_RC(OK);
}

static co_rc_t message_receive(co_reactor_user_t user,
			       unsigned char*    buffer,
			       unsigned long	 size)
{
	co_message_t*	message;
	unsigned long	message_size;
	long		size_left = size;
	long		position  = 0;
	co_daemon_t*	daemon    = (typeof(daemon))(user->private_data);

	while (size_left > 0) {
		message = (typeof(message))(&buffer[position]);
		message_size = message->size + sizeof(*message);
		size_left -= message_size;
		if (size_left >= 0) {
			switch(message->to) {
			case CO_MODULE_PRINTK:
				co_daemon_handle_printk(daemon, message);
			default:
				break;
			}
		}
		position += message_size;
	}

	return CO_RC(OK);
}

static
co_rc_t co_daemon_launch_net_daemons(co_daemon_t* daemon)
{
	int	i;
	co_rc_t rc;

	for (i=0; i < CO_MODULE_MAX_CONET; i++) {
		co_netdev_desc_t* net_dev;
		char		  interface_name[CO_NETDEV_DESC_STR_SIZE + 0x10] = {0, };

		net_dev = &daemon->config.net_devs[i];
		if (net_dev->enabled == PFALSE)
			continue;

		co_debug("launching daemon for conet%d", i);

		if (*net_dev->desc != 0)
			co_snprintf(interface_name,
				    sizeof(interface_name),
				    "-n \"%s\"",
				    net_dev->desc);

		switch (net_dev->type) {
		case CO_NETDEV_TYPE_BRIDGED_PCAP: {
			char mac_address[18];

			co_build_mac_address(mac_address, sizeof(mac_address), net_dev->mac_address);

			rc = co_launch_process(NULL,
					       "colinux-bridged-net-daemon -i %d -u %d %s -mac %s -p %d",
					       daemon->id,
					       i,
					       interface_name,
					       mac_address,
					       net_dev->promisc_mode);
			break;
		}

		case CO_NETDEV_TYPE_TAP: {
			rc = co_launch_process(NULL,
					       "colinux-net-daemon -i %d -u %d %s",
					       daemon->id,
					       i,
					       interface_name);
			break;
		}

		case CO_NETDEV_TYPE_SLIRP: {
			rc = co_launch_process(NULL,
					       "colinux-slirp-net-daemon -i %d -u %d%s%s",
					       daemon->id,
					       i,
					       (*net_dev->redir)?" -r ":"",
					       net_dev->redir);
			break;
		}

		case CO_NETDEV_TYPE_NDIS_BRIDGE: {
			char mac_address[18];

			co_build_mac_address(mac_address, sizeof(mac_address), net_dev->mac_address);

			rc = co_launch_process(NULL,
					       "colinux-ndis-net-daemon -i %d -u %d %s -mac %s -p %d",
					       daemon->id,
					       i,
					       interface_name,
					       mac_address,
					       net_dev->promisc_mode);
			break;
		}

		default:
			rc = CO_RC(ERROR);
		}

		if (!CO_OK(rc))
			co_terminal_print("WARNING: error launching network daemon!\n");
	}

	return CO_RC(OK);
}

static co_rc_t co_daemon_launch_serial_daemons(co_daemon_t *daemon)
{
	int i;
	co_rc_t rc;
	co_serialdev_desc_t *serial;

	for (i = 0, serial = daemon->config.serial_devs; i < CO_MODULE_MAX_SERIAL; i++, serial++) {
		char mode_param[CO_SERIAL_MODE_STR_SIZE + 10] = {0, };

		if (serial->enabled == PFALSE)
			continue;

		co_debug("launching daemon for ttys%d", i);

		if (serial->mode)
			co_snprintf(mode_param, sizeof(mode_param), " -m \"%s\"", serial->mode);

		rc = co_launch_process(NULL,
				       "colinux-serial-daemon -i %d -u %d -f %s%s",
			    	       daemon->id, i,
			    	       serial->desc,	/* -f COM1 */
			    	       mode_param);	/* -m 9600,n,8,2 */

		if (!CO_OK(rc))
			co_terminal_print("WARNING: error launching serial daemon!\n");
	}

	return CO_RC(OK);
}

static co_rc_t co_daemon_launch_executes(co_daemon_t *daemon)
{
	int i;
	co_rc_t rc;
	co_execute_desc_t *execute;

	for (i = 0, execute = daemon->config.executes; i < CO_MODULE_MAX_EXECUTE; i++, execute++) {

		if (execute->enabled == PFALSE)
			continue;

		co_debug("launching exec%d", i);

		rc = co_launch_process(&execute->pid,
				       (execute->args) ? "%s %s" : "%s",
				       execute->prog,
				       execute->args);

		if (!CO_OK(rc))
			co_terminal_print("WARNING: error launching exec%d '%s'!\n", i, execute->prog);
	}

	return CO_RC(OK);
}

static co_rc_t co_daemon_kill_executes(co_daemon_t* daemon)
{
	int i;
	co_rc_t rc;
	co_execute_desc_t *execute;

	for (i = 0, execute = daemon->config.executes; i < CO_MODULE_MAX_EXECUTE; i++, execute++) {

		if (execute->enabled == PFALSE || execute->pid == 0)
			continue;

		co_debug("killing exec%d", i);

		rc = co_kill_process(execute->pid);

		if (!CO_OK(rc))
			co_terminal_print("WARNING: error killing '%s'!\n", execute->prog);

		execute->pid = 0;
	}

	return CO_RC(OK);
}

static co_rc_t co_daemon_restart(co_daemon_t* daemon)
{
	co_rc_t rc = co_user_monitor_reset(daemon->monitor);

	if (!CO_OK(rc)) {
		co_terminal_print("colinux: reset unsuccessful\n");
		return rc;
	}

	rc = co_elf_image_load(daemon);
	if (!CO_OK(rc)) {
		co_terminal_print("colinux: error reloading vmlinux\n");
		return rc;
	}

	rc = co_load_initrd(daemon);
	if (!CO_OK(rc)) {
		co_terminal_print("colinux: error reloading initrd\n");
		return rc;
	}

	return rc;
}

co_rc_t co_daemon_run(co_daemon_t* daemon)
{
	co_rc_t			rc;
	co_reactor_t		reactor;
	co_module_t		modules[]	 = {CO_MODULE_PRINTK, };
	bool_t			restarting	 = PFALSE;
	co_start_parameters_t* 	start_parameters = daemon->start_parameters;

	rc = co_reactor_create(&reactor);
	if (!CO_OK(rc))
		return rc;

	co_terminal_print("PID: %d\n", (int)daemon->id);

	rc = co_user_monitor_open(reactor,
				  message_receive,
				  daemon->id,
				  modules,
				  sizeof(modules) / sizeof(co_module_t),
				  &daemon->message_monitor);

	if (!CO_OK(rc))
		goto out;

	daemon->message_monitor->reactor_user->private_data = (void*)daemon;

	if (start_parameters->pidfile_specified) {
		char buf[32];
		int size;

		size = co_snprintf(buf, sizeof(buf), "%d\n", (int)daemon->id);
		rc   = co_os_file_write(start_parameters->pidfile, buf, size);
		if (!CO_OK(rc)) {
			co_terminal_print("colinux: error creating PID file '%s'\n",
					  start_parameters->pidfile);
			return rc;
		}
	}

	if (start_parameters->launch_console) {
		co_debug_info("colinux: launching console");
		rc = co_launch_process(NULL,
				       "colinux-console-%s -a %d",
				       start_parameters->console,
				       daemon->id);
		if (!CO_OK(rc)) {
			co_terminal_print("error launching console\n");
			goto out;
		}
	}

	rc = co_daemon_launch_net_daemons(daemon);
	if (!CO_OK(rc)) {
		co_terminal_print("error launching network daemons\n");
		goto out;
	}

	rc = co_daemon_launch_serial_daemons(daemon);
	if (!CO_OK(rc))
		goto out;

	rc = co_daemon_launch_executes(daemon);
	if (!CO_OK(rc))
		goto out;

	co_terminal_print("colinux: booting\n");

	daemon->next_reboot_will_shutdown = PFALSE;
	do {
		restarting = PFALSE;

		rc = co_user_monitor_start(daemon->monitor);
		if (!CO_OK(rc))
			goto out;

		daemon->running = PTRUE;
		while (daemon->running) {
			co_monitor_ioctl_run_t params;
			rc = co_user_monitor_run(daemon->monitor, &params);
			if (!CO_OK(rc))
				break;
			co_reactor_select(reactor, 0);
		}

		if (CO_RC_GET_CODE(rc) == CO_RC_INSTANCE_TERMINATED) {
			co_monitor_ioctl_get_state_t params;
			co_termination_reason_t reason;

			co_terminal_print("colinux: Linux VM terminated\n");

			rc = co_user_monitor_get_state(daemon->monitor, &params);
			if (!CO_OK(rc)) {
				co_terminal_print("colinux: unable to get reason"
					          " for termination (bug?), aborting\n");
				break;
			}

			reason = params.termination_reason;

			switch (reason) {
			case CO_TERMINATE_REBOOT:
				if (daemon->next_reboot_will_shutdown) {
					co_terminal_print("colinux: shutting down after reboot.\n");
					break;
				}

				co_terminal_print("colinux: rebooted.\n");
				rc = co_daemon_restart(daemon);
				if (CO_OK(rc))
					restarting = PTRUE;

				break;

			case CO_TERMINATE_POWEROFF:
				co_terminal_print("colinux: powered off, exiting.\n");
				break;

			case CO_TERMINATE_PANIC:
				co_terminal_print("colinux: Kernel panic: %s\n", params.bug_info.text);

				if (co_strstr(params.bug_info.text, "VFS: Unable to mount root fs on")) {
					co_terminal_print_color(CO_TERM_COLOR_YELLOW,
						"colinux: kernel panic suggests that either you forget to supply a\n");
					co_terminal_print_color(CO_TERM_COLOR_YELLOW,
						"root= kernel boot paramter or the file / device mapped to the root\n");
					co_terminal_print_color(CO_TERM_COLOR_YELLOW,
						"file system is not found or inaccessible.\n");
					co_terminal_print_color(CO_TERM_COLOR_YELLOW,
						"Please Check your coLinux configuration and use option \"-v 3\".\n");
				}
				break;

			case CO_TERMINATE_HALT:
				co_terminal_print("colinux: halted, exiting.\n");
				break;

			case CO_TERMINATE_BUG:
				co_terminal_print("colinux: BUG at %s:%ld\n",
						  params.bug_info.text,
						  params.bug_info.line);
				break;

			case CO_TERMINATE_VMXE:
				co_terminal_print("colinux: An other virtualization runs in VMX mode.\n");
				break;

			default:
				co_terminal_print("colinux: terminated with code %d - abnormal exit, aborting\n",
						  reason);
				break;
			}
		}
	} while (restarting);

	co_daemon_kill_executes(daemon);

	co_user_monitor_close(daemon->message_monitor);

out:
	if (start_parameters->pidfile_specified) {
		co_rc_t rc1;

		rc1 = co_os_file_unlink(start_parameters->pidfile);
		if (!CO_OK(rc1))
			co_debug("error removing PID file '%s'",
				 start_parameters->pidfile);
	}

	co_reactor_destroy(reactor);

	return rc;
}

void co_daemon_end_monitor(co_daemon_t* daemon)
{
	co_debug("shutting down");

	co_daemon_monitor_destroy(daemon);
	co_os_file_free(daemon->buf);
}
