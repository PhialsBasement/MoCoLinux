/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#ifndef __COLINUX_COMMON_IOCTL_H__
#define __COLINUX_COMMON_IOCTL_H__

#include "common.h"

#include <colinux/common/import.h>
#include <colinux/common/config.h>


typedef enum {
	CO_MANAGER_IOCTL_BASE=0x10,

	CO_MANAGER_IOCTL_CREATE,
	CO_MANAGER_IOCTL_MONITOR,
	CO_MANAGER_IOCTL_STATUS,
	CO_MANAGER_IOCTL_DEBUG,
	CO_MANAGER_IOCTL_DEBUG_READER,
	CO_MANAGER_IOCTL_DEBUG_LEVELS,
	CO_MANAGER_IOCTL_INFO,
	CO_MANAGER_IOCTL_ATTACH,
	CO_MANAGER_IOCTL_MONITOR_LIST,
	CO_MANAGER_IOCTL_PROBE_VA,
	CO_MANAGER_IOCTL_PROBE_PASSAGE,
	CO_MANAGER_IOCTL_SAVE_STATE,
	CO_MANAGER_IOCTL_TEST_SWITCH,
	CO_MANAGER_IOCTL_TEST_ROUNDTRIP,
	CO_MANAGER_IOCTL_TEST_FAULT,
	CO_MANAGER_IOCTL_TEST_RESUME,
	CO_MANAGER_IOCTL_TEST_SPACE,
	CO_MANAGER_IOCTL_TEST_PAGEFAULT,
	CO_MANAGER_IOCTL_TEST_BADSTACK,
	CO_MANAGER_IOCTL_TEST_GUEST,
	CO_MANAGER_IOCTL_TEST_GUESTFAULT,
	CO_MANAGER_IOCTL_KLOAD_BEGIN,
	CO_MANAGER_IOCTL_KLOAD_CHUNK,
	CO_MANAGER_IOCTL_KLOAD_VERIFY,
	CO_MANAGER_IOCTL_KLOAD_ENTER,
	CO_MANAGER_IOCTL_KLOAD_END,
	CO_MANAGER_IOCTL_KCALL,
	CO_MANAGER_IOCTL_KBOOT,
	CO_MANAGER_IOCTL_KRAM,
	CO_MANAGER_IOCTL_KREAD,
	CO_MANAGER_IOCTL_COBD,
	CO_MANAGER_IOCTL_CONSOLE,
	CO_MANAGER_IOCTL_KSTOP,
	CO_MANAGER_IOCTL_CONET_DUMP,
	CO_MANAGER_IOCTL_CONET_TAKE,
	CO_MANAGER_IOCTL_CONET_PUT,
	CO_MANAGER_IOCTL_KMAP,
	CO_MANAGER_IOCTL_KUNMAP,
} co_manager_ioctl_t;

/*
 * This struct is mapped both in kernel space and userspace.
 */
typedef struct {
	unsigned long userspace_msgwait_count;
} co_monitor_user_kernel_shared_t;

/* interface for CO_MANAGER_IOCTL_CREATE: */
typedef struct {
	co_rc_t		    rc;
	co_symbols_import_t import;
	co_config_t	    config;
	co_info_t	    info;
	co_arch_info_t	    arch_info;
	unsigned long	    actual_memsize_used;
	void*		    shared_user_address;
	co_id_t		    id;
} co_manager_ioctl_create_t;

/*
 * ioctls()s under CO_MANAGER_IOCTL_MONITOR:
 */
typedef enum {
	CO_MONITOR_IOCTL_CLOSE,
	CO_MONITOR_IOCTL_LOAD_SECTION,
	CO_MONITOR_IOCTL_START,
	CO_MONITOR_IOCTL_RUN,
	CO_MONITOR_IOCTL_STATUS,
	CO_MONITOR_IOCTL_LOAD_INITRD,
	CO_MONITOR_IOCTL_GET_CONSOLE,  /* Get console dimentions and max buffer size */
	CO_MONITOR_IOCTL_GET_STATE,
	CO_MONITOR_IOCTL_RESET,
	CO_MONITOR_IOCTL_VIDEO_ATTACH, /* incomplete */
	CO_MONITOR_IOCTL_VIDEO_DETACH, /* incomplete */
	CO_MONITOR_IOCTL_CONET_BIND_ADAPTER,
	CO_MONITOR_IOCTL_CONET_UNBIND_ADAPTER
} co_monitor_ioctl_op_t;

