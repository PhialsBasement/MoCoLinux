#!/usr/bin/env python3
"""Talk to the guest's terminal.

The guest's console is served over TCP by colinux-daemon --console on the test
box. This is just a socket with a keyboard on one end -- interactively when run
with no arguments, or scripted by passing commands, which is what makes it
usable from an agent that has no tty of its own.

    coterm.py                       interactive
    coterm.py 'uname -a' 'ls /'     send these, print what comes back
"""

import re
import select
import socket
import sys
import time

HOST = "192.168.137.54"
PORT = 2323


# Shell-integration reports and paste markers, which are for a terminal
# emulator and are noise to everything else.
#
# Arch's bash ships OSC 3008 sequences carrying the machine id, boot id, user,
# host, pid, cwd and an exit status around every prompt and every command --
# several hundred bytes of it before each line that actually matters. Left in,
# they bury the output of every command in this file's own transcripts, and
# every reader ends up writing the same regex again by hand.
#
# Stripped: OSC (ESC ] ... BEL or ST, and the stray backslash a truncated ST
# leaves), and every CSI sequence -- colours, cursor moves, bracketed paste.
#
# The colours are stripped too, deliberately. This terminal's readers are a log
# file and a person reading a transcript, and "\x1b[1;36meth0\x1b[0m" is worse
# than "eth0" for both. Nothing here is a full-screen program that needs cursor
# control; if one ever is, it wants its own client rather than this.
OSC = re.compile(r'\x1b\][^\x07\x1b]*(?:\x07|\x1b\\\\?|\\)?')
CSI = re.compile(r'\x1b\[[0-9;?]*[a-zA-Z]')


def clean(text):
    return CSI.sub('', OSC.sub('', text))


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
        text = clean(data.decode('utf-8', 'replace'))
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
