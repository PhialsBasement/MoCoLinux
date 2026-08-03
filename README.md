# MoCoLinux

Cooperative Linux for x86-64. A modern Linux kernel running as a guest **inside
Windows XP x64**, at native speed, on real hardware — no hypervisor, no
emulation, no virtualisation extensions.

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

That is Manjaro with systemd, an ext4 root and a working network connection, on
a Lenovo M92p whose actual operating system is Windows XP x64 — both kernels
resident on the same processor at the same time, taking turns. Those 792
packages were installed by `pacman` over HTTPS, through two ring buffers in the
guest's own memory and a NAT running as a Windows process.

And since a terminal only proves so much:

![Manjaro with KDE running as native windows on Windows XP x64](doc/img/mocolinux-desktop.png)

Everything in that screenshot is one machine. On the left, `fastfetch` in an
xterm: Manjaro Linux, kernel 7.1.5, 792 packages. In the middle, KDE's own
Info Centre — Plasma 6.7.3, Qt 6.11.1, graphics platform X11. At the bottom,
`cmd.exe` answering `ver` with **Microsoft Windows [Version 5.2.3790]**. Both
systems report the same Intel i5-3470, because there is only one, and both
Linux applications have entries in the XP taskbar, because to Windows they are
ordinary windows.

No X server runs in the guest. The applications are X clients talking to a
server on the Windows side in multiwindow mode, reached at `DISPLAY=10.0.2.2:0`
— slirp rewrites that address to the host's own loopback, so the connection
never leaves the machine it started on. It is the arrangement coLinux-i386
used, and it needs no code in the driver at all.

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

- Boots **Manjaro with systemd** to a multi-user target, no failed units, from
  an image the tree builds (`tools/mkmanjarorootfs.sh`)
- Mounts an **ext4 root** over a cooperative block device
- Runs **processes in ring 3** — systemd, a login shell, the lot
- Serves an **interactive terminal** over TCP (hvc console, `/dev/hvc0`)
- Runs a **KDE desktop's applications as native Windows windows**, rootless,
  over an X server on the host — no X server, framebuffer or desktop shell in
  the guest
- **Preempts a running task.** A bare `while :; do :; done` in userspace used
  to freeze the guest permanently; the host now interrupts it, and time
  advances across it at real speed
- **Networking**: an ethernet device in the guest, NAT on the host, a static
  address configured by `systemd-networkd` at boot, and `pacman` installing a
  791-package desktop over HTTPS at 16 MB/s
- Runs **32-bit binaries** — the guest keeps its own `int $0x80` gate, so the
  IA32 syscall path works and `ldd` resolves
- **Landlock** and **user namespaces**, which pacman 7 and every modern sandbox
  refuse to run without
- Takes the host's clock as virtual time, so `jiffies` advance and sleeps wake
- Shuts down cleanly, and a run can be ended on demand from another process
- Survives interrupt storms: hardware interrupts cross back and are replayed
  into Windows' live IDT, so the host never goes deaf
- **Asynchronous block I/O.** Transfers run on kernel worker threads instead of
  inline on the crossing, so the guest keeps scheduling while its disk works. It
  used to freeze solid for the duration of every request — measured 8.7 s of
  guest CPU over 5 m 14 s of wall clock during a package install, which tripped
  systemd's own service watchdogs. A full build now answers the console in
  16–125 ms throughout. `--sync-cobd` restores the old path for comparison
- **Installs itself.** A single Win32 executable lays down the driver, the
  daemons, the kernel, an X server and the launchers, creates desktop shortcuts
  and a logon entry, then boots Linux and has it build its own Manjaro system on
  a fresh disk image over the network. Verified end to end: 217 console polls,
  zero timeouts, 1.3 GB downloaded, guest responsive the whole way

Not yet: SMP, the coLinux message layer (`co_monitor_t`, queues, reactor) that
upstream's `cocon`/`conet` consoles and devices expect — which is why
`colinux-console-nt` cannot attach to a guest here — a DHCP client in the guest
rather than a static address, inbound port redirects, hardware-accelerated
OpenGL past the 1.4 the GLX wire protocol can carry (applications render in
llvmpipe on the guest's single core, which is the main thing that makes a
graphical desktop feel slow), and a repaired incremental patch series:
`patch/7.1.5/current-tree-snapshot.diff` is the authoritative guest-side diff
and is deliberately not in `series`. `TODO` is current and specific about the
rest.