/* interface for CO_MANAGER_IOCTL_MONITOR: */
typedef struct {
	co_rc_t			rc;
	co_monitor_ioctl_op_t	op;
	/*
	 * Zero-length, not a C99 flexible array: this struct is embedded as the
	 * leading member of the per-operation structs below, so a flexible array
	 * here is not at the end of the enclosing struct and GCC rejects it.
	 */
	char			extra_data[0];
} co_manager_ioctl_monitor_t;

/* interface for CO_MANAGER_IOCTL_STATUS: */
typedef struct {
	unsigned long	state; /* co_manager_state_t */
	unsigned long	reserved;
	int		monitors_count;
	int		periphery_api_version;
	int		linux_api_version;
	char		compile_time[28];
} co_manager_ioctl_status_t;

/* interface for CO_MANAGER_IOCTL_INFO: */
typedef struct {
	unsigned long long hostmem_usage_limit;
	unsigned long long hostmem_used;
} co_manager_ioctl_info_t;

#define CO_MANAGER_ATTACH_MAX_MODULES 0x10

/* interface for CO_MANAGER_IOCTL_ATTACH: */
typedef struct {
	co_rc_t		rc;
	co_id_t		id;
	int		num_modules;
	co_module_t	modules[CO_MANAGER_ATTACH_MAX_MODULES];
} co_manager_ioctl_attach_t;

/* interface for CO_MANAGER_IOCTL_DEBUG_READER: */
typedef struct {
	co_rc_t       rc;
	void*         user_buffer;
	unsigned long user_buffer_size;
	unsigned long filled;
} co_manager_ioctl_debug_reader_t;

#ifdef COLINUX_DEBUG
/* interface for CO_MANAGER_IOCTL_DEBUG_LEVELS: */
typedef struct {
	co_debug_levels_t levels;
	bool_t            modify;
} co_manager_ioctl_debug_levels_t;
#endif

/*
 * interface for CO_MANAGER_IOCTL_PROBE_VA
 *
 * Walks the host's own page tables for one virtual address and reports what is
 * mapped at each level. Used to answer a design question the x86-64 passage code
 * hinges on: whether the address window the guest wants is free in the host's
 * kernel address space too. If it is, the passage page can be mapped at the same
 * virtual address on both sides, which makes other_map zero and removes the need
 * for the passage code to relocate its own instruction pointer between mappings.
 *
 * Read-only. It maps each page-table page, reads one entry, and unmaps it.
 */
#define CO_PROBE_VA_LEVELS 4

typedef struct {
	co_rc_t		   rc;
	unsigned long long va;		/* in:  address to walk		*/
	unsigned long long cr3;		/* out: host CR3 at probe time	*/
	unsigned long long entry[CO_PROBE_VA_LEVELS];	/* out: PML4,PDPT,PD,PT	*/
	int		   levels_walked;	/* out: how far it got	*/
	int		   present;	/* out: PTRUE if fully mapped	*/
	int		   large_page;	/* out: stopped at a 2M/1G page	*/
	int		   supported;	/* out: PFALSE on architectures
					   without a 4-level walker	*/
} co_manager_ioctl_probe_va_t;

/*
 * interface for CO_MANAGER_IOCTL_PROBE_PASSAGE
 *
 * Allocates a passage page exactly as the monitor would, reports where the host
 * put it and what its own page tables say about it, then frees it. Two things are
 * being asked:
 *
 *  - the host virtual address, because we do not choose it. If the guest can map
 *    the page at the same address, other_map is zero and the switch never has to
 *    relocate its own instruction pointer -- and, more importantly, no host page
 *    table has to be modified to arrange it.
 *
 *  - whether the page is executable. The passage code runs from it. i386 deals
 *    with Windows marking such allocations NX by hand-clearing the bit in the
 *    host's live page tables (arch/i386/antinx.c), which is a four-level walk on
 *    x86-64 and precisely the page-table tampering to avoid. If NX comes back
 *    clear there is nothing to solve; if it is set, the allocator has to change
 *    before any switch assembly is worth writing.
 *
 * Allocates and frees. Executes nothing, writes no page tables, touches no CR3.
 */
