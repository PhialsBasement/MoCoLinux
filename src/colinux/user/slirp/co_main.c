
/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2004 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#include <colinux/user/debug.h>
#include <colinux/user/reactor.h>
#include <colinux/user/monitor.h>
#include <colinux/user/cmdline.h>
#include <colinux/user/slirp/libslirp.h>
#include <colinux/user/slirp/ctl.h>
#include <colinux/user/slirp/co_main.h>
#include <colinux/os/user/misc.h>

/*******************************************************************************
 * Type Declarations
 */

#define PARM_SLIRP_REDIR_TCP 0
#define PARM_SLIRP_REDIR_UDP 1

typedef struct start_parameters {
	bool_t show_help;
	bool_t selftest;
	unsigned int index;
	co_id_t instance;
} start_parameters_t;

/*******************************************************************************
 * Globals
 */
static start_parameters_t g_daemon_parameters;
static co_reactor_t g_reactor;
static co_user_monitor_t *g_monitor_handle;

/* from slirp.c */
extern struct in_addr client_addr;
extern struct in_addr alias_addr;

static co_rc_t monitor_receive(co_reactor_user_t user, unsigned char *buffer, unsigned long size)
{
	co_message_t *message;
	unsigned long message_size;
	long size_left = size;
	long position = 0;

	while (size_left > 0) {
		message = (typeof(message))(&buffer[position]);
		message_size = message->size + sizeof(*message);
		size_left -= message_size;
		if (size_left >= 0) {
			co_slirp_mutex_lock();
			slirp_input(message->data, message->size);
			co_slirp_mutex_unlock();
		}
		position += message_size;
	}

	return CO_RC(OK);
}

/*
 * Self-test mode.
 *
 * This slirp has never executed on Win64. It is 2004 code whose protocol
 * structures are overlays on wire format, so a compiler that lays a bitfield
 * out differently, or a SIZEOF_CHAR_P that disagrees with the pointer width,
 * does not produce an error -- it produces packets that parse as nonsense,
 * silently, in a NAT bridged to a guest kernel. slirp.c now asserts the six
 * structure sizes at compile time, but a size being right is not the same as
 * the code working.
 *
 * So it runs here first: in its own process, fed frames from memory, replies
 * checked byte by byte, before any of it goes near the driver or a guest. A
 * crash kills a test program and nothing else.
 *
 * ARP and DHCP are the two exchanges worth having and neither needs a network.
 * ARP is answered inside slirp.c without touching the IP path, which makes it
 * the narrowest "is slirp alive" question there is. DHCP is the interesting
 * one: a DISCOVER traverses ip_input, udp_input and bootp_input, so it
 * exercises struct ip, struct udphdr and struct udpiphdr in both directions --
 * the layouts that were wrong, and what pacman needs first, since a guest with
 * no lease resolves no names.
 */
static bool_t	     selftest_active;
static unsigned char selftest_frame[16][1600];
static int	     selftest_len[16];
static int	     selftest_count;

int slirp_can_output(void)
{
	return 1;
}

void slirp_output(const uint8_t *pkt, int pkt_len)
{
	if (selftest_active) {
		if (selftest_count < 16 && pkt_len > 0 && pkt_len <= 1600) {
			memcpy(selftest_frame[selftest_count], pkt, pkt_len);
			selftest_len[selftest_count] = pkt_len;
			selftest_count++;
		}
		return;
	}

	/* Received packet from Slirp */
	struct {
		co_message_t message;
		co_linux_message_t message_linux;
		char data[pkt_len];
	} message;

	message.message.from = CO_MODULE_CONET0 + g_daemon_parameters.index;
	message.message.to = CO_MODULE_LINUX;
	message.message.priority = CO_PRIORITY_DISCARDABLE;
	message.message.type = CO_MESSAGE_TYPE_OTHER;
	message.message.size = sizeof(message) - sizeof(message.message);
	message.message_linux.device = CO_DEVICE_NETWORK;
	message.message_linux.unit = g_daemon_parameters.index;
	message.message_linux.size = pkt_len;
	memcpy(message.data, pkt, pkt_len);

	g_monitor_handle->reactor_user->send(g_monitor_handle->reactor_user,
					     (unsigned char *)&message, sizeof(message));
}

/*
 * A DHCP DISCOVER, built from byte offsets rather than from slirp's own
 * structs. Those structs are the thing under test; using them to build the
 * input would hide exactly the layout errors this is looking for. If these
 * offsets and slirp's idea of a header ever disagree, that disagreement is the
 * finding.
 */
