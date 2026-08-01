# MoCoLinux

Cooperative Linux for x86-64. A modern Linux kernel running as a guest **inside
Windows XP x64**, at native speed, on real hardware — no hypervisor, no
emulation, no virtualisation extensions.

```
[root@mocolinux ~]# systemctl is-system-running
running
[root@mocolinux ~]# ping -c 3 10.0.2.2
64 bytes from 10.0.2.2: icmp_seq=2 ttl=255 time=10.0 ms
3 packets transmitted, 3 received, 0% packet loss
[root@mocolinux ~]# pacman -Sy
:: Synchronizing package databases...
 core downloading...
 extra downloading...
```

That is Arch Linux with systemd, an ext4 root and a working network connection,
on a Lenovo M92p whose actual operating system is Windows XP x64 — both kernels
resident on the same processor at the same time, taking turns. The package
databases came down over HTTPS, through two ring buffers in the guest's own
memory and a NAT running as a Windows process.

## What cooperative virtualisation is

Not a virtual machine. There is no guest-physical address space, no shadow page
tables, no VT-x. Both kernels run at ring 0 on the bare processor, and a
**world switch** swaps the entire CPU context — CR0/CR2/CR3/CR4/CR8, GDT, IDT,
LDT, TR, segment selectors, the per-CPU MSRs, the debug registers and the FPU
state — between one and the other. The guest is trusted, exactly as a driver is
trusted, because it *is* running with kernel privilege.

