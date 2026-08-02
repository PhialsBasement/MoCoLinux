#!/usr/bin/env python3
"""Talk to the guest's terminal.

The guest's console is served over TCP by colinux-daemon --console on the test
box. This is just a socket with a keyboard on one end -- interactively when run
with no arguments, or scripted by passing commands, which is what makes it
usable from an agent that has no tty of its own.

    coterm.py                       interactive
    coterm.py 'uname -a' 'ls /'     send these, print what comes back
    coterm.py -t 1800 'pacman -Syu' allow half an hour for one command

Exits with the status of the last command that ran, so a script can tell
whether the thing it asked for worked.
"""

import os
import re
import select
import socket
import sys
import time

HOST = os.environ.get("COTERM_HOST", "192.168.137.54")
PORT = int(os.environ.get("COTERM_PORT", "2323"))

# How long one command may take before we give up waiting for its sentinel.
# Generous by default: this terminal is used to drive package installs and
# filesystem builds, and the previous six-second window is what made it useless
# for exactly those.
DEFAULT_TIMEOUT = float(os.environ.get("COTERM_TIMEOUT", "900"))


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

#
# The sentinel, and why it is spelled with a quote in the middle.
#
# Waiting a fixed number of seconds for output cannot work: this terminal drives
# package installs and mke2fs runs, and anything slower than the window was
# simply lost -- it arrived after the client had disconnected, and until the
# driver's console ioctl was fixed to honour a zero-length read it was then
# destroyed outright by the server's own liveness probe. Even with that fixed,
# a fixed window either truncates slow commands or wastes time on fast ones.
#
# So the shell is asked to say when it is done, and we read until it does. The
# awkward part is that the console echoes what we type, so a naive marker
# appears twice: once in the echo of the command line, once in the real output.
# Splitting the literal with a shell-level quote means the echoed form contains
# COTERM''_END and the printed form contains COTERM_END, so matching the latter
# cannot match our own echo.
#
# The sentinel carries $? as well, which is the command's exit status -- the
# thing a caller most often actually wants and previously had to ask for in a
# separate round trip.
SENTINEL_SEND = "printf '\\nCOTERM''_END_%d\\n' $?"
SENTINEL_RE = re.compile(r'COTERM_END_(\d+)')


def clean(text):
    return CSI.sub('', OSC.sub('', text))


def read_until(sock, pattern, timeout, echo=True):
    """Read until pattern matches, or the deadline passes.

    Returns (text, match). match is None on timeout, and the caller is expected
    to say so rather than pretend the output was complete -- a terminal that
    silently truncates is how a failed command reads as a successful one.
    """
    buf = []
    joined = ''
    deadline = time.time() + timeout

    while time.time() < deadline:
        r, _, _ = select.select([sock], [], [], 0.5)
        if not r:
            continue
        data = sock.recv(65536)
        if not data:
            break
        text = clean(data.decode('utf-8', 'replace'))
        buf.append(text)
        if echo:
            sys.stdout.write(text)
            sys.stdout.flush()
        joined = ''.join(buf)
        m = pattern.search(joined)
        if m:
            return joined, m

    return joined, None


def sync(sock):
    """Get to a known state: a prompt, with nothing of the past still in flight.

    Whatever a previous session left in the ring arrives first, so it is read
    and discarded up to our own sentinel rather than being mistaken for the
    output of the first command.
    """
    sock.sendall(b'\n' + SENTINEL_SEND.encode() + b'\n')
    read_until(sock, SENTINEL_RE, 30, echo=False)


def main():
    args = sys.argv[1:]
    timeout = DEFAULT_TIMEOUT

    if args and args[0] in ('-t', '--timeout'):
        timeout = float(args[1])
        args = args[2:]

    sock = socket.create_connection((HOST, PORT), timeout=10)
    sock.setblocking(False)

    if not args:
        sync(sock)
        print('--- interactive; ctrl-c to quit ---')
        while True:
            r, _, _ = select.select([sock, sys.stdin], [], [], 0.2)
            if sock in r:
                data = sock.recv(65536)
                if not data:
                    break
                sys.stdout.write(clean(data.decode('utf-8', 'replace')))
                sys.stdout.flush()
            if sys.stdin in r:
                line = sys.stdin.readline()
                if not line:
                    break
                sock.sendall(line.encode())
        return 0

    sync(sock)

    status = 0
    for command in args:
        print('\n===> %s' % command)
        sock.sendall(command.encode() + b'; ' + SENTINEL_SEND.encode() + b'\n')
        _, m = read_until(sock, SENTINEL_RE, timeout)

        if m is None:
            print('\n--- coterm: no sentinel after %gs; the command may still be'
                  ' running in the guest' % timeout)
            sock.close()
            return 124	# same convention as timeout(1)

        status = int(m.group(1))

    sock.close()
    return status


if __name__ == '__main__':
    sys.exit(main())