#define CO_PROBE_ALLOC_MAX 4

typedef struct {
	char			    name[40];	/* out: allocator name		*/
	unsigned long long	    va;		/* out: host virtual address	*/
	unsigned long long	    pa;		/* out: physical address	*/
	int			    ok;		/* out: allocation succeeded	*/
	int			    used;	/* out: this is the one the passage
						   page actually allocates from	*/
	co_manager_ioctl_probe_va_t walk;	/* out: page-table walk of va	*/
} co_probe_alloc_result_t;

typedef struct {
	co_rc_t			rc;
	int			pages;	/* out: pages requested per attempt	*/
	int			count;	/* out: allocators reported		*/
	co_probe_alloc_result_t	result[CO_PROBE_ALLOC_MAX];
} co_manager_ioctl_probe_passage_t;

/*
 * interface for CO_MANAGER_IOCTL_SAVE_STATE
 *
 * Capture the host's CPU state into a co_arch_state_stack_t and hand it back.
 * Reads only. The save half of the world switch, exercised on its own so that
 * the state layout and the values the restore half must reproduce are known
 * before any of it is written.
 */
typedef struct {
	co_rc_t			rc;
	int			supported;
	int			restore;	/* in: also put the state back	*/
	co_arch_state_stack_t	state;
} co_manager_ioctl_save_state_t;

/*
 * interface for CO_MANAGER_IOCTL_TEST_SWITCH
 *
 * Change CR3 into an address space containing only the passage page, store a
 * sentinel from code executing there, and change back. Tests the one property the
 * whole passage design rests on: that execution continues across a CR3 write
 * because the code page is mapped at the same address in both address spaces.
 *
 * The full result struct lives in arch/switch.h; this carries a copy of it.
 */
typedef struct {
	co_rc_t	rc;
	int	supported;
	int	succeeded;
	unsigned long long passage_va;
	unsigned long long passage_pa;
	unsigned long long code_va;
	unsigned long long host_cr3;
	unsigned long long guest_cr3;
	unsigned long long expected;
	unsigned long long observed;
	unsigned long long guest_gdt;
	unsigned long long guest_idt;
	unsigned long long fault_handler;
	unsigned long long fault_rip;
	int		   faulted;
	unsigned long	   code_size;
	int		   iterations;
	unsigned long long counter;
	unsigned long long reg_accum;
	unsigned long long guest_stubs;
	unsigned long long guest_tss;
	unsigned long long ist_stack;
	unsigned long long guest_text;
	unsigned long long guest_stack;
	unsigned long	   tables;
	unsigned long long vector;
	unsigned long long error_code;
	unsigned long long cr2;
	int		   preflight_checked;
	int		   preflight_failed;
	int		   preflight_level;
	unsigned long long preflight_va;
} co_manager_ioctl_test_switch_t;

/* interface for the CO_MANAGER_IOCTL_KLOAD_* family */
typedef struct {
	co_rc_t		   rc;
	unsigned long long min_va;
	unsigned long long max_va;
} co_manager_ioctl_kload_begin_t;

typedef struct {
	co_rc_t		   rc;
	unsigned long long va;
	unsigned long	   size;
	int		   zero;	/* allocate the pages but write nothing */
	unsigned char	   data[0];
} co_manager_ioctl_kload_chunk_t;

typedef struct {
	co_rc_t		   rc;
	unsigned long long va;
	unsigned long	   size;
	unsigned long long checksum;
	unsigned long	   pages;
	unsigned long	   chunks;
	unsigned long	   tables;
} co_manager_ioctl_kload_verify_t;

/*
 * interface for CO_MANAGER_IOCTL_KREAD: read guest memory through the guest's
 * own page tables. What the daemon reads is what the guest would have read --
 * the same walk, the same frames -- which is what makes post-mortem structure
 * decoding (the printk ring, for one) trustworthy.
 */
typedef struct {
	co_rc_t		   rc;
	unsigned long long va;
	unsigned long	   size;
	unsigned char	   data[0];
} co_manager_ioctl_kread_t;

