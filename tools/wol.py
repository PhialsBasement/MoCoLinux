#!/usr/bin/env python3
"""Wake the test box over the LAN.

    wol.py [mac] [broadcast]

The other half of tools/off.bat. Between them the machine can be powered down
and brought back without anybody being in front of it, which matters here
because a port that swaps CPU context with the Windows kernel produces a lot of
resets and each one used to mean a walk to the power button.

Defaults are the M92p's Intel 82579LM on the direct RJ45 link. That NIC keeps
its PHY powered when the machine is off, which is why this works at all -- the
wireless adapter does not, so a magic packet has to go over the wire.

Sent to the subnet broadcast and to 255.255.255.255, on ports 9 and 7, because
which of those a given BIOS listens on is not worth finding out the hard way at
two in the morning. The packet is trivially small and duplicates are harmless:
the NIC matches on the payload, not on how it arrived.

Measured: the box answers its transfer agent about twenty seconds after the
packet.

What this cannot do is help a machine that is frozen rather than off. Nothing
running under Windows can, including tools/off.bat -- see the note there about
Intel AMT, whose ports are closed on this machine.
"""

import socket
import sys

DEFAULT_MAC = "FC:4D:D4:2D:DB:4B"	# Intel 82579LM, the wired link
DEFAULT_BCAST = "192.168.137.255"


def magic(mac):
    raw = bytes.fromhex(mac.replace(":", "").replace("-", ""))
    if len(raw) != 6:
        raise ValueError(f"{mac!r} is not a six-byte MAC")
    return b"\xff" * 6 + raw * 16


def wake(mac=DEFAULT_MAC, bcast=DEFAULT_BCAST):
    packet = magic(mac)
    sent = 0

    for target in (bcast, "255.255.255.255"):
        for port in (9, 7):
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            try:
                sock.sendto(packet, (target, port))
                sent += 1
            except OSError as err:
                print(f"  {target}:{port}: {err}")
            finally:
                sock.close()

    return sent


def main():
    mac = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_MAC
    bcast = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_BCAST

    sent = wake(mac, bcast)
    if not sent:
        print("wol: nothing sent")
        return 1

    print(f"wol: woke {mac} ({sent} packets); it answers in about twenty seconds")
    return 0


if __name__ == "__main__":
    sys.exit(main())
