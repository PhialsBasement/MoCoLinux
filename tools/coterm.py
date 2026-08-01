#!/usr/bin/env python3
"""Talk to the guest's terminal.

The guest's console is served over TCP by colinux-daemon --console on the test
box. This is just a socket with a keyboard on one end -- interactively when run
with no arguments, or scripted by passing commands, which is what makes it
usable from an agent that has no tty of its own.

    coterm.py                       interactive
    coterm.py 'uname -a' 'ls /'     send these, print what comes back
"""

import select
import socket
import sys
import time

HOST = "192.168.137.54"
PORT = 2323


def drain(sock, seconds):
    """Collect whatever arrives within a window, printing as it lands."""
    out = []
    deadline = time.time() + seconds
    while time.time() < deadline:
        r, _, _ = select.select([sock], [], [], 0.2)
        if not r:
            continue
        data = sock.recv(4096)
        if not data:
            break
        text = data.decode('utf-8', 'replace')
        out.append(text)
        sys.stdout.write(text)
        sys.stdout.flush()
    return ''.join(out)


def main():
    commands = sys.argv[1:]

    sock = socket.create_connection((HOST, PORT), timeout=10)
    sock.setblocking(False)

    # Whatever the shell has already printed, plus a nudge to redraw a prompt.
    drain(sock, 2)
    sock.sendall(b'\n')
    drain(sock, 2)

    if not commands:
        print('--- interactive; ctrl-c to quit ---')
        while True:
            r, _, _ = select.select([sock, sys.stdin], [], [], 0.2)
            if sock in r:
                data = sock.recv(4096)
                if not data:
                    break
                sys.stdout.write(data.decode('utf-8', 'replace'))
                sys.stdout.flush()
            if sys.stdin in r:
                line = sys.stdin.readline()
                if not line:
                    break
                sock.sendall(line.encode())
        return

    for command in commands:
        print('\n===> %s' % command)
        sock.sendall(command.encode() + b'\n')
        drain(sock, 6)

    sock.close()


if __name__ == '__main__':
    main()