/*
 * interface for CO_MANAGER_IOCTL_KMAP / CO_MANAGER_IOCTL_KUNMAP: give a
 * user-mode process a direct, persistent view of the guest's RAM.
 *
 * This is the thing a virtual machine cannot do and this design gets for free.
 * Guest physical memory IS host physical memory here, so a host process does
 * not need to marshal anything across an address-space boundary -- it needs the
 * pages mapped once, and then a texture or a vertex buffer the guest wrote is
 * simply memory the daemon can read at memcpy speed. KREAD, by contrast, is an
 * ioctl and a page-table walk per call; it is right for a few kilobytes of
 * printk ring and hopeless for a gigabyte a second of GPU resources.
 *
 * SLICED, and not by preference. An MDL records its length in a CSHORT, so one
 * MDL can describe at most (32767 - sizeof(MDL)) / sizeof(PFN_NUMBER) pages --
 * on x86-64 that is (32767 - 48) / 8 = 4089 pages, a little under 16 MB. A
 * kload block is CO_KLOAD_CHUNK_BYTES (32 MB, 8192 pages), so one MDL per block
 * fails on every full-size block, deterministically, every time. 8 MB slices
 * are used instead: comfortably under the ceiling with room for a differently
 * sized MDL header, and a round number of them per block.
 *
 * (The design note this came from said 16 MB and 4089-versus-8185 pages. 8185
 * is the 32-bit figure, where PFN_NUMBER is four bytes; a 4096-page slice would
 * have overshot the real x86-64 ceiling by seven pages and failed on the first
 * call. The arithmetic is spelled out above so the next person can check it
 * rather than inherit it.)
 */
#define CO_KMAP_SLICE_BYTES	(8ULL << 20)
#define CO_KMAP_MAX_RANGES	256

typedef struct {
	unsigned long long pa;		/* guest physical == host physical */
	unsigned long long bytes;
	unsigned long long user_va;	/* where the caller may read it */
} co_kmap_range_t;

typedef struct {
	co_rc_t		   rc;
	unsigned long	   count;	/* out: ranges actually filled */
	unsigned long	   max_slice;	/* in: 0 means CO_KMAP_SLICE_BYTES */
	unsigned long long total_bytes;	/* out */
	co_kmap_range_t	   range[CO_KMAP_MAX_RANGES];
} co_manager_ioctl_kmap_t;

typedef struct {
	co_rc_t		   rc;
	unsigned long	   released;	/* out: slices unmapped */
} co_manager_ioctl_kunmap_t;

/*
 * interface for CO_MANAGER_IOCTL_COBD: attach a backing store to a unit.
 *
 * The path is opened by the driver rather than passed as a handle, because a
 * handle opened by the daemon belongs to a user-mode process and would go away
 * when it exits -- while the guest is still running inside the driver, holding
 * a filesystem mounted on it.
 *
 * How many units there are is part of this contract rather than the driver's
 * private business, because the daemon has to size its own argument list to
 * match: the driver refuses a unit past its array and the guest simply never
 * probes one, so a daemon that offered a fifth disk would fail in two places
 * that both look like the disk is missing. kernel/cobd.h and the guest's
 * drivers/block/cobd.c both take their bound from this number.
 */
#define CO_COBD_MAX_UNITS	4


typedef struct {
	co_rc_t		   rc;
	unsigned long	   unit;	/* in */
	char		   path[512];	/* in: NT object path */
	unsigned long long size;	/* out: bytes */
} co_manager_ioctl_cobd_t;

/*
 * interface for CO_MANAGER_IOCTL_CONSOLE: one turn of the terminal.
 *
 * Keystrokes in, screen output out, in a single call so a client is one poll
 * loop rather than two. It is deliberately callable while the guest is running
 * -- the monitor loop holds its own ioctl for as long as a hundred and twenty
 * seconds, and a terminal that could only be serviced between runs would not
 * be a terminal. Nothing here takes the manager lock; the two rings have one
 * writer each per direction, so the console client and the monitor loop touch
 * disjoint words.
 */
typedef struct {
	co_rc_t		   rc;
	unsigned long	   in_size;	/* in: keystrokes offered */
	unsigned long	   in_taken;	/* out: how many fitted */
	unsigned long	   out_size;	/* in: room for output */
	unsigned long	   out_len;	/* out: bytes produced */
	char		   in[512];
	char		   out[2048];
} co_manager_ioctl_console_t;

