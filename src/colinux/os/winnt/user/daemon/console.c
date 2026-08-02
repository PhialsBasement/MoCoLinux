/*
 * A terminal server for the guest, over TCP.
 *
 * The guest's console is two rings in its own memory and the driver knows how
 * to reach them (kernel/console.c). What was missing was something for a human
 * to connect to, and it cannot live in the process that boots the guest: that
 * one is inside a single ioctl for as long as the guest runs, which is the
 * whole point of a cooperative monitor and is also why it cannot service a
 * socket meanwhile.
 *
 * So this is a second process against the same driver. The console ioctl takes
 * no lock and touches only its own side of each ring, so the two coexist
 * without arrangement.
 *
 * TCP rather than the console this process was started from, because the test
 * box is headless and reached over the network: the daemon is launched through
 * a transfer agent whose stdin is not a terminal. A socket is something you can
 * point netcat or telnet at from the machine you are actually sitting at.
 */

#include <windows.h>
#include <winsock2.h>

#include <colinux/common/common.h>
#include <colinux/common/libc.h>
#include <colinux/user/manager.h>
#include <colinux/os/user/manager.h>
#include <colinux/os/user/misc.h>

#include "console.h"

/*
 * Telnet clients open in line mode and negotiate; a raw socket from netcat
 * does not. Ask for character-at-a-time and no echo so that a telnet client
 * behaves like the raw one -- three-byte IAC sequences that netcat harmlessly
 * hands to the shell as control characters it ignores.
 */
static void co_console_telnet_setup(int sock)
{
	static const unsigned char negotiate[] = {
		255, 251, 3,	/* IAC WILL SUPPRESS-GO-AHEAD	*/
		255, 251, 1,	/* IAC WILL ECHO		*/
		255, 254, 1,	/* IAC DONT ECHO		*/
	};

	send(sock, (const char*)negotiate, sizeof(negotiate), 0);
}

/*
 * Strip telnet's in-band commands so they do not reach the shell.
 *
 * Only enough to survive a default telnet client: an IAC introduces a two or
 * three byte command, and a doubled IAC is a literal 255. Anything else passes
 * through untouched, which is what a raw client needs.
 */
static unsigned long co_console_strip_iac(char* buf, unsigned long len)
{
	unsigned long i = 0, o = 0;

	while (i < len) {
		unsigned char c = (unsigned char)buf[i];

		if (c != 255) {
			buf[o++] = buf[i++];
			continue;
		}

		if (i + 1 >= len)
			break;			/* split command: drop the tail */

		if ((unsigned char)buf[i + 1] == 255) {
			buf[o++] = (char)255;
			i += 2;
			continue;
		}

		/* WILL/WONT/DO/DONT carry an option byte, others do not */
		switch ((unsigned char)buf[i + 1]) {
		case 251: case 252: case 253: case 254:
			i += 3;
			break;
		default:
			i += 2;
			break;
		}
	}

	return o;
}

/*
 * Has a guest been here and then left?
 *
 * The probe is a console pump with no input and no output buffer.
 * co_console_pump guards both directions with `if (in_size)` and
 * `if (out_size)`, so with both zero it takes the lock, checks that the guest's
 * ring address is still published, and returns -- touching neither ring. That
 * matters: draining output to find out whether a guest exists would throw away
 * whatever it had printed for the next client to read.
 *
 * "Not yet" and "finished" are the same return code and are not the same
 * situation. Waiting indefinitely is correct before the first guest, because
 * this is meant to be startable before the boot. After one has been served,
 * the same answer means the run is over and this process is now only in the
 * way, so it leaves.
 */
static bool_t co_console_guest_gone(co_manager_handle_t handle)
{
	static bool_t	     seen_guest = PFALSE;
	static unsigned long gone_polls = 0;
	unsigned long taken = 0, produced = 0;
	co_rc_t rc;

	rc = co_manager_console(handle, NULL, 0, &taken, NULL, 0, &produced);

	if (CO_OK(rc)) {
		seen_guest = PTRUE;
		gone_polls = 0;
		return PFALSE;
	}

	if (!seen_guest)
		return PFALSE;

	/* Two seconds at the 200 ms accept timeout, same bound as the bridge. */
	if (++gone_polls < 10)
		return PFALSE;

	co_terminal_print("console: the guest is gone -- exiting so the driver can\n"
			  "  be unloaded and the binaries replaced\n");
	return PTRUE;
}

