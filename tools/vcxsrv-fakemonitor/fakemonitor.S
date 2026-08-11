/*
 * fakemonitor -- give VcXsrv 1.14.2.1 the one monitor it never reports.
 *
 * The problem. VcXsrv answers RandR with zero outputs, zero CRTCs and zero
 * modes, so a client that enumerates monitors finds nothing:
 *
 *     $ xrandr
 *     Screen 0: minimum 0 x 0, current 1600 x 900, maximum 4096 x 4096
 *     (and nothing else -- no output, no mode, no monitor)
 *
 * Anything that looks for a display before opening a window fails on that.
 *
 * Why the server has no monitor, and why it nearly does. The X server owns
 * output creation; no client, proxy or library can add one over the wire.
 * But xorg-server already contains a complete routine that manufactures
 * exactly one CRTC and one connected output named "default" for drivers that
 * only speak the old RandR 1.0 interface -- RRScanOldConfig() in
 * randr/rrinfo.c. RRGetInfo() calls it:
 *
 *     if (!(*pScrPriv->rrGetInfo) (pScreen, &rotations))
 *         return FALSE;
 *     if (pScrPriv->nSizes)
 *         RRScanOldConfig(pScreen, rotations);
 *
 * nSizes is raised by RRRegisterSize(), which a video driver is expected to
 * call from its rrGetInfo hook. XWin's hook -- winRandRGetInfo() in
 * hw/xwin/winrandr.c -- sets the rotation mask and the size range and returns,
 * never registering a size. So nSizes stays 0, RRScanOldConfig never runs, and
 * the server describes a screen with nothing on it. That single omission is
 * the entire bug, and every RandR reply the server sends is already correct
 * code waiting for a monitor to describe.
 *
 * What this is. A replacement rrGetInfo hook. It calls the original, then does
 * the one thing the original forgot: registers the current screen as a single
 * 60 Hz size. The server's own RRScanOldConfig then creates the CRTC, creates
 * the output, marks it Connected, builds the mode, and points the CRTC at it.
 * From there stock code answers every RandR request from every client -- xrandr
 * over libXrandr, and anything speaking xcb-randr directly -- because the
 * monitor is real as far as the server is concerned. Nothing is presented
 * through it; it exists to be enumerated.
 *
 * RRRegisterSize() itself is not in the shipped binary: nothing called it, so
 * the linker dropped it. Its body is nine stores, and they are open-coded here
 * against the struct layout of the pinned 1.14.2.1 source, every offset
 * verified against the shipped code (see patch-vcxsrv.py, which re-checks them
 * before writing anything).
 *
 * Position independence is not optional. vcxsrv.exe sets DYNAMIC_BASE, so
 * Windows relocates the image and no absolute address may be baked in. Every
 * reference below is RIP-relative or a relative call, which is why this is
 * assembled and linked at the address the patcher will place it at rather than
 * compiled as ordinary C.
 *
 * Failure is designed to be invisible: on any unexpected state -- no screen
 * private, a size already registered, malloc failing -- it leaves the server
 * exactly as it found it and returns what the original hook returned.
 */

	.text
	.globl	moco_randr_getinfo

/* rrScrPrivRec, from randr/randrstr.h */
#define PRIV_NSIZES	0xf0
#define PRIV_PSIZES	0xf8
#define PRIV_ROTATION	0x100
#define PRIV_RATE	0x104
#define PRIV_SIZE	0x108

/* ScreenRec, from include/scrnintstr.h */
#define SCR_WIDTH	0x0c
#define SCR_HEIGHT	0x0e
#define SCR_MMWIDTH	0x10
#define SCR_MMHEIGHT	0x12
#define SCR_PRIVATES	0x3e0

/* RRScreenSize / RRScreenRate, from randr/randrstr.h */
#define SZ_ID		0x00
#define SZ_WIDTH	0x04
#define SZ_HEIGHT	0x06
#define SZ_MMWIDTH	0x08
#define SZ_MMHEIGHT	0x0a
#define SZ_NRATES	0x0c
#define SZ_PRATES	0x10
#define SIZEOF_SCREENSIZE 0x18

#define RATE_RATE	0x00
#define RATE_MODE	0x08
#define SIZEOF_SCREENRATE 0x10