/*
 * interface for CO_MANAGER_IOCTL_CONET_DUMP: a read-only window into the
 * guest's TX network ring, live.
 *
 * Returns the four ring indices, fresh, plus `size` bytes of raw ring
 * starting at absolute position `start`. Nothing in the guest is written --
 * not even tx_tail, so the guest cannot observe the read and two identical
 * calls return identical bytes. Record parsing belongs to the caller. The
 * walk runs under the net lock, which is what makes a call racing the run's
 * teardown a clean NOT_FOUND instead of a walk through freed page tables.
 */
#define CO_CONET_DUMP_MAX 8192
typedef struct {
	co_rc_t		   rc;
	unsigned int	   tx_head;	/* out */
	unsigned int	   tx_tail;	/* out */
	unsigned int	   rx_head;	/* out */
	unsigned int	   rx_tail;	/* out */
	unsigned int	   start;	/* in: absolute tx ring position */
	unsigned int	   size;	/* in: bytes wanted; out: returned */
	unsigned char	   data[0];	/* out */
} co_manager_ioctl_conet_dump_t;

/*
 * interface for CO_MANAGER_IOCTL_CONET_TAKE: consume the TX ring by moving
 * tx_tail forward -- the host's one writable word there. The driver reads
 * head and tail fresh under its lock, refuses a tail that would move
 * backward or past the head (INVALID_PARAMETER, nothing written), and
 * returns all four indices as they stood after the write.
 */
typedef struct {
	co_rc_t		   rc;
	unsigned int	   new_tail;	/* in */
	unsigned int	   tx_head;	/* out */
	unsigned int	   tx_tail;	/* out (== new_tail on success) */
	unsigned int	   rx_head;	/* out */
	unsigned int	   rx_tail;	/* out */
} co_manager_ioctl_conet_take_t;

/*
 * interface for CO_MANAGER_IOCTL_CONET_PUT: deliver frames to the guest.
 *
 * Frames are appended to the RX ring and published by advancing rx_head once,
 * at the end. A full ring stops the batch where it ran out and reports how many
 * were taken, because the guest may be reading the record at rx_tail and must
 * never have it rewritten underneath it -- the caller keeps the rest and
 * retries.
 *
 * Many frames per call, and that is the whole point of the shape.
 *
 * This used to carry exactly one frame, which was right when the only writer
 * was --net-peer answering a ping at a time. Under a real download it is the
 * thing that decides throughput, because the cost is per call rather than per
 * byte: a METHOD_BUFFERED round trip, a non-paged staging buffer allocated and
 * freed by the I/O manager, net_lock taken and dropped, and a walk of the
 * guest's page tables. Measured at roughly a millisecond, which put a hard
 * ceiling near 925 frames a second:
 *
 *     1.39 MB/s / 1500 bytes = ~925 frames/s = ~925 ioctls/s
 *
 * and that is exactly what a guest download measured, from a mirror 5 ms away
 * and from one on another continent alike -- the give-away that the network was
 * never the limit. The TX direction never had this problem because co_net_fetch
 * has always pulled a window of the ring in one call.
 *
 * The layout is the ring's own: a 32-bit length, the frame, then padding to a
 * four-byte boundary, repeated `frames` times. Reusing it means the driver can
 * copy records into the ring without reformatting them, and there is one
 * description of a record in the tree rather than two that drift.
 */
#define CO_CONET_PUT_MAX	1514
/*
 * How much frame data one call may carry. 32 KB is about twenty-one full-sized
 * frames -- enough that the per-call cost stops dominating, and small enough
 * that the I/O manager's staging allocation stays modest. It is a
 * METHOD_BUFFERED buffer, so every byte of this is allocated and freed from
 * non-paged pool on every call that uses it.
 */
#define CO_CONET_PUT_BATCH	0x8000
typedef struct {
	co_rc_t		   rc;
	unsigned int	   frames;	/* in: records present in data */
	unsigned int	   size;	/* in: bytes of data used */
	unsigned int	   taken;	/* out: records actually appended */
	unsigned char	   data[0];	/* in: length-prefixed records */
} co_manager_ioctl_conet_put_t;