static int selftest_build_discover(unsigned char *f, const unsigned char *mac)
{
	int udp_len = 8 + 244;
	int ip_len  = 20 + udp_len;
	unsigned long sum = 0;
	int i;

	memset(f, 0, 14 + ip_len);

	memset(f + 0, 0xff, 6);
	memcpy(f + 6, mac, 6);
	f[12] = 0x08; f[13] = 0x00;

	f[14] = 0x45;
	f[16] = (unsigned char)(ip_len >> 8);
	f[17] = (unsigned char)(ip_len & 0xff);
	f[22] = 64;
	f[23] = 17;
	memset(f + 30, 0xff, 4);

	for (i = 0; i < 20; i += 2)
		sum += ((unsigned long)f[14 + i] << 8) | f[15 + i];
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	sum = ~sum & 0xffff;
	f[24] = (unsigned char)(sum >> 8);
	f[25] = (unsigned char)(sum & 0xff);

	f[34] = 0x00; f[35] = 68;
	f[36] = 0x00; f[37] = 67;
	f[38] = (unsigned char)(udp_len >> 8);
	f[39] = (unsigned char)(udp_len & 0xff);

	f[42] = 1;
	f[43] = 1;
	f[44] = 6;
	f[46] = 0x12; f[47] = 0x34; f[48] = 0x56; f[49] = 0x78;
	memcpy(f + 70, mac, 6);
	f[278] = 0x63; f[279] = 0x82;
	f[280] = 0x53; f[281] = 0x63;
	f[282] = 53; f[283] = 1; f[284] = 1;
	f[285] = 0xff;

	return 14 + ip_len;
}

static co_rc_t co_slirp_selftest(void)
{
	static const unsigned char mac[6] = { 0x02, 0xc0, 0x11, 0x00, 0x00, 0x01 };
	unsigned char frame[1600];
	int len, i, failures = 0;
	void *probe;

	selftest_active = PTRUE;

	/*
	 * Addresses first, because everything else rests on them. slirp keeps
	 * pointers in the u_int32_t link fields inside its wire structures, so
	 * this process has to be confined to 32 bits of address space -- which
	 * is why the daemon is linked at 0x400000 with its
	 * large-address-aware flag cleared. If either half of that did not
	 * happen, it fails here, in a report, instead of as corruption in the
	 * middle of a download.
	 */
	probe = malloc(4096);
	co_terminal_print("addresses (all must fit in 32 bits):\n");
	co_terminal_print("  a static (client_addr)   %p\n", (void *)&client_addr);
	co_terminal_print("  a heap block             %p\n", probe);
	co_terminal_print("  code (this function)     %p\n", (void *)co_slirp_selftest);

	if ((unsigned long long)(size_t)&client_addr >> 32 ||
	    (unsigned long long)(size_t)probe >> 32 ||
	    (unsigned long long)(size_t)co_slirp_selftest >> 32) {
		co_terminal_print("  FAIL: an address is above 4 GB. slirp's 32-bit queue\n");
		co_terminal_print("        links cannot hold it -- check the daemon was\n");
		co_terminal_print("        linked --image-base 0x400000 and had\n");
		co_terminal_print("        IMAGE_FILE_LARGE_ADDRESS_AWARE cleared.\n");
		failures++;
	} else {
		co_terminal_print("  ok\n");
	}
	free(probe);

	slirp_init();
	co_terminal_print("slirp_init() returned\n");

	/* ARP: answered in slirp.c, no IP path involved. */
	selftest_count = 0;
	memset(frame, 0, sizeof(frame));
	memset(frame + 0, 0xff, 6);
	memcpy(frame + 6, mac, 6);
	frame[12] = 0x08; frame[13] = 0x06;
	frame[14] = 0x00; frame[15] = 0x01;
	frame[16] = 0x08; frame[17] = 0x00;
	frame[18] = 6;    frame[19] = 4;
	frame[20] = 0x00; frame[21] = 0x01;
	memcpy(frame + 22, mac, 6);
	memcpy(frame + 28, &client_addr, 4);
	memcpy(frame + 38, &alias_addr, 4);
	slirp_input(frame, 42);

	if (selftest_count == 1 && selftest_len[0] == 42 &&
	    selftest_frame[0][20] == 0 && selftest_frame[0][21] == 2 &&
	    memcmp(selftest_frame[0] + 28, &alias_addr, 4) == 0) {
		co_terminal_print("ARP:  reply from %02x:%02x:%02x:%02x:%02x:%02x  ok\n",
				  selftest_frame[0][22], selftest_frame[0][23],
				  selftest_frame[0][24], selftest_frame[0][25],
				  selftest_frame[0][26], selftest_frame[0][27]);
	} else {
		co_terminal_print("ARP:  FAIL -- %d frames out", selftest_count);
		if (selftest_count)
			co_terminal_print(", first %d bytes, opcode %d",
					  selftest_len[0],
					  (selftest_frame[0][20] << 8) | selftest_frame[0][21]);
		co_terminal_print("\n");
		failures++;
	}

	/*
	 * DHCP: through ip_input, udp_input and bootp_input, the path that
	 * reads struct ip and struct udpiphdr. A wrong layout here is the bug
	 * that presents as "the guest never gets a lease".
	 */
	selftest_count = 0;
	len = selftest_build_discover(frame, mac);
	slirp_input(frame, len);

	if (selftest_count >= 1 && selftest_len[0] >= 286 &&
	    selftest_frame[0][42] == 2 &&
	    selftest_frame[0][282] == 53 && selftest_frame[0][284] == 2) {
		const unsigned char *y = selftest_frame[0] + 58;

		co_terminal_print("DHCP: OFFER of %d.%d.%d.%d, %d bytes  ok\n",
				  y[0], y[1], y[2], y[3], selftest_len[0]);
	} else {
		co_terminal_print("DHCP: FAIL -- %d frames out", selftest_count);
		if (selftest_count)
			co_terminal_print(", first %d bytes, op %d, dhcp type %d",
					  selftest_len[0], selftest_frame[0][42],
					  selftest_frame[0][284]);
		co_terminal_print("\n");
		failures++;
	}

	/* On failure, the bytes themselves, because a verdict is not evidence. */
	for (i = 0; failures && i < selftest_count; i++) {
		int j;

		co_terminal_print("  frame %d (%d bytes):", i, selftest_len[i]);
		for (j = 0; j < selftest_len[i] && j < 64; j++)
			co_terminal_print("%s%02x", (j % 16) ? " " : "\n    ",
					  selftest_frame[i][j]);
		co_terminal_print("\n");
	}

	co_terminal_print("slirp selftest: %s\n",
			  failures ? "FAILED" : "all checks passed");

	selftest_active = PFALSE;
	return failures ? CO_RC(ERROR) : CO_RC(OK);
}

