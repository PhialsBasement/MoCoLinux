# MoCoLinux

Cooperative Linux for x86-64. A modern Linux kernel running as a guest **inside
Windows XP x64**, at native speed, on real hardware — no hypervisor, no
emulation, no virtualisation extensions.

```
/ # uname -a
Linux (none) 7.1.5 #39 PREEMPT_DYNAMIC x86_64 GNU/Linux
/ # cat /proc/uptime
92.23 92.24
/ # ls /
bin  dev  etc  init  lost+found  proc  root  sbin  sys  tmp  usr  var
```

That is Linux 7.1.5 with an ext4 root, running BusyBox userspace, on a Lenovo
M92p whose actual operating system is Windows XP x64 — both kernels resident on
the same processor at the same time, taking turns.

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

- Boots a stock-configured Linux 7.1.5 to userspace
- Mounts an **ext4 root** over a cooperative block device
- Runs **processes in ring 3** — `init`, a shell, `mount`, `ps`, `free`
- Serves an **interactive terminal** over TCP (hvc console, `/dev/hvc0`)
- Takes the host's clock as virtual time, so `jiffies` advance and sleeps wake
- Survives interrupt storms: hardware interrupts cross back and are replayed
  into Windows' live IDT, so the host never goes deaf

Not yet: networking, the coLinux message layer (`co_monitor_t`, queues,
reactor) that upstream's `cocon`/`conet` consoles and devices expect, SMP, and
a repaired incremental patch series — `patch/7.1.5/current-tree-snapshot.diff`
is currently the authoritative guest-side diff and is deliberately not in
`series`.

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

### Layout

| Path | What |
| --- | --- |
| `src/colinux/arch/x86_64/switch.c` | the world switch, fault stubs, monitor loop |
| `src/colinux/kernel/kload.c` | loads vmlinux into host memory, builds the guest's tables |
| `src/colinux/kernel/cobd.c` | cooperative block device, host half |
| `src/colinux/kernel/console.c` | terminal rings, host half |
| `src/colinux/user/elf_load.c` | the daemon: ELF loading, symbol resolution, boot |
| `patch/7.1.5/` | the guest-side kernel changes |
| `doc/porting-x86_64` | design notes for the port |
| `doc/building-modern` | the working build recipe |

## Building

Needs a cross toolchain (`mingw-w64-gcc`, `binutils`) and a Linux kernel tree.
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
colinux-daemon.exe --boot-kernel vmlinux --max-switches 400000 \
                   --cobd0 \??\C:\path\to\root.img
colinux-daemon.exe --console 2323          (a second process: a terminal)
```

`tools/mkrootfs.sh` builds an ext4 root image with BusyBox in it, and
`tools/coterm.py` is a client for the console port.

The two processes are separate on purpose: the one running the guest is inside
a single `ioctl` for as long as the guest lives, so it cannot also service a
socket.

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

## Licence

GPL v2, as coLinux was. See `LICENSE`.

Original coLinux by Dan Aloni and contributors; see `CREDITS` and `THANKS`.