/*
 * interface for CO_MANAGER_IOCTL_KSTOP: end a running boot loop, now.
 *
 * Sets the same abort flag driver unload uses; the monitor loop checks it on
 * every crossing, so a guest that is still crossing -- and a live guest
 * always is, the host's clock arrives through it -- stops within
 * milliseconds. The run then ends through its ordinary exit: the daemon that
 * booted the guest prints its full report and calls KLOAD_END itself, so
 * nothing about teardown is new. This exists for a guest that is wedged but
 * alive, where the alternatives are the fifteen-minute deadline or the power
 * button, and the second of those has already cost stranded memory and a
 * walk to the machine.
 */
typedef struct {
	co_rc_t		   rc;
	int		   was_running;	/* out: a loop existed to stop */
} co_manager_ioctl_kstop_t;

/* interface for CO_MANAGER_IOCTL_KRAM: give the guest physical memory */
/*
 * Must equal CO_KLOAD_MAX_BLOCKS. The driver fills one entry per block it
 * actually allocated and the daemon turns each into an e820 range, so a value
 * below the driver's block cap would describe a guest smaller than the one that
 * exists -- memory allocated, mapped, and never mentioned to the kernel.
 */
#define CO_KRAM_MAX_RANGES 127
typedef struct {
	co_rc_t		   rc;
	unsigned long long ram_bytes;	/* in: total RAM target */
	unsigned long long text_va;	/* in */
	unsigned long long end_va;	/* in */
	unsigned long	   ram_pages;	/* out: pages allocated for RAM */
	unsigned long	   tables;	/* out */
	/*
	 * Where the guest's memory actually is.
	 *
	 * Guest physical addresses are host physical addresses, because Linux
	 * calls __va() on what it reads out of its own page tables and those
	 * entries hold host physical addresses. The memory comes in several
	 * contiguous blocks, each one a usable e820 range at its true address;
	 * range 0 additionally carries the reserved page-table region at its
	 * top. The daemon builds the e820 and phys_base from these rather than
	 * inventing a layout.
	 */
	unsigned long long phys_base;	/* out: what the guest's __pa() adds */
	unsigned long long total_usable;/* out: usable bytes across all ranges */
	int		   range_count;	/* out */
	struct {
		unsigned long long pa;		/* host physical base */
		unsigned long long usable;	/* usable RAM bytes */
		unsigned long long reserved;	/* reserved bytes above usable */
	} range[CO_KRAM_MAX_RANGES];
} co_manager_ioctl_kram_t;