One caveat worth knowing before you run it, because it will look like a bug in
your machine rather than in this one: **the first boot after a host reboot is
reliable and the second is not.** Guest RAM is freed and re-allocated every run,
and it has to come from `MmAllocateContiguousMemory` in unbroken 32 MB runs, so
a run's worth of churn leaves the host's physical memory holed and the next
boot's sixty-odd allocations grind the whole machine while Windows tries to
manufacture runs that no longer exist. Task Manager shows memory available
throughout. Allocating once per driver load and reusing is the fix; pseudo-
physical memory is the real one, and both are in `TODO`.

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
| `tools/mkmanjarorootfs.sh` | builds the Manjaro desktop image, from nothing |
| `tools/mkrootfs.sh` | builds the minimal BusyBox image used for bring-up |
| `tools/coterm.py` | a client for the guest's console port |
| `tools/shot.cs` | screenshots the XP desktop, compiled and run on the box |
| `tools/decode-minidump.py` | attribute a bugcheck's stack to this driver |
| `tools/pe-clear-laa.py` | confine the slirp daemon to 2 GB of address space |
| `doc/runbook` | the two sequences: box to desktop, and host to installed Manjaro |
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

The short way, on the target machine: run **`mocolinux-setup.exe`**. It lays down
the driver, the daemons, the kernel, VcXsrv and the launchers, makes desktop
shortcuts and a logon entry, asks for a restart, and then boots Linux and has it
build its own Manjaro system on a fresh disk image over the network. After that
the machine has Linux at every logon and an icon per application; **Start
MoCoLinux** brings it up by hand if it was stopped.

That takes about fifteen minutes and roughly 2.5 GB of downloads, nearly all of
it pacman fetching packages on the target — the release itself carries no built
desktop, only the seed that can make one.

