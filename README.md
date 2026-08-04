# MoCoLinux

Cooperative Linux for x86-64: a modern Linux kernel running as a guest inside
Windows XP x64 and Windows 7 x64 on real hardware, without a hypervisor,
emulation, or virtualization extensions. It is a port of
[coLinux](http://colinux.org/) (i386, unmaintained since ~2011) to x86-64,
against a 2026 kernel.

```
[root@mocolinux ~]# systemctl is-system-running
running
[root@mocolinux ~]# fastfetch
  OS: Manjaro Linux x86_64
  Host: 3209AV5 (ThinkCentre M92p)
  Kernel: Linux 7.1.5
  Packages: 792 (pacman)[stable]
  Memory: 201.96 MiB / 929.75 MiB (22%)
  Disk (/): 5.76 GiB / 29.36 GiB (20%) - ext4
  Local IP (eth0): 10.0.2.15/24
```

![Manjaro with KDE running as native windows on Windows XP x64](doc/img/mocolinux-desktop.png)

The screenshot is one machine: KDE Plasma 6.7.3 applications and `cmd.exe`
(Windows 5.2.3790) sharing the same desktop and taskbar. No X server runs in
the guest — applications are X clients talking to a VcXsrv on the Windows side
in multiwindow mode (`DISPLAY=10.0.2.2:0`; slirp rewrites that address to the
host's loopback).

![The same guest on Windows 7 x64: Dolphin, Kate and Konsole as Aero windows](doc/img/mocolinux-win7-desktop.png)

The same driver, kernel and root image on Windows 7 x64 — Dolphin, Kate and
Konsole with Aero frames and taskbar buttons, `fastfetch` reporting the
ThinkCentre's own i5-3470. One binary serves both hosts: XP ignores embedded
signatures, and Windows 7 accepts the same test-signed driver once
`bcdedit /set testsigning on` is in force, which Setup does for you.

## How it works

There is no guest-physical address space, no shadow page tables, no VT-x. Both
kernels run at ring 0 on the bare processor; a **world switch** swaps the full
CPU context (CR0/CR2/CR3/CR4/CR8, GDT, IDT, LDT, TR, segment selectors, per-CPU
MSRs, debug registers, FPU state) between them. The guest is trusted, like a
driver, because it runs with kernel privilege.

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

The **passage page** is a handful of pages mapped at an identical virtual
address on both sides, holding the switch code, both saved CPU states, an IST
stack, a TSS and the console ring — valid mid-crossing, when CR3 has changed
but nothing else has.

Guest RAM is host-contiguous memory described at its true physical addresses in
a synthesized e820 (Linux calls `__va()` on its own page-table entries, so
guest physical must equal host physical).

### Networking

The guest has an ordinary ethernet device backed by two lock-free byte rings in
its own `.bss` (one writer per word per direction). Frames are stored whole:
32-bit length, frame, padding to a 4-byte boundary, never straddling the wrap.

```
  guest: conet_colinux.c            host: kernel/net.c        colinux-slirp-net-daemon -R
  ────────────────────              ──────────────────        ───────────────────────────
  ndo_start_xmit ─► TX ring ──►  CONET_DUMP / CONET_TAKE ──►  slirp_input
                                 (walk the guest's tables                │
  RX kthread  ◄── RX ring  ◄───   under net_lock)          ◄── slirp_output ──► winsock
```

The host reads the rings by walking the guest's page tables; the base address
is retired under a lock before the address space is freed. NAT is coLinux's
vendored slirp, run in a process confined to a 2 GB address space so its
32-bit queue links can hold pointers.

## Status

Working, verified on hardware (Lenovo ThinkCentre M92p, i5-3470) under both
Windows XP x64 and Windows 7 x64:

- Boots Manjaro with systemd to multi-user target, no failed units, from an
  image the tree builds (`tools/mkmanjarorootfs.sh`)
- ext4 root over a cooperative block device; ring-3 processes
- Interactive terminal over TCP (hvc console, `/dev/hvc0`)
- KDE applications as native Windows windows, rootless, over a host-side X
  server
- Preemption: the host interrupts a busy-looping task; time advances at real
  speed
- Networking: guest ethernet device, host NAT, static address via
  `systemd-networkd`; pacman installs a 791-package desktop over HTTPS at
  16 MB/s
- 32-bit binaries (the guest keeps its own `int $0x80` gate)
- Landlock and user namespaces (required by pacman 7 and modern sandboxes)
- Virtual time from the host clock; clean shutdown; stop-on-demand from
  another process
- Hardware interrupts are forwarded across the switch and replayed into
  Windows' live IDT
- Asynchronous block I/O on kernel worker threads (one queue and worker per
  unit); console latency stays at 16–125 ms through a full package install.
  `--sync-cobd` restores the synchronous path for comparison
- Self-installing: `mocolinux-setup.exe` lays down driver, daemons, kernel,
  X server and launchers, then boots Linux and builds a Manjaro system on a
  fresh image over the network

Not yet:

- SMP
- The coLinux message layer (`co_monitor_t`, queues, reactor), so upstream's
  `cocon`/`conet` consoles and devices — including `colinux-console-nt` —
  cannot attach
- DHCP in the guest (static address only); inbound port redirects
- Hardware-accelerated OpenGL beyond GLX 1.4 (applications render in llvmpipe
  on the guest's single core — the main reason the desktop feels slow)
- A repaired incremental patch series: `patch/7.1.5/current-tree-snapshot.diff`
  is the authoritative guest-side diff and is deliberately not in `series`

Known issue: the first boot after a host reboot is reliable, the second is
not. Guest RAM is allocated per run from `MmAllocateContiguousMemory` in 32 MB
runs, and a run's churn fragments host physical memory, so the next boot's
allocations grind the machine. Allocate-once-per-driver-load is the
workaround; pseudo-physical memory is the fix. Both are in `TODO`.

## Layout

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
| `tools/mkmanjarorootfs.sh` | builds the Manjaro desktop image |
| `tools/mkrootfs.sh` | builds the minimal BusyBox bring-up image |
| `tools/coterm.py` | client for the guest's console port |
| `tools/shot.cs` | screenshots the host desktop, compiled and run on the box |
| `tools/decode-minidump.py` | attribute a bugcheck's stack to this driver |
| `tools/pe-clear-laa.py` | confine the slirp daemon to 2 GB of address space |
| `doc/runbook` | box to desktop, and host to installed Manjaro |
| `doc/porting-x86_64` | design notes for the port |
| `doc/building-modern` | the working build recipe |

## Building

Requires a cross toolchain (`mingw-w64-gcc`, `binutils`) and two kernel trees:
the Windows side builds against 2.6.33 headers (the passage-page ABI is a
header inside the guest kernel tree), while the guest kernel is 7.1.5. The
2.6.33 tree is used for headers only — nothing in it is built or run.

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

The short way, on the target machine: run `mocolinux-setup.exe`. It installs
the driver, daemons, kernel, VcXsrv and launchers, creates desktop shortcuts
and a logon entry, then boots Linux and builds a Manjaro system on a fresh
disk image over the network (~15 minutes, ~2.5 GB of downloads — the release
carries no built desktop, only the seed).

![The installer's desktop shortcuts, and four Linux applications started from them](doc/img/moco-desktop.png)

The long way, which is what the installer runs and what to use while
developing:

```
colinux-daemon.exe --boot-kernel vmlinux --max-switches none \
                   --cobd0 \??\C:\path\to\root.img
colinux-daemon.exe --console 2323          (a second process: a terminal)
colinux-slirp-net-daemon.exe -R            (a third: NAT for the guest)
colinux-daemon.exe --run konsole           (start one app in a running guest)
```

- `--mem MB` sets guest RAM (default 1024). It is a target: the e820 describes
  what the host can actually produce in unbroken 32 MB runs, and falling short
  is reported rather than fatal.
- The image ships `10-eth0.network` with slirp's fixed layout and
  `systemd-networkd` enabled, so the guest configures its own network at boot.
- For a desktop, start an X server on the Windows side in multiwindow mode
  (`vcxsrv :0 -multiwindow -ac`, or `dist-x64/xstart.bat`) and run X clients
  in the guest. `DISPLAY=10.0.2.2:0` is already in the image's environment.
- `tools/mkrootfs.sh` builds the minimal BusyBox image (`--init /bin/sh`);
  `tools/mkmanjarorootfs.sh` builds the Manjaro desktop image (needs a Linux
  host with `pacman` and `e2fsprogs`; handles the alpm skeleton, package
  install, keyring, `ldconfig`, network unit, fonts and `lib32` packages).
- The daemon processes are separate on purpose: the one running the guest sits
  inside a single ioctl for the guest's lifetime and cannot also service a
  socket. End a session with `colinux-daemon.exe --stop`, a `poweroff` in the
  guest, or by unloading the driver.
- `--net-dump` prints the guest's network rings and decodes their frames
  read-only; `--net-take` does the same and consumes them.

## Porting notes

Constraints that shaped the port, recorded in full in `doc/porting-x86_64`
and the commit history:

- A free-running guest keeps real IF **enabled**: hardware interrupts vector
  through the guest's IDT into a stub, world-switch back, and are replayed
  into Windows' live IDT. Relying on voluntary yields deafens the core to its
  clock and to TLB-shootdown IPIs.
- Real IF must be **clear across the crossing** itself: between the CR3 write
  and the IDTR load, a stale IDTR points into an unmapped table and any
  interrupt triple-faults with no dump.
- Anything the switch restores per crossing it must also save per crossing,
  including `GS_BASE` (Windows' KPCR moves with its scheduler).
- PatchGuard (new in XP x64) checksums the host's GDT/IDT/TSS on a randomized
  timer; any standing modification bugchecks `0x109` minutes later with
  nothing of yours on the stack. Host structures are restored exactly.
- Vector `0x80` is carved out of the host-owned 32–255 range: it is Linux's
  32-bit syscall gate (DPL 3), not a machine interrupt.
- The guest's IF is virtual; interrupt gates and `SYSCALL` clear the real
  flag, so `local_irq_enable()` must restore the hardware flag too.
- After boot handoff the guest runs on `init_top_pgt`; `cpu_entry_area` and
  the vmalloc region (with `CONFIG_VMAP_STACK`, most task stacks) exist only
  in the guest's tables. Host-side reads must walk the guest's live PML4.
- Ticks must be injected even when the guest never enters its kernel;
  otherwise a busy userspace loop owns the CPU forever and every watchdog
  that would report it runs on the tick that stopped.
- slirp's wire-overlay structs require `-mno-ms-bitfields` (else `struct ip`
  is 24 bytes and nothing parses); its `errno` is `WSAGetLastError()`, so
  would-block checks must compare against `WSAEWOULDBLOCK`. Six
  `_Static_assert`s pin the structure sizes.
- NTFS zero-fills sparse tails synchronously inside a write;
  `FileValidDataLengthInformation` removes minutes-long first writes on large
  images.
- The host's pointer into a guest's rings is retired under a lock inside
  `co_kload_free`, where no caller can forget it; pended IRPs are read and
  cleared under the cancel spin lock.
- `%p` in the tree's own `snprintf` must fetch 64 bits under LLP64, or every
  logged pointer prints with its top half missing.

## Licence

GPL v2, as coLinux was. See `LICENSE`.

Original coLinux by Dan Aloni and contributors; see `CREDITS` and `THANKS`.
