# MoCoLinux

MoCoLinux runs a modern Linux kernel as a guest inside Windows XP x64,
Windows 7 x64, Windows 8.1 x64 and Windows 10 x64 on real hardware. It does
not use a hypervisor, emulation, or virtualization extensions. It supports
fragmented physical-RAM backing and guest memory sizes up to 128 GB. It is an
x86-64 port of [coLinux](http://colinux.org/) (i386, unmaintained since about
2011), updated to a 2026 kernel.

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

The screenshot shows KDE Plasma 6.7.3 applications and `cmd.exe`
(Windows 5.2.3790) on the same desktop and taskbar. There is no X server in
the guest. The applications are X clients connected to VcXsrv on the Windows
side in multiwindow mode (`DISPLAY=10.0.2.2:0`; slirp rewrites this address
to the host loopback).

![The same guest on Windows 7 x64: Dolphin, Kate and Konsole as Aero windows](doc/img/mocolinux-win7-desktop.png)

The same driver, kernel and root image on Windows 7 x64. Dolphin, Kate and
Konsole have Aero frames and taskbar buttons, and `fastfetch` reports the
machine's i5-3470.

![The same guest again on Windows 8.1 x64, with cmd.exe reporting 6.3.9600 beside it](doc/img/mocolinux-win81-desktop.png)

The same setup on Windows 8.1 x64, with `cmd.exe` reporting 6.3.9600.

![And on Windows 10 IoT Enterprise LTSC, with KDE, Dolphin and fastfetch reporting virgl on the GT 730](doc/img/mocolinux-win10-desktop.png)

