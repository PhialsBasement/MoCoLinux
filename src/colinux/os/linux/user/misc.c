/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#include <stdio.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <colinux/common/libc.h>
#include <colinux/os/alloc.h>
#include <colinux/os/timer.h>
#include <colinux/os/user/misc.h>


static char const co_term_color_yellow[]=
	{ "\033[33;1;40m" };	/* brown foreground, bold, black background */
static char const co_term_color_white[]=
	{ "\033[37;1;40m" };	/* white foreground, bold, black background */
static char const co_term_color_normal[]=
	{ "\033[0m" };		/* reset all attributes to their defaults */
static char const * co_term_color_current = co_term_color_normal;


static void co_terminal_printv(const char *format, va_list ap)
{
	char buf[0x100];
	int len;

	co_vsnprintf(buf, sizeof(buf), format, ap);

	printf("%s", buf);

	len = co_strlen(buf);
	while (len > 0  &&  buf[len-1] == '\n')
		buf[len - 1] = '\0';

	co_debug_lvl(prints, 11, "prints \"%s\"\n", buf);
}

void co_terminal_print(const char *format, ...)
{
	va_list ap;

	if (co_term_color_current != co_term_color_normal) {

		co_term_color_current = co_term_color_normal;
		printf (co_term_color_current);
	}

	va_start(ap, format);
	co_terminal_printv(format, ap);
	va_end(ap);
}

void co_terminal_print_color(co_terminal_color_t color, const char *format, ...)
{
	va_list ap;
	char const * str;

	/* use simple VT100 sequences, see: man console_codes */
	switch (color) {
	case CO_TERM_COLOR_YELLOW:
		str = co_term_color_yellow;
		break;
	case CO_TERM_COLOR_WHITE:
		str = co_term_color_white;
		break;
	default:
		str = co_term_color_current;
	}

	if (co_term_color_current != str) {
		/* change to new color */
		co_term_color_current = str;
		printf (co_term_color_current);
	}

	va_start(ap, format);
	co_terminal_printv(format, ap);
	va_end(ap);
}

void co_os_get_timestamp(co_timestamp_t *dts)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);

	dts->high = tv.tv_sec;
        dts->low = tv.tv_usec;
}

int co_udp_socket_connect(const char *addr, unsigned short int port)
{
	struct sockaddr_in server;
	int ret, sock;

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock == -1)
		return -1;

	co_bzero((char *)&server, sizeof(server));
	server.sin_family = AF_INET;
	server.sin_addr.s_addr = inet_addr(addr);
	server.sin_port = htons(port);

	ret = connect(sock, (struct sockaddr *)&server, sizeof(server));
	if (ret)
		return -1;

	return sock;
}

int co_udp_socket_send(int sock, const char *buffer, unsigned long size)
{
	return send(sock, buffer, size, 0);
}

void co_udp_socket_close(int sock)
{
	close(sock);
}

bool_t co_os_claim_single_instance(const char* name)
{
	/*
	 * Not implemented on the Linux host, and it says so rather than
	 * pretending to guard. The daemons that need this -- the slirp bridge,
	 * the console and the debug reader -- are the Windows ones, where a
	 * second copy competes for a guest and holds a driver handle open. If
	 * the Linux host ever grows the same problem, this wants an flock on a
	 * file under /run rather than a silent PTRUE.
	 */
	(void)name;
	return PTRUE;
}