static co_rc_t wait_loop(void)
{
	int ret, nfds;
	fd_set rfds, wfds, xfds;
	struct timeval tv;
	co_rc_t rc;

	while (1) {
		/* Slirp main loop as copied from QEMU. */
		rc = co_reactor_select(g_reactor, 1);
		if (!CO_OK(rc))
			break;

		nfds = -1;
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		FD_ZERO(&xfds);

		slirp_select_fill(&nfds, &rfds, &wfds, &xfds);
		tv.tv_sec = 0;
		tv.tv_usec = 1000;
		ret = select(nfds + 1, &rfds, &wfds, &xfds, &tv);
		if (ret >= 0) {
			slirp_select_poll(&rfds, &wfds, &xfds);
		}
	}

	return rc;
}

/********************************************************************************
 * parameters
 */

static void syntax(void)
{
	co_terminal_print("Cooperative Linux Slirp Virtual Network Daemon\n");
	co_terminal_print("Dan Aloni, 2004 (c)\n");
	co_terminal_print("\n");
	co_terminal_print("syntax: \n");
	co_terminal_print("\n");
	co_terminal_print("  colinux-slirp-net-daemon -i pid -u unit [-h]\n");
	co_terminal_print("\n");
	co_terminal_print("    -h                      Show this help text\n");
	co_terminal_print("    -t                      Self-test: feed slirp canned ARP and DHCP\n");
	co_terminal_print("                            frames and check its replies. Needs no guest,\n");
	co_terminal_print("                            no monitor and no network.\n");
	co_terminal_print("    -i pid                  coLinux instance ID to connect to\n");
	co_terminal_print("    -u unit                 Network device index number (0 for eth0, 1 for\n");
	co_terminal_print("                            eth1, etc.)\n");
	co_terminal_print("    -r tcp|udp:hport:cport[:count]  port redirection.\n");
}

static co_rc_t
parse_redir_param (char *p)
{
	int iProto, iHostPort, iClientPort, iPortCount, i;

	do {
		// minimal len is "tcp:x:x"
		if (strlen (p) < 7)
			return CO_RC(ERROR);

		if (strncasecmp(p, "tcp", 3) == 0)
			iProto = PARM_SLIRP_REDIR_TCP;
		else if (strncasecmp(p, "udp", 3) == 0)
			iProto = PARM_SLIRP_REDIR_UDP;
		else
			return CO_RC(ERROR);

		// check first ':'
		p += 3;
		if (*p != ':')
			return CO_RC(ERROR);
		iHostPort = strtol(p+1, &p, 10);

		// check second ':'
		if (*p != ':')
			return CO_RC(ERROR);
		iClientPort = strtol(p+1, &p, 10);

		// check optional third ':'
		iPortCount = 1;
		if (*p == ':')
			iPortCount = strtol(p+1, &p, 10);

		if (iPortCount <= 0)
			return CO_RC(ERROR);

		for (i = 0; i < iPortCount; i++) {
			co_debug("slirp redir %d %d:%d", iProto, iHostPort+i, iClientPort+i);
			if (slirp_redir(iProto, iHostPort+i, client_addr, iClientPort+i) < 0) {
				co_terminal_print("conet-slirp-daemon: slirp redir %d:%d failed.\n",
						  iHostPort+i, iClientPort+i);
			}
		}

		// Next redirection?
	} while (*p++ == '/');

	return CO_RC(OK);
}