The same setup on Windows 10 IoT Enterprise LTSC (10.0.19044), with KVA
Shadow (KPTI) active. One binary works on all four hosts. XP ignores embedded
signatures. Windows 7, 8.1 and 10 load the same test-signed driver after
`bcdedit /set testsigning on`, which the installer enables. 8.1 and 10 have
additional requirements; see [Windows 8 and 8.1](#windows-8-and-81) and
[Windows 10](#windows-10).

## How it works

There is no guest-physical address space, no shadow page tables, and no VT-x.
Both kernels run at ring 0 on the processor. A world switch swaps the full
CPU context (CR0/CR2/CR3/CR4/CR8, GDT, IDT, LDT, TR, segment selectors,
per-CPU MSRs, debug registers, FPU state) between them. Because the guest
runs with kernel privilege, it is trusted in the same way a driver is.

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

The passage page is a small set of pages mapped at the same virtual address
in both address spaces. It holds the switch code, both saved CPU states, an
IST stack, a TSS and the console ring. Because it is mapped identically on
both sides, it stays usable during the switch, after CR3 has changed but
before the rest of the context has.

### FragRAM

FragRAM backs the guest with whatever physical pages Windows can provide;
guest RAM does not need to be physically contiguous. The earlier x86-64 port
used a Linux guest page number directly as the host machine page number. With
that scheme, a host could have enough free RAM in total and still fail to
boot the guest, because the free pages were scattered after normal use or
repeated guest runs.

The driver now allocates cached nonpaged-pool chunks that are contiguous in
its kernel virtual address space but may be scattered across physical RAM,
and records the machine frame behind every 4 KB page. Linux still sees dense
RAM starting at pseudo-physical address zero: a p2m table translates a guest
page number before it is placed in a hardware page table, and an m2p hash
translates it back when Linux reads that entry. The guest direct map uses
4 KB leaves, because a huge page would incorrectly imply that the host frames
are adjacent.

Supporting sizes up to 128 GB also required 64-bit memory sizes and block
accounting, allocation and p2m/m2p metadata sized for 131072 MiB, and a
change to where guest RAM physically lives. It is no longer nonpaged pool:
the boot daemon creates a named shared section (`Global\MoCoLinuxGuestRAM`)
sized for the configured RAM, and the driver locks and kernel-maps 8 MB
blocks of it, building the p2m from their frames exactly as before. Because
blocks are carved sequentially, a guest pseudo-physical address is an offset
into that section -- so the GPU daemon opens the same section and reaches
EVERY byte of guest RAM through one ordinary view, with no per-window
mappings, no working-set locking, and none of the kernel calls whose failure
mode is a bugcheck rather than an error. The old on-demand 12 MB windows
(with LRU eviction, `--kmap-cap MB`) survive only as the fallback for a
legacy driver.

This is not overcommit or ballooning. Memory advertised to Linux still needs
real nonpaged host backing. `--mem 131072` is the supported maximum, not a
guarantee that every supported Windows machine can spare 128 GB. If the host
supplies less than the requested amount, the shortfall is reported and the
guest e820 map describes the amount actually backed.

### Networking

The guest has an ordinary ethernet device backed by two lock-free byte rings
in its own `.bss` (one writer per word per direction). Frames are stored
whole: a 32-bit length, the frame, then padding to a 4-byte boundary. A frame
never straddles the ring wrap.

```
  guest: conet_colinux.c            host: kernel/net.c        colinux-slirp-net-daemon -R
  ────────────────────              ──────────────────        ───────────────────────────
  ndo_start_xmit ─► TX ring ──►  CONET_DUMP / CONET_TAKE ──►  slirp_input
                                 (walk the guest's tables                │
  RX kthread  ◄── RX ring  ◄───   under net_lock)          ◄── slirp_output ──► winsock
```

The host reads the rings by walking the guest's page tables; the base address
is retired under a lock before the address space is freed. NAT is provided by
coLinux's vendored slirp, run in a process limited to a 2 GB address space so
that its 32-bit queue links can hold pointers. `-r tcp:2222:22` redirects a
host port into the guest. The image runs sshd, which is useful for diagnosing
graphical problems.

### Timers

The guest has no timer hardware; it takes time from the host clock.
Previously this was a stream of 1 kHz periodic ticks generated at cooperative
boundaries, which rounded every sleep, fence wait and present in the guest to
a millisecond in each direction. A `nanosleep(50 µs)` took 1937 µs.

The clockevent is now oneshot. `set_next_event` writes the deadline, in the
host's 100 ns monotonic units, into a per-vCPU slot that the host reads. The
monitor limits its idle wait to the nearest deadline and wakes on a
high-resolution kernel timer. Guest sleeps now have a median of 567 µs
instead of 1937 µs. 1 ms and 5 ms sleeps land closer to their deadlines than
a Windows user-mode wait on the same machine (1580 µs vs 1996 µs, 5687 vs
5996), because the idle loop can re-arm after an early wake and a single
user-mode wait cannot.

Two details are required for correctness:

- The published deadline is level-triggered; firing does not clear it.
  `tick_nohz` skips reprogramming when it believes the device still holds the
  same expiry. A device that cleared the deadline on fire would never fire
  again, and the guest would freeze silently, because the watchdogs that
  would report the problem also need time to advance.
- The host reads the deadline slot through the guest's CR3, not the loader's
  page tables. The loader's tables no longer describe the guest after it
  switches to its own kernel tables. This bug broke the first implementation
  of the cooperative timer; `co_kload_host_ptr` has a warning comment about
  it.

Windows sets the lower bound for accuracy. A high-resolution waitable timer
with zero coalescing tolerance, set for 100 µs, wakes after 487-586 µs on
this machine; `tools/timerfloor.c` reproduces this. The guest cannot be more
accurate than the host's interrupt, so the design targets that bound. The
change also improved graphics: `glxgears` went from 579-721 fps to 840-855
fps with no changes to the graphics code.

### Graphics

The guest has a virtio-gpu device, render-only. The transport uses no traps.
MMIO transports depend on a store to a fake register trapping; here, every
register is a normal store into a structure in guest RAM, and the kick is a
counter that `cogpu-daemon.exe` polls from another core. The submission path
contains no ioctl and no world switch. 2000 context round trips from a guest
client measure a 3 µs median.

```
  guest: Mesa/virgl ─► virtio rings in guest RAM ◄── cogpu-daemon.exe
         /dev/dri/renderD128                          │ (persistent user-mode
                                                      │  windows onto guest RAM)
                                                      ▼
                                            virglrenderer ─► WGL ─► the card
```

The daemon reaches guest RAM through its view of the shared section that
backs it: a guest physical address is an offset, resolution is one add, and
every byte of the guest is reachable from any thread at zero cost -- so it
parses requests and writes replies in place, and a resource's whole backing
arrives as a single iovec because the view is linear in guest-physical
space. A resource's backing list of {guest physical
address, length} pairs becomes iovecs pointing into the guest's pages, so the
command path performs no copies. virglrenderer, cross-built for mingw with a
WGL winsys, replays the guest's GL on the host card. The guest reports
`virgl (GeForce GT 730/PCIe/SSE2)` with GL 4.2. Indirect GLX previously gave
GL 1.4 in software.

Both APIs share one presenter, and both are enabled by default.
`--present-r2` on `cogpu-daemon.exe` enables OpenGL and Vulkan;
`--no-present` disables both.

![glxgears and the LunarG Vulkan cube running at the same time as native
windows on Windows 10, beside the guest's kernel
log](doc/img/glxvksupport.png)

#### OpenGL

Presentation is CoPresent. VirtualGL and the `moco-gl` wrapper have been
removed. A Mesa 26.1.6 build (`libGL`, `libEGL`, `libGLESv2`, Gallium and
`libgbm`, in both 64-bit and 32-bit) is installed as the system GL driver. It
uses Mesa's upstream DRI3 loader, opens `/dev/dri/renderD128`, and keeps the
normal DRI images, buffer queue, buffer age and native fences. Only the final
presentation step is redirected: the client emits `VIRGL_CCMD_MOCO_PRESENT`
in the command stream it already renders through, and cogpu displays the
texture that virglrenderer already owns on the Windows GPU. VcXsrv remains
the unmodified window and input layer.

Because presentation is carried in the command stream, it goes through the
render node the client already has open. No socket, extra device or
filesystem share is needed, so sandboxed clients work without configuration.
The earlier version used a broker socket under `/run`, which sandboxes cannot
see; Firefox's content processes and Steam's pressure-vessel container
silently fell back to software rendering while unsandboxed clients worked.
The broker, its pinned ring, its dma-buf export and its root-only pagemap
requirement have been deleted.

Measured on the GT 730, with the broker stopped and its socket deleted to
confirm it is unused: 543 fps (64-bit) and 602 fps (32-bit) on
`tools/glbench` at 1280x720, and 550 fps when booting with no flags, as the
desktop shortcut does. Steam's log reports `MoCo DRI3: direct
virgl/CoPresent active` for three drawables from inside its container.
Firefox renders directly and stays interactive.

`glxgears` at its default size measures 840-855 fps over three runs. It
previously measured 579-721 fps. The improvement came from the timer change,
not the GL path: with oneshot clockevents, fence waits, presents and sleeps
are no longer rounded to a millisecond (see [Timers](#timers)). The host
measures about 1300 fps for the same load, so the guest reaches about two
thirds of native performance. The remaining gap is dominated by
geometry-heavy loads rather than per-frame overhead.

The staged acceptance gates are in
[`doc/direct-presentation`](doc/direct-presentation). The checksum-pinned
Mesa patch and the rebuild/rollback procedure are in
[`doc/building-copresent`](doc/building-copresent).

#### Vulkan

The guest has Vulkan through Venus, running on the host's Vulkan driver.
Mesa's `libvulkan_virtio` (both ABIs, installed as an ICD with an absolute
`library_path`) serialises the guest's Vulkan calls. virglrenderer's `vkr`
decoder, cross-built for mingw with the WINQ Windows patches and running its
render server as in-process worker threads, replays them on the host driver.
`vulkaninfo` in the guest reports
`Virtio-GPU Venus (NVIDIA GeForce GT 730)`, `DRIVER_ID_MESA_VENUS`, as a
DISCRETE device.

```
  guest: Vulkan app ─► Mesa venus ICD ─► virtio rings ◄── cogpu-daemon.exe
         /dev/dri/renderD128            + MOCO_PRESENT      │  vkr decoder
                                          ioctl             ▼
                                                  the host's Vulkan driver
```

Host-visible memory needs special handling in a cooperative guest. A Vulkan
allocation that the application maps must be host memory that appears in the
guest's physical address space, and there is no BAR or ReBAR to put it in.
The p2m therefore has a window arena above guest RAM, in address space that
belongs to no e820 range, and `KWINDOW_AT` maps host pages into a
caller-chosen slot there. `vkMapMemory` returns a pointer the guest writes at
4.6-4.8 GiB/s, which matches a native `memcpy` on this machine (its DRAM
limit). No copies are involved.

Presentation reuses the OpenGL presenter. The guest issues
`DRM_IOCTL_VIRTGPU_MOCO_PRESENT` on the render node it already renders
through, so sandboxes need no configuration, as with GL. The daemon uploads
the frame into a per-window virgl texture, and the same overlay draws it over
the same VcXsrv window. X keeps window management; only the pixels bypass it.
When a Vulkan client's context is destroyed, its bridge is released and its
last frame is removed from the screen.

`vkcube` runs at a steady 368 fps. An offscreen clear-and-readback verifies
byte-exact output (262144 pixels, zero wrong). GL and Vulkan coexist: gears
and the cube together, two Vulkan clients together, and one client exiting
while the other continues presenting were all verified with zero DRM errors.
The screenshot above shows this configuration.

#### Direct3D, through Wine and DXVK

Wine 11.14 with DXVK runs Direct3D 11 in the guest. The application's D3D
calls become Vulkan, Venus carries them to the host driver, and frames return
through the same presenter as everything else. Direct3D support was the
motivation for the Vulkan work.

![Unigine Heaven 4.0 rendering in Direct3D 11 as a native Windows window, with
the guest's kernel log beside it](doc/img/uniginedxvkwine.png)

Unigine Heaven 4.0, a 2013 D3D11 benchmark, runs at 26 fps at 1024x768, low
quality, tessellation off. A minimal D3D11 frame loop (`tools/d3dbench.c`,
clear and present, no geometry) measures 304 fps through the same stack,
which measures the path's overhead rather than the card.

Three version requirements apply:

- **DXVK 1.10.3, not 2.x or 3.x.** NVIDIA caps Kepler at Vulkan 1.2, Venus
  reports that version, and DXVK 2.0 and later require Vulkan 1.3. Newer
  releases enumerate the GT 730, skip it as unsupported, and fail with no
  adapters.
- **`d3dcompiler_42` from `winetricks`.** Heaven imports that DLL. Wine's
  builtin HLSL compiler rejects its 2013-era `half` types with 2396
  `E5017: not yet implemented` errors, which shows as a black scene with a
  working HUD, because only the simple shaders compile.
- **All Present-extension calls gated.** VcXsrv has no Present extension, and
  libxcb closes the connection when a stub call is made for a missing
  extension. Three such calls existed. The last was
  `xcb_register_for_special_xge`, which a search for `xcb_present_*` does not
  find; it killed each client one frame after its swapchain came up.

Known limits: presents are fire-and-forget, like the GL path, so FIFO is not
throttled and a client renders as fast as the card allows rather than at a
refresh rate. There is also no native baseline for the 26 fps figure yet;
running the same binary through `wined3d` (D3D11 to OpenGL to virgl) and on
the host directly would separate the card's cost from the stack's. lavapipe
remains installed as the software fallback for hosts whose drivers cannot
serve Venus.

## Status

Working, verified on hardware (Lenovo ThinkCentre M92p, i5-3470) under
Windows XP x64, Windows 7 x64, Windows 8.1 x64 and Windows 10 x64. See
[Windows 8 and 8.1](#windows-8-and-81) and [Windows 10](#windows-10) for the
requirements on those hosts.

- Boots Manjaro with systemd to multi-user target, no failed units, from an
  image the tree builds (`tools/mkmanjarorootfs.sh`)
- ext4 root over a cooperative block device; ring-3 processes
- Interactive terminal over TCP (hvc console, `/dev/hvc0`)
- KDE applications as native Windows windows, rootless, over a host-side X
  server
- Preemption: the host interrupts a busy-looping task and time advances at
  real speed
- Cooperative SMP: the installed launcher requests `--cpus 2`. Two Linux
  processors run concurrently on distinct host logical processors with
  per-vCPU switch state, passage pages, timers and posted-IPI delivery.
  Linux receives their real physical-core/SMT relationship
- Networking: guest ethernet device, host NAT, static address via
  `systemd-networkd`. pacman installs a 791-package desktop over HTTPS at
  16 MB/s
- Hardware-accelerated OpenGL on the host card, enabled by default, with no
  wrapper: virtio-gpu in the guest, virglrenderer on the host, and CoPresent
  carrying resource identity (not finished frames) in the guest's virgl
  command stream. Unmodified GLX and EGL programs of both ABIs load Mesa's
  DRI3 provider and report direct virgl; 543 fps (64-bit) and 602 fps
  (32-bit) on `tools/glbench` at 1280x720 on a GT 730
- Hardware-accelerated Vulkan on the host card, through Venus: the guest's
  Mesa `libvulkan_virtio` (both ABIs) serialises to virglrenderer's `vkr`
  decoder, which runs on the host's Vulkan driver. `vulkaninfo` reports
  `Virtio-GPU Venus (NVIDIA GeForce GT 730)` as a discrete device,
  host-visible memory maps at DRAM speed (4.6-4.8 GiB/s) through a window
  arena in the p2m with no BAR involved, and `vkcube` presents into its own
  native window at 368 fps beside an OpenGL client
- Direct3D 11, through Wine 11.14 and DXVK 1.10.3 on top of that Vulkan:
  Unigine Heaven 4.0 renders at 26 fps (1024x768, low, no tessellation) as a
  native window, and `tools/d3dbench.c` measures 304 fps for the path itself
- Sandboxed clients work without configuration in both APIs, because
  presentation needs only the render node they already have: Firefox renders
  directly and stays interactive, and Steam logs direct CoPresent from inside
  its pressure-vessel container
- Inbound port redirects (`-r tcp:2222:22` reaches the guest's sshd)
- 32-bit binaries (the guest keeps its own `int $0x80` gate)
- Landlock and user namespaces (required by pacman 7 and modern sandboxes)
- Virtual time from the host clock; clean shutdown; stop-on-demand from
  another process
- Hardware interrupts are forwarded across the switch and replayed into
  Windows' live IDT
- Asynchronous block I/O on kernel worker threads (one queue and worker per
  unit); console latency stays at 16–125 ms through a full package install.
  `--sync-cobd` restores the synchronous path for comparison
- Self-installing: `mocolinux-setup.exe` installs the driver, daemons,
  kernel, the GPU daemon with virglrenderer, X server and launchers, then
  boots Linux and builds a Manjaro system on a fresh image over the network
- The authoritative complete guest-side Linux 7.1.5 diff is
  [`patch/7.1.5/current-tree-snapshot.diff`](patch/7.1.5/current-tree-snapshot.diff).
  It is regenerated against the released tarball and checked by applying and
  reverse-applying it with zero fuzz and an exact tree comparison

Not yet done:

- SMP beyond two vCPUs, and long-duration desktop/Steam testing. The
  two-vCPU path is functional and benchmarked. Oneshot clock events shipped;
  [Timers](#timers) has the numbers
- The coLinux message layer (`co_monitor_t`, queues, reactor). Upstream's
  `cocon`/`conet` consoles and devices, including `colinux-console-nt`,
  cannot attach
- DHCP in the guest (static address only)
- Native-speed presentation. Direct presentation is the packaged default for
  both ABIs, and the oneshot clockevents reduced per-frame synchronisation
  cost from about 2.5 ms to 1.2-1.5 ms; `glxgears` now reaches about two
  thirds of the host's figure instead of half. Geometry-heavy loads remain
  slower, and an asynchronous multi-buffered release path is the next task
- Vsync. Presents are fire-and-forget in both APIs, so `FIFO` is not
  throttled and a client renders as fast as the card allows. No workload has
  needed it so far; a game will
- Proton, and games. Wine and DXVK work: Direct3D 11 runs on the host card
  and Unigine Heaven renders through it (see
  [Direct3D, through Wine and DXVK](#direct3d-through-wine-and-dxvk)). But no
  game has been launched yet, Proton has never been installed here
  (GE-Proton standalone plus `umu-launcher` is the route that does not need
  Steam), and D3D9 and D3D12 are untested; only D3D11 has run. Zink over
  Venus is also untested
- Steam's client. It launches and its helpers run, but the storefront UI
  fails as described below, and a 5000-fish WebGL load killed its GPU process
  on this hardware
- Steam's storefront UI. Its CEF helper fails to create a browser window
  against VcXsrv 1.14, even though GPU initialisation is clean, a Vulkan
  device is present (now an accelerated one, not just lavapipe) and CoPresent
  is active for its other drawables. The same failure occurs with this stack
  disabled, so it is a CEF/X-server problem, not a graphics one.
  `XFree86-VidModeExtension` is also absent and cannot be enabled on the
  pinned XP-compatible server

### Cooperative SMP milestone

To the project's knowledge, MoCoLinux is the first coLinux-style cooperative
kernel to run a working SMP Linux guest on Windows 10. This is guest SMP, not
a uniprocessor guest on an SMP host: Linux reports CPUs 0 and 1 online and
schedules work on both simultaneously. Upstream coLinux
[documented that its guest could use only one CPU](https://colinux.fandom.com/wiki/FAQ#Q39._Does_coLinux_take_advantage_of_dual_core_processors?)
and its changelog records that the daemon was
[pinned to the first processor while SMP remained unresolved](https://colinux.sourceforge.net/?section=changelog).

The first validated two-vCPU run was recorded on 2026-08-09 on the
ThinkCentre M92p (Core i5-3470, four physical cores, no SMT), Windows 10 IoT
Enterprise LTSC 21H2 build 19044, and Linux 7.1.5. Each result compares the
same running guest with one worker against two; higher is better:

| Workload | 1 vCPU | 2 vCPUs | Gain |
| --- | ---: | ---: | ---: |
| `sysbench cpu --cpu-max-prime=20000`, 10 s | 345.23 events/s | 682.96 events/s | **1.978x** |
| `openssl speed -evp sha256`, 8192-byte blocks | 320,064.72 kB/s | 634,843.21 kB/s | **1.983x** |
| `sysbench memory` sequential write, 1 MiB blocks | 17,631.70 MiB/s | 35,828.47 MiB/s | **2.032x** |
| `sysbench memory` sequential read, 1 MiB blocks | 21,729.63 MiB/s | 42,494.90 MiB/s | **1.956x** |

The memory figures are a hot-buffer/cache-path scaling test, not a
measurement of the M92p's raw DRAM bandwidth. Stability and scheduling checks
also passed:

- A 20-second two-worker `stress-ng` matrix run accumulated 39.65 CPU-seconds
  and passed both workers, showing that both vCPUs stayed busy for the full
  run.
- Four oversubscribed context-switch workers completed 3,275,146 operations
  in 10.02 seconds (327,349/s), with no failed or untrustworthy metrics.
- A lock/yield-heavy two-thread sysbench run completed 35,432 events in
  exactly 10 seconds, with 0.56 ms average latency and balanced workers.
- Firefox, the workload that previously drove both processors into a hard
  deadlock, loaded pages normally after the posted-IPI polling fix. The guest
  remained reachable over SSH after every test, and a post-stress two-second
  sleep measured 2.023 seconds.

The mechanism is similar to Xen PV's, without emulating an APIC. A posted
per-vCPU bitmap carries the message, and a targeted Windows DPC is the
doorbell that interrupts a running target core. A separate targeted 100 Hz
deadline guarantees that a userspace-bound vCPU crosses to the monitor, where
a guarded cooperative interrupt entry batches the guest's 1 ms clock events.
Kernel spin waits poll posted vectors without allowing re-entry from NMI,
hardirq or virtual-interrupt-off regions. CPU scaling works for two vCPUs;
presentation frame rate remains limited by the separate GPU-to-X transport
described above.

Guest APIC routing IDs are synthetic, because no APIC hardware is addressed.
Package, core and SMT topology comes from CPUID on each pinned host logical
processor, so Linux's sibling masks describe the placement Windows actually
supplied.

The same tested build uses the [FragRAM](#fragram) path described above. A
2048 MiB guest boot has been verified. 128 GB is the supported configuration
maximum, not a tested configuration.

## Layout

| Path | What |
| --- | --- |
| `src/colinux/arch/x86_64/switch.c` | the world switch, fault stubs, monitor loop |
| `src/colinux/kernel/kload.c` | loads vmlinux into host memory, builds the guest's tables |
| `src/colinux/kernel/cobd.c` | cooperative block device, host half |
| `src/colinux/kernel/console.c` | terminal rings, host half |
| `src/colinux/kernel/net.c` | network rings, host half: read, consume, inject |
| `src/colinux/kernel/vgpu.c` | virtio-gpu transport, host half: publish, retire, idle gate |
| `src/colinux/os/winnt/user/cogpu-daemon/` | the GPU device: vring service, virglrenderer, the WGL winsys |
| `src/colinux/user/copresent/` | standalone fenced producer used to test presentation without a GL application |
| `src/colinux/user/conet_ring.c` | the ring format and its decoder, shared by both readers |
| `src/colinux/user/slirp/` | vendored slirp, with its Win64 fixes |
| `src/colinux/user/elf_load.c` | the daemon: ELF loading, symbol resolution, boot |
| `patch/7.1.5/current-tree-snapshot.diff` | the complete guest-side kernel patch, including cooperative SMP/IPIs, conet, async COBD and the trapless virtio-GPU transport |
| `patch/7.1.5/{async-cobd-src,vgpu-src}/` | standalone development copies of the guest device sources already folded into the cumulative patch |
| `patch/mesa-26.1.6/moco-copresent.diff` | the complete zero-fuzz Mesa GLX/CoPresent delta against the checksum-pinned release |
| `tools/mkmanjarorootfs.sh` | builds the Manjaro desktop image |
| `tools/mkrootfs.sh` | builds the minimal BusyBox bring-up image |
| `tools/coterm.py` | client for the guest's console port |
| `tools/shot.cs` | screenshots the host desktop, compiled and run on the target machine |
| `tools/decode-minidump.py` | attributes a bugcheck's stack to this driver |
| `tools/pe-clear-laa.py` | limits the slirp daemon to 2 GB of address space |
| `doc/runbook` | from bare machine to desktop, and from host to installed Manjaro |
| `doc/porting-x86_64` | design notes for the port |
| `doc/direct-presentation` | staged plan and acceptance gates for the presentation path |
| `doc/building-copresent` | exact host and dual-ABI DRI3 Mesa build; isolated test, system selection and rollback |
| `doc/building-release` | end to end: every product, the build order, known problems, cutting a release |
| `doc/building-modern` | the Windows driver and daemons in detail |

## Building

[`doc/building-release`](doc/building-release) is the end-to-end recipe: what
each of the six products is, which script builds it, the required build
order, and the known problems in each step. Start there.

Building requires a cross toolchain (`mingw-w64-gcc`, `binutils`),
`osslsigncode` for the driver signature, and two kernel trees. The Windows
side builds against 2.6.33 headers (the passage-page ABI is a header inside
the guest kernel tree); the guest kernel is 7.1.5. The 2.6.33 tree is used
for headers only. Nothing in it is built or run.

The host and guest halves are one matched interface and should be built from
the same commit. `linux.sys` and the host daemons come from `src/colinux/`.
The matching `vmlinux` comes from applying
`patch/7.1.5/current-tree-snapshot.diff` to pristine Linux 7.1.5. Keeping
both source halves in the repository makes a tested driver/kernel pair
reproducible.

The Windows side, driver through release, is one script:

```sh
tools/build.sh                    # build, sign, stage into dist-x64/
tools/build.sh --release 0.5.0    # ...and assemble release/MoCoLinux-0.5.0/
```

`build.sh` compiles the driver and daemons, builds the installer, signs the
driver with the test certificate (NT 6 and later refuse the unsigned image
the linker emits), and stages a file set it checks for completeness: the
binaries, the `virglrenderer`/`libepoxy` DLLs the GPU daemon loads, the
launchers, and the guest-side `coxwire` shim. `--release` then assembles a
directory whose manifest is read from the installer's own payload list. It
refuses to finish if anything is missing, links the Universal CRT (which does
not start on XP), or depends on a DLL the release does not carry.

The script finds the 2.6.33 header tree and the `download/prefix-mingw` cross
prefix at their default locations; `COLINUX_TARGET_KERNEL_SOURCE` and
`COLINUX_VIRGL_PREFIX` override them. It does not build `vmlinux` or
`root-arch.img`, because both are slow to build and change rarely, but it
reports whether the staged copies are present.

CoPresent adds separately built Linux Mesa components, which `build.sh`
cannot produce as part of a Windows build. Run
`tools/build-mesa-copresent.sh` for the matched patched GL stack (it builds
both the 64-bit and 32-bit ABI) and `tools/build-copresent-guest.sh` for the
standalone test producer. The latter verifies the official Mesa 26.1.6
SHA-256 and applies the repository patch with zero fuzz. Read
[`doc/building-copresent`](doc/building-copresent) before enabling the
development stack system-wide; it also contains the non-destructive rollback
procedure.

To drive `comake` directly, or for the underlying recipe and every
environment variable, see `doc/building-modern`. The guest kernel is built
separately: apply `patch/7.1.5/current-tree-snapshot.diff` to a 7.1.5 tree
and build `vmlinux` normally. The patch defaults `CONFIG_VIRTIO_COLINUX` on;
verify that it is built in, not a module. `CONFIG_KASAN` must be off, because
the host's allocation lands in PML4 slot 501, inside Linux's KASAN shadow
region.

## Running

On the target machine, run `mocolinux-setup.exe`. It installs the driver,
daemons, kernel, VcXsrv and launchers, creates desktop shortcuts and a logon
entry, then boots Linux and builds a Manjaro system on a fresh disk image
over the network (about 15 minutes and 2.5 GB of downloads; the release
carries no built desktop, only the seed image).

![The installer's desktop shortcuts, and four Linux applications started from them](doc/img/moco-desktop.png)

For development, run the components manually. This is also what the installer
runs:

```
colinux-daemon.exe --boot-kernel vmlinux --max-switches none \
                   --cpus 2 \
                   --cobd0 \??\C:\path\to\root.img
colinux-daemon.exe --console 2323          (a second process: a terminal)
colinux-slirp-net-daemon.exe -R            (a third: NAT for the guest)
cogpu-daemon.exe                           (a fourth: the guest's GPU)
colinux-daemon.exe --run konsole           (start one app in a running guest)
```

- `--mem MB` sets usable pseudo RAM (default 1024, maximum 131072). It is a
  target: e820 describes what the scattered nonpaged-pool backing actually
  supplied, and falling short is reported rather than fatal. See
  [FragRAM](#fragram) for how the backing is translated.
- `--cpus N` sets the guest's processor count (the daemon default is one; the
  installed launcher requests two). Every vCPU needs a distinct host logical
  processor, and the driver reserves capacity for Windows and the GPU daemon
  instead of placing two guest processors on the same one.
- The image ships `10-eth0.network` with slirp's fixed layout and
  `systemd-networkd` enabled, so the guest configures its own network at
  boot.
- For a desktop, start an X server on the Windows side in multiwindow mode
  (`vcxsrv :0 -multiwindow -ac`, or `dist-x64/xstart.bat`) and run X clients
  in the guest. `DISPLAY=10.0.2.2:0` is already set in the image's
  environment. GL applications need no wrapper, because CoPresent is the
  system GL driver. To confirm the card is being used, check that
  `glxinfo | grep renderer` reports `virgl`.
- `tools/mkrootfs.sh` builds the minimal BusyBox image (`--init /bin/sh`).
  `tools/mkmanjarorootfs.sh` builds the Manjaro desktop image (needs a Linux
  host with `pacman` and `e2fsprogs`; it handles the alpm skeleton, package
  install, keyring, `ldconfig`, network unit, fonts and `lib32` packages).
- The daemon processes are separate on purpose: the one running the guest
  sits inside a single ioctl for the guest's lifetime and cannot also
  service a socket. End a session with `colinux-daemon.exe --stop`, a
  `poweroff` in the guest, or by unloading the driver.
- `--net-dump` prints the guest's network rings and decodes their frames
  read-only; `--net-take` does the same and consumes them.

## Windows 8 and 8.1

Supported and verified on hardware, with everything the XP and 7 hosts have,
including hardware-accelerated OpenGL on the host card (screenshot above).

Two of the following also apply to Windows 7 (NT 6.1): test signing and UAC.
The other two are new in 8.1. The installer handles all four and refuses to
continue, rather than half-install, if it cannot:

- **Secure Boot must be off** *(8.1)*. A test-signed driver cannot load with
  Secure Boot on, and nothing later in the install can work around that. The
  suitability check stops there and prints the firmware steps.
- **Test signing must be on** *(7 and 8.1)*. The installer enables it and
  reboots. The driver service is created after that reboot, not before,
  because a service created while signing is still enforced can never start.
- **No hypervisor** *(8.1)*. Hyper-V, VBS/HVCI or a running VM means the
  guest is not at ring 0 on real hardware. Checked via CPUID leaf 1 ECX
  bit 31.
- **Elevation** *(7 and 8.1)*. `mocolinux-setup.exe` carries a
  `requireAdministrator` manifest. The logon entry cannot, because UAC runs
  Startup-folder shortcuts with the filtered token, so `moco-boot.vbs`
  re-launches itself through the `runas` verb on NT 6 and later. Without
  this, the X server came up and the desktop looked like a working install,
  but `sc start CoLinuxDriver` was silently refused and the Linux half was
  missing.

Two smaller differences from XP:

- **No telnet client.** It has been an optional Windows feature, off by
  default, since Vista. The terminal shortcut goes through `moco-term.bat`,
  which offers to enable it and otherwise says which feature box to tick.
- **User-space mappings land above 4 GB.** XP and 7 place the driver's MDL
  mappings of guest RAM low; 8.1 does not. `co_manager_kmap` reported those
  addresses through an `unsigned long`, which is 32 bits under LLP64, so
  every slice above 4 GB lost its top half and the GPU daemon dereferenced
  half a pointer. This is fixed. It is noted here because it is the one place
  where the host version changed behaviour rather than policy, and because
  the same truncation bug class has also affected `vm_ptr_t`, the host ISR
  address and `snprintf %p` in this tree.

Windows 8 (6.2) shares all of the above and is expected to work, but has not
been tested on hardware.

## Windows 10

Supported and verified on hardware (Windows 10 IoT Enterprise LTSC 21H2,
build 19044) with GPU acceleration and a two-vCPU SMP guest. Everything the
earlier hosts have works. Two world-switch fixes are specific to this
version:

- **CR4 is loaded before CR3 at the switch.** Windows 10 with KVA Shadow sets
  `CR4.PCIDE`, which makes bits 11:0 of CR3 a PCID and bit 63 the NOFLUSH
  flag. The guest clears PCIDE (`nopcid`). Writing the host's PCID-encoded
  CR3 while PCIDE is still off writes reserved bits, which raises `#GP` in
  the passage page with IF clear, triple-faults, and freezes the machine. The
  switch now loads the entering side's CR4 (with PGE cleared for the TLB
  flush) before writing CR3.
- **`MSR_TSC_AUX` (0xC0000103) is saved and restored per switch.** Windows 10
  stores the logical processor number there for `RDTSCP`, and PatchGuard
  verifies it has not been modified. The guest's `cpu_init()` overwrites it.
  Without the save/restore, PatchGuard bugchecks with `0x109`
  (`CRITICAL_STRUCTURE_CORRUPTION`, arg4 = 0x7, arg3 = 0xC0000103) on its
  randomized timer, minutes after boot. Confirmed from a minidump. The value
  is saved in the existing `temp_cr3` slot to avoid growing the shared state
  struct.

Neither fix affects XP, 7 or 8.1, where PCIDE is not set and TSC_AUX is not
checked, so the same binary runs on all four hosts.

Requirements are the same as 8.1 (test signing, no hypervisor, elevation),
with no additions. Windows 10 enables KVA Shadow by default on affected
hardware; the switch handles it and it does not need to be disabled.

## Porting notes

These constraints shaped the port. They are recorded in full in
`doc/porting-x86_64` and the commit history:

- A free-running guest keeps the real IF enabled. Hardware interrupts vector
  through the guest's IDT into a stub, world-switch back, and are replayed
  into Windows' live IDT. Relying on voluntary yields would leave the core
  unable to receive its clock interrupt or TLB-shootdown IPIs.
- The real IF must be clear during the switch itself. Between the CR3 write
  and the IDTR load, a stale IDTR points into an unmapped table, and any
  interrupt in that window triple-faults with no dump.
- Anything the switch restores per crossing it must also save per crossing,
  including `GS_BASE` (Windows' KPCR moves with its scheduler).
- PatchGuard (introduced in XP x64) checksums the host's GDT/IDT/TSS on a
  randomized timer. Any standing modification bugchecks `0x109` minutes
  later, with none of this driver's code on the stack. Host structures are
  restored exactly.
- Vector `0x80` is carved out of the host-owned 32–255 range. It is Linux's
  32-bit syscall gate (DPL 3), not a machine interrupt.
- The guest's IF is virtual. Interrupt gates and `SYSCALL` clear the real
  flag, so `local_irq_enable()` must restore the hardware flag too.
- After boot handoff the guest runs on `init_top_pgt`. `cpu_entry_area` and
  the vmalloc region (with `CONFIG_VMAP_STACK`, most task stacks) exist only
  in the guest's tables, so host-side reads must walk the guest's live PML4.
- Ticks must be injected even when the guest never enters its kernel.
  Otherwise a busy userspace loop owns the CPU forever, and every watchdog
  that would report it depends on the tick that stopped.
- slirp's wire-overlay structs require `-mno-ms-bitfields` (otherwise
  `struct ip` is 24 bytes and nothing parses). Its `errno` is
  `WSAGetLastError()`, so would-block checks must compare against
  `WSAEWOULDBLOCK`. Six `_Static_assert`s pin the structure sizes.
- NTFS zero-fills sparse tails synchronously inside a write.
  `FileValidDataLengthInformation` removes minutes-long first writes on
  large images.
- The host's pointer into a guest's rings is retired under a lock inside
  `co_kload_free`, where no caller can forget it. Pended IRPs are read and
  cleared under the cancel spin lock.
- `%p` in the tree's own `snprintf` must fetch 64 bits under LLP64, or every
  logged pointer prints with its top half missing.

## Licence

GPL v2, as coLinux was. See `LICENSE`.

Original coLinux by Dan Aloni and contributors; see `CREDITS` and `THANKS`.