/* interface for CO_MANAGER_IOCTL_KBOOT */
typedef struct {
	co_rc_t		   rc;
	int		   supported;
	/* in */
	unsigned long long entry_va;
	unsigned long long initial_code_va;
	unsigned long long start_kernel_va;
	unsigned long long early_console_va;
	unsigned long long colinux_console_va;
	unsigned long long ring_symbol_va;
	unsigned long long guest_flag_va;
	/*
	 * The cooperative timer. tick_entry_va is asm_sysvec_co_timer, the
	 * stock idtentry the host vectors a running guest to; virtual_if_va is
	 * the guest's virtual interrupt flag, which says whether it may be
	 * vectored there at all. Without these the guest has no asynchronous
	 * entry and nothing can ever preempt a task that stays runnable.
	 */
	unsigned long long tick_entry_va;
	unsigned long long virtual_if_va;
	int		   max_switches;
	int		   step;
	int		   batch;
	/*
	 * The kernel's own statically linked page tables, init_top_pgt first.
	 * The host relocates them to where the image really is and switches the
	 * guest into them, because the kernel walks and edits these directly --
	 * early_ioremap_init follows the chain down to install the fixmap.
	 */
	unsigned long long kernel_tables[8];
	int		   kernel_table_count;
	/*
	 * The guest's __ex_table. Linux faults on purpose -- segment loads,
	 * rdmsr_safe, every user copy -- and its own #GP and #PF handlers
	 * consult this table to recover. Those handlers do not run in a
	 * cooperative guest, so the host applies the entries instead.
	 */
	unsigned long long ex_table_start;
	unsigned long long ex_table_stop;
	/* where the guest keeps co_colinux_passage_page, for cooperative yields */
	unsigned long long passage_symbol_va;
	/* where the guest keeps co_colinux_console_io, for the terminal */
	unsigned long long console_io_va;
	/* where it keeps co_colinux_net_io, for the network rings */
	unsigned long long net_io_va;
	/* where it keeps co_colinux_cobd_io, the async block completion ring */
	unsigned long long cobd_io_va;
	/*
	 * Where it keeps co_colinux_vgpu_io, the virtio-gpu transport.
	 *
	 * Zero when the guest kernel has no transport driver, which is the
	 * normal state for any vmlinux built before R4 -- the device simply
	 * never appears and everything else boots exactly as before.
	 */
	unsigned long long vgpu_io_va;
	/* 1 = run block I/O asynchronously off the monitor thread (default) */
	int		   async_cobd;
	/* out */
	unsigned long long guest_cr3;
	unsigned long	   tables;
	unsigned long long console_ring_va;
	unsigned long long console_written;
	unsigned long long console_capacity;
	char		   console_text[2048];
	int		   faulted;
	int		   returned_voluntarily;
	int		   hit_limit;
	int		   hit_deadline;
	unsigned long	   guest_switches;
	int		   unforwardable;
	int		   vmx_present;
	unsigned long	   switches;
	unsigned long	   interrupts;
	/* Cooperative timer interrupts the host injected into a running guest. */
	unsigned long	   ticks_injected;
	unsigned long	   steps;
	unsigned long	   trace_next;
	unsigned long long trace[16];
	unsigned long long vector;
	unsigned long long fault_rip;
	unsigned long long error_code;
	unsigned long long cr2;
	unsigned long long fault_rdi;	/* the guest's operand when it faulted */
	unsigned long long fault_rax;
	/* the whole register frame at the stop: [0] r15 .. [14] rax */
	unsigned long long stop_regs[15];
	unsigned long	   warnings;	/* ud2s stepped over, as the kernel would */
	unsigned long long warning_rip[8];
	/* faults recovered from the guest's own __ex_table, as its handlers would */
	unsigned long	   fixups;
	unsigned long long fixup_rip[8];
	int		   fixup_type[8];
	int		   fault_extype;	/* set if the fatal fault had an entry */
	int		   ex_entries;		/* how many the host loaded */
	int		   reached_idle;	/* guest halted -- booted through to idle */
	/* the cooperative protocol: voluntary crossings, by kind */
	unsigned long	   run_yields;
	unsigned long	   idle_yields;
	unsigned long	   block_requests;
	unsigned long	   block_errors;
	int		   terminated;
	unsigned long long terminate_reason;
	unsigned long long stop_operation;
	int		   preflight_checked;
	int		   preflight_failed;
	int		   preflight_level;
	unsigned long long preflight_va;
	/* mirrors co_arch_boot_result_t: which host register a crossing moved */
	int		   host_corrupt_field;
	unsigned long	   host_corrupt_step;
	unsigned long long host_corrupt_expected;
	unsigned long long host_corrupt_actual;
} co_manager_ioctl_kboot_t;

/* interface for CO_MANAGER_IOCTL_KCALL: mirrors co_arch_kcall_test_t */
typedef struct {
	co_rc_t		   rc;
	int		   supported;
	int		   succeeded;
	unsigned long long memset_va;		/* in, then echoed back */
	unsigned long long strlen_va;		/* in, then echoed back */
	unsigned long long scratch_va;
	unsigned long long memset_ret;
	unsigned long long strlen_ret;
	unsigned long long strlen_expected;
	unsigned long long snprintf_va;		/* in, then echoed back */
	unsigned long long snprintf_ret;
	unsigned long long snprintf_expected;
	int		   pattern_ok;
	int		   text_ok;
	char		   text[128];
	unsigned long long early_printk_va;	/* in */
	unsigned long long early_console_va;	/* in */
	unsigned long long colinux_console_va;	/* in */
	unsigned long long ring_symbol_va;	/* in */
	unsigned long long console_ring_va;
	unsigned long long console_written;
	unsigned long long console_capacity;
	int		   console_ok;
	char		   console_text[160];
	int		   first_bad;
	int		   first_bad_byte;
	int		   faulted;
	unsigned long long vector;
	unsigned long long fault_rip;
	unsigned long long cr2;
} co_manager_ioctl_kcall_t;

/* interface for CO_MANAGER_IOCTL_TEST_SPACE: mirrors co_arch_space_test_t */
typedef struct {
	co_rc_t		   rc;
	int		   supported;
	int		   succeeded;
	unsigned long long root;
	unsigned long	   mapped;
	unsigned long	   verified;
	unsigned long	   mismatched;
	unsigned long	   tables;
	unsigned long long first_bad_va;
	unsigned long long first_bad_expect;
	unsigned long long first_bad_got;
	int		   unmapped_reported;
	int		   unmapped_level;
} co_manager_ioctl_test_space_t;