static co_rc_t
co_slirp_parse_args(co_command_line_params_t cmdline, start_parameters_t *parameters)
{
	co_rc_t rc;
	char redir_buff [0x100];
	bool_t instance_specified;
	bool_t unit_specified;
	bool_t redir_specified;

	/* Parse command line */
	rc = co_cmdline_params_one_arugment_int_parameter(cmdline, "-i",
							  &instance_specified, &parameters->instance);
	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_one_arugment_int_parameter(cmdline, "-u",
							  &unit_specified, &parameters->index);
	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_one_arugment_parameter(cmdline, "-r", &redir_specified,
						      redir_buff, sizeof(redir_buff));
	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_argumentless_parameter(cmdline, "-h", &parameters->show_help);
	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_argumentless_parameter(cmdline, "-t", &parameters->selftest);
	if (!CO_OK(rc))
		return rc;

	/*
	 * The self-test takes no instance and no unit, so it has to be
	 * answered before the checks that insist on both.
	 */
	if (parameters->selftest)
		return CO_RC(OK);

	if (parameters->show_help)
		return CO_RC(OK);

	rc = co_cmdline_params_check_for_no_unparsed_parameters(cmdline, PTRUE);
	if (!CO_OK(rc)) {
		syntax();
		return rc;
	}

	if (!unit_specified) {
		co_terminal_print("conet-slirp-daemon: device index not specified\n");
		return CO_RC(ERROR);
	}

	if ((parameters->index < 0) ||
	    (parameters->index >= CO_MODULE_MAX_CONET))
	{
		co_terminal_print("conet-slirp-daemon: invalid index: %d\n", parameters->index);
		return CO_RC(ERROR);
	}

	if (!instance_specified) {
		co_terminal_print("conet-slirp-daemon: coLinux instance not specificed\n");
		return CO_RC(ERROR);
	}

	if (redir_specified) {
		rc = parse_redir_param(redir_buff);
		if (!CO_OK(rc)) {
			co_terminal_print("conet-slirp-daemon: Error in redirection '%s'\n", redir_buff);
			return rc;
		}
	}

	return CO_RC(OK);
}

co_rc_t co_slirp_main(int argc, char *argv[])
{
	co_command_line_params_t cmdline;
	co_rc_t rc;
	co_module_t module;

	co_debug_start();
	co_process_high_priority_set();
	slirp_init();

	rc = co_cmdline_params_alloc(argv+1, argc-1, &cmdline);
	if (!CO_OK(rc))
		goto out;

	rc = co_slirp_parse_args(cmdline, &g_daemon_parameters);
	if (!CO_OK(rc))
		goto out_params;

	if (g_daemon_parameters.show_help) {
		syntax();
		goto out;
	}

	/*
	 * Before the mutex, the reactor or the monitor: the self-test needs
	 * none of them and has to run on a box with no guest at all.
	 */
	if (g_daemon_parameters.selftest) {
		rc = co_slirp_selftest();
		goto out_params;
	}

	co_debug("conet-slirp-daemon: create mutex");
	rc = co_slirp_mutex_init();
	if (!CO_OK(rc))
		goto out_params;

	co_debug("conet-slirp-daemon: create reactor");
	rc = co_reactor_create(&g_reactor);
	if (!CO_OK(rc))
		goto out_mutex;

	co_debug("conet-slirp-daemon: connecting to monitor");

	module = CO_MODULE_CONET0 + g_daemon_parameters.index;
	rc = co_user_monitor_open(g_reactor, monitor_receive,
				  g_daemon_parameters.instance,
				  &module, 1,
				  &g_monitor_handle);
	if (!CO_OK(rc)) {
		co_terminal_print("conet-slirp-daemon: monitor open failed\n");
		goto out_close;
	}

	co_terminal_print("conet-slirp-daemon: running\n");

	wait_loop();

out_close:
	co_reactor_destroy(g_reactor);

out_mutex:
	co_slirp_mutex_destroy();

out_params:
	co_cmdline_params_free(cmdline);

out:
	if (!CO_OK(rc))
		co_terminal_print("conet-slirp-daemon: exitcode %x\n", (int)rc);

	co_debug_end();
	return rc;
}
