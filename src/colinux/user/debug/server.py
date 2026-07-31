#!/usr/bin/env python3

# Receive coLinux driver debug packets and append them to a file.
#
# Flushing after every packet is the whole point, not tidiness. This log exists
# to be read after the machine sending it has died -- if the last thing it said
# is sitting in this process's buffer, the log is worthless precisely when it is
# needed. The original wrote without flushing and appeared to capture nothing.

from socket import *
import sys

if len(sys.argv) < 2:
    sys.exit("usage: server.py <logfile>")

log = open(sys.argv[1], "ab", buffering=0)
s = socket(AF_INET, SOCK_DGRAM, 0)
s.bind(("0.0.0.0", 63000))

while True:
    data, address = s.recvfrom(0x10000)
    if len(data) == 0:
        break
    log.write(data)