/* interface for CO_MANAGER_IOCTL_MONITOR_LIST: */
typedef struct {
	co_rc_t       rc;
	unsigned long count;
	co_id_t       ids[CO_MAX_MONITORS];
} co_manager_ioctl_monitor_list_t;

/*
 * Monitor ioctl()s
 */

/* interface for CO_MONITOR_IOCTL_LOAD_SECTION: */
typedef struct {
	co_manager_ioctl_monitor_t pc;
	char*			   user_ptr;
	/* 64-bit: these are guest kernel addresses, and LLP64 `unsigned long`
	 * on the Windows host is only 4 bytes. */
	unsigned long long	   address;
	unsigned long long	   size;
	unsigned long		   index;
	unsigned char		   buf[0];
} co_monitor_ioctl_load_section_t;

/* interface for CO_MONITOR_IOCTL_LOAD_INITRD: */
typedef struct {
	co_manager_ioctl_monitor_t pc;
	unsigned long		   size;
	unsigned char		   buf[0];
} co_monitor_ioctl_load_initrd_t;

/* interface for CO_MONITOR_IOCTL_GET_CONSOLE: */
typedef struct {
	co_manager_ioctl_monitor_t pc;
	co_console_config_t        config;
} co_monitor_ioctl_get_console_t;

typedef struct {
	unsigned long	line;
	unsigned long	code;
	char		text[128];
} co_monitor_linux_bug_invocation_t;

/* interface for CO_MONITOR_IOCTL_GET_STATE: */
typedef struct {
	co_manager_ioctl_monitor_t 	  pc;
	unsigned long			  monitor_state;
	co_termination_reason_t		  termination_reason;
	co_monitor_linux_bug_invocation_t bug_info;
} co_monitor_ioctl_get_state_t;

/* interface for CO_MONITOR_IOCTL_RUN: */
typedef struct {
	co_manager_ioctl_monitor_t pc;
} co_monitor_ioctl_run_t;

typedef enum {
	CO_MONITOR_MESSAGE_TYPE_TERMINATED,
	CO_MONITOR_MESSAGE_TYPE_DEBUG_LINE,
	CO_MONITOR_MESSAGE_TYPE_TRACE_POINT,
} co_monitor_message_type_t;

typedef struct {
	co_monitor_message_type_t type;
	union {
		struct {
			co_termination_reason_t reason;
		} terminated;
	};
} co_daemon_message_t;

/* interface for CO_MONITOR_IOCTL_STATUS */
typedef struct co_monitor_ioctl_status {
	co_manager_ioctl_monitor_t pc;
} co_monitor_ioctl_status_t;

#ifdef CONFIG_COOPERATIVE_VIDEO
/* interface for CO_MONITOR_IOCTL_VIDEO_ATTACH/DETACH: */
typedef struct {
	co_manager_ioctl_monitor_t pc;
	int			   unit;
	void*			   address;
	void*			   handle;
} co_monitor_ioctl_video_t;
#endif

/***************** support kernel mode conet ***********************/
typedef enum {
	CO_CONET_BRIDGE,	/* bridge conet adapter to external */
	CO_CONET_NAT,		/* NAT conet traffic to external */
	CO_CONET_HOST		/* communicate with local host only */
} co_conet_protocol_t;

/* CO_MONITOR_IOCTL_CONET_BIND_ADAPTER */
typedef struct { /* for co_manager_ioctl_monitor_t extra_data */
	co_manager_ioctl_monitor_t pc;
	co_conet_protocol_t	   conet_proto;
	int		    	   conet_unit;
	int			   promisc_mode;
	char			   mac_address[6];
	char 			   netcfg_id[128]; /* null terminated */
} co_monitor_ioctl_conet_bind_adapter_t;

/* CO_MONITOR_IOCTL_CONET_UNBIND_ADAPTER */
typedef struct { /* for co_manager_ioctl_monitor_t extra_data */
	co_manager_ioctl_monitor_t pc;
	int			   conet_unit;
} co_monitor_ioctl_conet_unbind_adapter_t;

#endif