/*
 * Serve one client at a time, forever.
 *
 * A poll loop rather than blocking reads, because both directions have to make
 * progress and neither has an event to wait on: the guest produces output
 * whenever it runs, which is on another thread entirely. Ten milliseconds is
 * below the threshold where typing feels laggy and far above the cost of the
 * two ioctls it takes.
 */
co_rc_t co_winnt_console_server(unsigned short port)
{
	co_manager_handle_t handle;
	WSADATA wsa;
	SOCKET listener, client;
	struct sockaddr_in addr;
	int one = 1;

	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		co_terminal_print("console: winsock unavailable\n");
		return CO_RC(ERROR);
	}

	handle = co_os_manager_open();
	if (!handle) {
		co_terminal_print("console: cannot open the driver -- is it loaded,\n");
		co_terminal_print("console: and has a guest been booted?\n");
		return CO_RC(ERROR);
	}

	listener = socket(AF_INET, SOCK_STREAM, 0);
	if (listener == INVALID_SOCKET) {
		co_terminal_print("console: cannot create a socket\n");
		co_os_manager_close(handle);
		return CO_RC(ERROR);
	}

	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));

	co_memset(&addr, 0, sizeof(addr));
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port        = htons(port);

	if (bind(listener, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
	    listen(listener, 1) != 0) {
		co_terminal_print("console: cannot listen on port %d\n", (int)port);
		closesocket(listener);
		co_os_manager_close(handle);
		return CO_RC(ERROR);
	}

	co_terminal_print("console: listening on port %d\n", (int)port);
	co_terminal_print("console: connect with  nc <this box> %d\n", (int)port);

	for (;;) {
		char netbuf[512];
		char outbuf[2048];
		unsigned long pending = 0;
		bool_t leaving = PFALSE;

		/*
		 * Wait for a client with a deadline rather than in accept(),
		 * so that a console with nobody attached still notices the
		 * guest going away. Blocking in accept() forever is why one of
		 * these was still sitting on the box holding a driver handle
		 * hours after its guest had finished -- which locks
		 * colinux-daemon.exe against being overwritten, keeps `sc stop`
		 * in STOP_PENDING, and cannot be cleared by taskkill because
		 * this process has an open handle to the driver.
		 */
		{
			fd_set afds;
			struct timeval atv;

			FD_ZERO(&afds);
			FD_SET(listener, &afds);
			atv.tv_sec  = 0;
			atv.tv_usec = 200000;

			if (select(0, &afds, NULL, NULL, &atv) <= 0) {
				if (co_console_guest_gone(handle))
					break;
				continue;
			}
		}

		client = accept(listener, NULL, NULL);
		if (client == INVALID_SOCKET)
			continue;

		co_terminal_print("console: client connected\n");
		co_console_telnet_setup(client);
		{
			u_long nonblocking = 1;
			ioctlsocket(client, FIONBIO, &nonblocking);
		}

		for (;;) {
			unsigned long taken = 0, produced = 0;
			int got;
			co_rc_t rc;

			/*
			 * Read from the socket only when the previous
			 * keystrokes have all been handed over, so nothing is
			 * dropped when the guest's input ring is full.
			 */
			if (pending == 0) {
				got = recv(client, netbuf, sizeof(netbuf), 0);
				if (got == 0)
					break;			/* closed */
				if (got > 0)
					pending = co_console_strip_iac(netbuf, got);
				else if (WSAGetLastError() != WSAEWOULDBLOCK)
					break;
			}

			rc = co_manager_console(handle,
						netbuf, pending, &taken,
						outbuf, sizeof(outbuf), &produced);
			if (!CO_OK(rc)) {
				/*
				 * No guest yet, or it has finished. The first is
				 * worth waiting through with the client still
				 * connected; the second means there is nothing
				 * left to serve.
				 */
				if (co_console_guest_gone(handle)) {
					leaving = PTRUE;
					break;
				}
				Sleep(200);
				continue;
			}

			if (taken && taken < pending)
				co_memmove(netbuf, netbuf + taken, pending - taken);
			pending -= taken;

			if (produced) {
				int sent = 0;

				while (sent < (int)produced) {
					int n = send(client, outbuf + sent,
						     produced - sent, 0);
					if (n <= 0) {
						if (WSAGetLastError() == WSAEWOULDBLOCK) {
							Sleep(5);
							continue;
						}
						break;
					}
					sent += n;
				}
			}

			/* idle only when there was nothing to do either way */
			if (!produced && !pending)
				Sleep(10);
		}

		co_terminal_print("console: client gone\n");
		closesocket(client);

		if (leaving)
			break;
	}

	closesocket(listener);
	co_os_manager_close(handle);
	WSACleanup();

	return CO_RC(OK);
}