![The installer's desktop shortcuts, and four Linux applications started from them](doc/img/moco-desktop.png)

The long way, which is what the short way runs and what to use while developing:

```
colinux-daemon.exe --boot-kernel vmlinux --max-switches none \
                   --cobd0 \??\C:\path\to\root.img
colinux-daemon.exe --console 2323          (a second process: a terminal)
colinux-slirp-net-daemon.exe -R            (a third: NAT for the guest)
colinux-daemon.exe --run konsole           (start one app in a running guest)
```

`--mem MB` sets the guest's RAM; the default is 1024. It is a target rather than
a demand — what the host can actually produce in unbroken 32 MB runs is what the
e820 describes, and falling short is reported rather than fatal.

The guest configures its own network at boot, so there is nothing to type: the
image ships `10-eth0.network` with slirp's fixed layout and `systemd-networkd`
enabled. `tools/coterm.py` is a client for the console port.

For a desktop, start an X server on the Windows side in multiwindow mode
(`vcxsrv :0 -multiwindow -ac`, or `dist-x64/xstart.bat`) and run X clients in
the guest. `DISPLAY=10.0.2.2:0` is already in the image's environment, and slirp
rewrites that address to the host's own loopback, so the connection never leaves
the machine and no port redirect is involved. There is no X server in the guest
and none is wanted — the Windows server is also the window manager, which is
what makes the windows native.

On root filesystems: `tools/mkrootfs.sh` builds the **minimal BusyBox** image,
which is what the early bring-up used and what `--init /bin/sh` is for.
`tools/mkmanjarorootfs.sh` builds the **Manjaro desktop** image in the
screenshot — it wants a Linux host with `pacman` and `e2fsprogs`, takes either
an image file or a block device, and does the whole job: the directory skeleton
alpm needs before it will initialise, the package install, the keyring
(`pacman-key --populate`, which is not optional — without it every package fails
verification as untrusted while its signature is perfectly good), `ldconfig`,
the network unit, and the fonts and `lib32` packages a desktop turns out to
need.

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
- **The host's structures are not ours to edit, and PatchGuard is watching.**
  XP x64 is the first Windows with Kernel Patch Protection. A fix for an `ltr`
  race left the *host's* TSS descriptor marked available instead of busy — one
  standing bit in a live processor GDT — and the machine died minutes later with
  bugcheck `0x109 CRITICAL_STRUCTURE_CORRUPTION`, parameter 4 = 3, *"a processor
  GDT"*. It checks on a randomized timer, so the crash arrives long after the
  cause, with the box idle and nothing of yours on the stack; it also
  deliberately manifests as `STATUS_BREAKPOINT` in unrelated drivers. Hours went
  into hunting a socket bug that did not exist. This is the same reason `antinx`
  has no successor on x86-64.
- **A queue has to be given the fairness a serial path had by accident.** Making
  block I/O asynchronous with one worker and one FIFO was *slower* in the way
  that matters: a multi-minute write to one disk blocked another disk's reads
  behind it, and the guest could not page in its own shell. One queue and one
  worker per unit fixed it. The synchronous version was fair because it could
  only ever do one thing at a time.
- **NTFS zero-fills on your behalf, synchronously, inside your write.** A freshly
  reserved image has a valid-data length of zero, so the first write near its end
  makes the filesystem write everything in between first. On a 16 GB image that
  is minutes inside one request. `FileValidDataLengthInformation` removes it.
- **A tickbox that does not do anything is worse than no tickbox.** Setup asked
  whether to install the X server, recorded the answer and never acted on it, so
  applications launched into a display that did not exist — and an X client with
  no server produces no error whatsoever. `grep -i vcxsrv` over the installer
  found one hit, and it was the label.
- **A component that leaves on purpose is not a component that failed.** The
  slirp bridge disappearing mid-download was filed as a bug in the bridge. It
  was the bridge working: it gives up after two seconds of unreadable rings so
  the driver can be unloaded, and the guest really had gone — it was dying of the
  GDT corruption above. Three separate times that evening a symptom was mistaken
  for a cause, in each case by trusting a single quiet sample over a timeline.
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
- **A guest with no asynchronous entry cannot be preempted, and nothing says
  so.** Ticks were synthesised only at the idle boundary, and hardware
  interrupts are replayed into Windows without one guest instruction running —
  so a task that stayed runnable without entering the kernel owned the
  processor forever, and the idle task that would have advanced time was
  exactly what it prevented from running. A bare `while :; do :; done` froze
  the whole guest permanently. Nothing faulted, so there was no oops; and the
  softlockup, RCU-stall and hung-task detectors that exist to catch this all
  run on the tick it had stopped. The source had said it plainly for months —
  *"while the guest is busy computing, time is not delivered"* — filed as a
  note about clock accuracy rather than the hazard it was.
- **Vectors 32-255 belong to the host, except the one that does not.**
  Vector `0x80` sits inside that range and is not a machine interrupt: it is
  Linux's 32-bit syscall gate, installed DPL 3. Handing it to the host's stubs
  with the rest made `int $0x80` from ring 3 a #GP with error code
  `(0x80 << 3) | 2`. Every 32-bit binary in the port's life died there, and
  none could say so — `ld-linux.so.2` faults before it can report, so `ldd`
  lists every library as "not found" and installers conclude `libc.so.6` is
  missing while it sits in `/usr/lib32` in perfect health.
- **The host's page tables stop being the guest's the moment the guest gets
  its own.** After boot handoff the guest runs on `init_top_pgt`, and the graft
  is one way, so `cpu_entry_area` and the whole vmalloc region — with
  `CONFIG_VMAP_STACK`, most task stacks — exist only over there. Reading an
  interrupt frame through the host's load-time PML4 returns NOT_FOUND, and a
  caller that treats that as "nothing to do" does nothing, silently, forever.
  That is what made the first working cooperative timer look completely dead.
- **`errno` is not `errno` on Windows.** slirp defines it as
  `WSAGetLastError()`, so its `EAGAIN` check could never match the
  `WSAEWOULDBLOCK` that winsock actually returns, and every would-block read was
  treated as the peer hanging up. That one comparison was the entire network:
  1.68 MB/s before, 16.4 MB/s after.
- **An instrument that lies is worse than none.** `%p` in this tree's own
  `snprintf` fetched pointers through `unsigned long`, four bytes under LLP64,
  so every pointer the driver ever logged printed with its top half missing —
  and a truncated kernel address does not look wrong, it looks small.

## Licence

GPL v2, as coLinux was. See `LICENSE`.

Original coLinux by Dan Aloni and contributors; see `CREDITS` and `THANKS`.