#define REFRESH		60
#define RR_ROTATE_0	1

/*
 * Bool moco_randr_getinfo(ScreenPtr pScreen, Rotation *pRotations)
 *
 * Win64: rcx = pScreen, rdx = pRotations. rbx/rsi/rdi are callee-saved, and
 * three pushes plus the 32-byte shadow space leave rsp 16-byte aligned at
 * every call below.
 */
moco_randr_getinfo:
	push	%rbx
	push	%rsi
	push	%rdi
	sub	$0x20, %rsp

	mov	%rcx, %rsi			/* keep pScreen */
	call	winRandRGetInfo			/* args are already in place */
	test	%eax, %eax
	je	.Lout				/* it failed: say so, change nothing */

	/*
	 * rbx = rrGetScrPriv(pScreen). dixLookupPrivate stores the private
	 * inline when the key has a size and behind a pointer otherwise; both
	 * forms are open-coded here exactly as the server's own callers do.
	 */
	cmpl	$0, rrPrivKeySize(%rip)
	je	.Lindirect
	movslq	rrPrivKeyOffset(%rip), %rbx
	add	SCR_PRIVATES(%rsi), %rbx
	jmp	.Lhave_priv
.Lindirect:
	movslq	rrPrivKeyOffset(%rip), %rcx
	mov	SCR_PRIVATES(%rsi), %rax
	mov	(%rcx,%rax,1), %rbx
.Lhave_priv:
	test	%rbx, %rbx
	je	.Ldone				/* no RandR on this screen */

	/*
	 * RRScanOldConfig consumes the list each time (it frees pSizes and
	 * zeroes nSizes), so a non-zero count here means someone registered
	 * already this round -- leave it alone.
	 */
	cmpl	$0, PRIV_NSIZES(%rbx)
	jne	.Ldone

	mov	$SIZEOF_SCREENSIZE, %ecx
	call	*malloc_iat(%rip)		/* the CRT whose free() the server calls */
	test	%rax, %rax
	je	.Ldone
	mov	%rax, %rdi			/* rdi = the size */

	/*
	 * A rate list is what makes the mode report 60.00 Hz instead of 0.00:
	 * RRScanOldConfig multiplies it into the dot clock. It is optional --
	 * if this allocation fails the size is still registered, just without
	 * a refresh rate.
	 */
	mov	$SIZEOF_SCREENRATE, %ecx
	call	*malloc_iat(%rip)
	test	%rax, %rax
	je	.Lno_rates
	movw	$REFRESH, RATE_RATE(%rax)
	movq	$0, RATE_MODE(%rax)
	mov	%rax, SZ_PRATES(%rdi)
	movl	$1, SZ_NRATES(%rdi)
	jmp	.Lfill
.Lno_rates:
	movq	$0, SZ_PRATES(%rdi)
	movl	$0, SZ_NRATES(%rdi)

.Lfill:
	movl	$0, SZ_ID(%rdi)
	movzwl	SCR_WIDTH(%rsi), %eax
	mov	%ax, SZ_WIDTH(%rdi)
	movzwl	SCR_HEIGHT(%rsi), %eax
	mov	%ax, SZ_HEIGHT(%rdi)
	movzwl	SCR_MMWIDTH(%rsi), %eax
	mov	%ax, SZ_MMWIDTH(%rdi)
	movzwl	SCR_MMHEIGHT(%rsi), %eax
	mov	%ax, SZ_MMHEIGHT(%rdi)

	/*
	 * Publish it, and name it as the current configuration: RRScanOldConfig
	 * only calls RRCrtcNotify -- the step that actually lights the CRTC and
	 * gives the output a geometry -- for the entry matching pScrPriv->size
	 * and ->rate. Registering a size without these leaves a connected
	 * output with no mode set.
	 */
	mov	%rdi, PRIV_PSIZES(%rbx)
	movl	$1, PRIV_NSIZES(%rbx)
	movw	$RR_ROTATE_0, PRIV_ROTATION(%rbx)
	movl	$REFRESH, PRIV_RATE(%rbx)
	movl	$0, PRIV_SIZE(%rbx)

.Ldone:
	mov	$1, %eax
.Lout:
	add	$0x20, %rsp
	pop	%rdi
	pop	%rsi
	pop	%rbx
	ret