The original [coLinux](http://colinux.org/) did this for i386 and stopped being
maintained around 2011. This is a port of that idea to x86-64, against a 2026
kernel, on the last Windows that will run the original.

## Status

Working, on hardware:

- Boots **Arch Linux with systemd** to a multi-user target, no failed units
- Mounts an **ext4 root** over a cooperative block device
- Runs **processes in ring 3** — systemd, a login shell, the lot
- Serves an **interactive terminal** over TCP (hvc console, `/dev/hvc0`)
- **Networking**: an ethernet device in the guest, NAT on the host, and
  `pacman -Sy` fetching package databases over HTTPS
- Takes the host's clock as virtual time, so `jiffies` advance and sleeps wake
- Shuts down cleanly, and a run can be ended on demand from another process
- Survives interrupt storms: hardware interrupts cross back and are replayed
  into Windows' live IDT, so the host never goes deaf

Not yet: SMP, the coLinux message layer (`co_monitor_t`, queues, reactor) that
upstream's `cocon`/`conet` consoles and devices expect — which is why
`colinux-console-nt` cannot attach to a guest here — a DHCP client in the guest
rather than a static address, inbound port redirects, `CONFIG_SECURITY_LANDLOCK`
(pacman sandboxes its downloads and refuses without it), and a repaired
incremental patch series: `patch/7.1.5/current-tree-snapshot.diff` is the
authoritative guest-side diff and is deliberately not in `series`. `TODO` is
current and specific about the rest.

## How it fits together

```
  Windows XP x64                          Linux 7.1.5
  ─────────────                           ───────────
  colinux-daemon.exe ──ioctl──►  linux.sys
                                    │
                                    │  co_switch_full  ◄─── the passage page:
                                    ▼                       mapped at the same
                              [ world switch ]              address in BOTH
                                    │                       address spaces
                                    ▼
                              guest kernel ──► ring 3 processes
```

The **passage page** is the fulcrum: a handful of pages mapped at an identical
virtual address on both sides, holding the switch code, both saved CPU states,
an IST stack, a TSS and the console ring. Code executing there is valid
mid-crossing, when CR3 has changed but nothing else has.

Guest RAM is host-contiguous memory described at its true physical addresses in
a synthesised e820, because Linux calls `__va()` on its own page-table entries —
so guest physical must equal host physical.

### Networking

The guest has an ordinary ethernet device whose transport is two byte rings in
its own `.bss` — one writer per word per direction, so no lock is needed between
two kernels that share no scheduler. Frames travel whole: a 32-bit length, the
frame, padding to a four-byte boundary, so a record never straddles the wrap.

```
  guest: conet_colinux.c            host: kernel/net.c        colinux-slirp-net-daemon -R
  ────────────────────              ──────────────────        ───────────────────────────
  ndo_start_xmit ─► TX ring ──►  CONET_DUMP / CONET_TAKE ──►  slirp_input
                                 (walk the guest's tables                │
  RX kthread  ◄── RX ring  ◄───   under net_lock)          ◄── slirp_output ──► winsock
```

The host reaches the rings the way a crash-dump reader would — by walking the
guest's page tables — and the address it walks from is retired under a lock
before the address space is freed, because a second process polling a guest that
has just exited is a use-after-free in a driver, which bugchecks rather than
fails. NAT is coLinux's vendored slirp, running in a process linked into a 2 GB
address space so that the 32-bit queue links inside its wire structures can hold
a pointer at all.

### Layout

| Path | What |
| --- | --- |
| `src/colinux/arch/x86_64/switch.c` | the world switch, fault stubs, monitor loop |
| `src/colinux/kernel/kload.c` | loads vmlinux into host memory, builds the guest's tables |
| `src/colinux/kernel/cobd.c` | cooperative block device, host half |
| `src/colinux/kernel/console.c` | terminal rings, host half |
| `src/colinux/kernel/net.c` | network rings, host half — read, consume, inject |
| `src/colinux/user/conet_ring.c` | the ring format and its decoder, shared by both readers |
| `src/colinux/user/slirp/` | vendored slirp, with its Win64 repairs |
| `src/colinux/user/elf_load.c` | the daemon: ELF loading, symbol resolution, boot |
| `patch/7.1.5/` | the guest-side kernel changes, including `drivers/net/conet_colinux.c` |
| `tools/decode-minidump.py` | attribute a bugcheck's stack to this driver |
| `tools/pe-clear-laa.py` | confine the slirp daemon to 2 GB of address space |
| `doc/porting-x86_64` | design notes for the port |
| `doc/building-modern` | the working build recipe |

## Building

Needs a cross toolchain (`mingw-w64-gcc`, `binutils`) and two kernel trees, which
is less odd than it looks: the Windows side still builds against 2.6.33 headers,
because the passage-page ABI is a header inside the guest kernel tree and the
driver includes it, while the guest itself is 7.1.5. The 2.6.33 tree is a header
source and nothing else — no part of it is built or run.

See `doc/building-modern` for the full recipe; briefly:

```sh
# the Windows side -- driver and daemons
cd src
COLINUX_ARCH=x86_64 \
COLINUX_TARGET_KERNEL_PATH=/path/to/linux-2.6.33.7-source \
COLINUX_TARGET_KERNEL_SOURCE=/path/to/linux-2.6.33.7-source \
COLINUX_TARGET_KERNEL_BUILD=/path/to/linux-2.6.33.7-build \
    python3 ../bin/make.py colinux

# the guest kernel: apply patch/7.1.5/current-tree-snapshot.diff to a
# 7.1.5 tree, then build vmlinux normally
```

`CONFIG_KASAN` must be off — the host's allocation lands in PML4 slot 501,
inside Linux's KASAN shadow region.

## Running

```
colinux-daemon.exe --boot-kernel vmlinux --max-switches none \
                   --cobd0 \??\C:\path\to\root.img
colinux-daemon.exe --console 2323          (a second process: a terminal)
colinux-slirp-net-daemon.exe -R            (a third: NAT for the guest)
```

Then, in the guest, an address from slirp's fixed layout:

```sh
ip link set eth0 up
ip addr add 10.0.2.15/24 dev eth0
ip route add default via 10.0.2.2
echo nameserver 10.0.2.3 > /etc/resolv.conf
```

`tools/coterm.py` is a client for the console port.

On root filesystems, plainly: `tools/mkrootfs.sh` builds the **minimal BusyBox**
image, which is what the early bring-up used and what `--init /bin/sh` is for.
The **Arch root in the transcript above was built by hand** from
`download/archlinux-bootstrap-x86_64.tar.zst` — unpacked into an image with
`mke2fs -d`, then given a populated pacman keyring, a mirror, a network unit and
`systemd-networkd` enabled, all through the guest's own terminal. Nothing in the
tree reproduces that yet, which is the first entry under "The Arch root
filesystem is not reproducible" in `TODO`; the keyring in particular is not
optional, since without `pacman-key --populate archlinux` every package fails
verification as untrusted even though its signature is good.

The processes are separate on purpose: the one running the guest is inside a
single `ioctl` for as long as the guest lives, so it cannot also service a
socket. `--max-switches none` means no switch limit, and with a terminal
attached there is no deadline either — a session ends when you end it, with
`colinux-daemon.exe --stop`, a `poweroff` in the guest, or by unloading the
driver. All three ask the monitor loop to stop and get an answer within one
crossing.

There are two read-only instruments for the network path: `--net-dump` prints
the guest's rings and decodes the frames in them without the guest being able
to tell, and `--net-take` does the same and then consumes them.

## Notes from the port

A few things that cost real time and are recorded in the commit messages:

- **Real IF stays on.** A free-running guest runs with hardware interrupts
  *enabled*; they vector through the guest's IDT into a stub, world-switch back,
  and are replayed into Windows' live IDT. Keeping IF clear and relying on
  voluntary yields freezes the machine within milliseconds — a deaf core misses
  its clock and stops acknowledging other cores' TLB-shootdown IPIs.
- **But not across the crossing.** The switch writes CR3 several instructions
  before it loads the entering side's IDTR, and in that gap a stale IDTR points
  at a table the new address space may not map. Triple fault, no bugcheck, no
  dump. Every entrance to the switch must have real IF clear.
- **Anything restored per crossing must be saved per crossing** — including
  `GS_BASE`, which holds Windows' KPCR and moves whenever its scheduler runs.
- **The guest's interrupt flag is virtual, and hardware doesn't know that.** An
  interrupt gate clears the real IF on delivery and `SYSCALL` clears it from
  `MSR_SYSCALL_MASK`, so `local_irq_enable()` has to put the *hardware* flag
  back or the processor stays deaf for a whole syscall.
- **"All the callers" is a claim about a program, not a file.** The host's
  pointer to the guest's rings is retired under a lock before the address space
  is freed, and the four places in `manager.c` that do it were audited as if
  they were all of them. `co_kload_begin` is a fifth, in another file, and it
  frees the previous run's space at the start of a new one — so a second process
  polling across a re-run walked freed page tables. The retirement now happens
  inside `co_kload_free`, where no caller can forget it.
- **A lock taken one line too late is not a lock.** The pended-IRP pointer in
  the handle-close path was *read* outside the cancel spin lock and only
  *cleared* inside it, so two paths could complete the same IRP. Twenty-two
  years old, harmless until a polling second process made handle churn routine,
  then four bugchecks in one evening.
- **2004 code compiles for Win64 and still cannot run.** slirp's protocol
  structures are overlays on wire format, and mingw-w64 defaults to
  `-mms-bitfields`, which gives `u_int x:4, y:4` a four-byte storage unit:
  `struct ip` measures 24 bytes with `ip_src` eight bytes from where the wire
  has it, and every datagram parses as nonsense. `SIZEOF_CHAR_P` was hardcoded
  to 4 as well, and *both* of its values are wrong here — one breaks the
  overlays, the other truncates the pointers stored in their 32-bit queue
  links. Six `_Static_assert`s now pin those structure sizes.
- **An instrument that lies is worse than none.** `%p` in this tree's own
  `snprintf` fetched pointers through `unsigned long`, four bytes under LLP64,
  so every pointer the driver ever logged printed with its top half missing —
  and a truncated kernel address does not look wrong, it looks small.

## Licence

GPL v2, as coLinux was. See `LICENSE`.

Original coLinux by Dan Aloni and contributors; see `CREDITS` and `THANKS`.
