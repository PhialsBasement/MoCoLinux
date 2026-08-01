/*
 * The host half of the guest's interactive console.
 *
 * The guest keeps two circular buffers in an ordinary kernel object
 * (co_colinux_console_io, arch/x86/kernel/early_printk.c). They are not in the
 * passage page -- it has no room, every one of host_temp's seven pages being
 * spoken for -- so they are reached the way any other guest memory is, by
 * walking the guest's page tables. That is co_kload_read and co_kload_write,
 * which already exist for exactly this kind of access.
 *
 * Each ring has one writer per side: the host owns in_head and the guest owns
 * in_tail; the guest owns out_head and the host owns out_tail. No word is
 * written by both, which is what makes this safe with no lock between two
 * worlds that are not even scheduled by the same kernel.
 *
 * The indices free-run and are masked rather than wrapped, so "full" is
 * head - tail == size and there is no ambiguity between empty and full to
 * resolve with a spare slot.
 */

#include <colinux/common/common.h>
#include <colinux/kernel/manager.h>
#include <colinux/kernel/kload.h>
#include <colinux/os/kernel/mutex.h>

#include "console.h"

#define CO_CONSOLE_IN_SIZE	1024
#define CO_CONSOLE_OUT_SIZE	4096

/* Offsets into struct co_console_io, which the guest side must match. */
#define CO_CIO_IN_HEAD		0x00
#define CO_CIO_IN_TAIL		0x04
#define CO_CIO_OUT_HEAD		0x08
#define CO_CIO_OUT_TAIL		0x0c
#define CO_CIO_IN		0x10
#define CO_CIO_OUT		(CO_CIO_IN + CO_CONSOLE_IN_SIZE)

/*
 * Where the rings are in the guest. The daemon resolves the symbol and passes
 * it with the boot request; zero means this guest has no console, which is not
 * an error -- every run before this one worked that way.
 *
 * The lock is not protecting the rings, which need none: each word has a
 * single writer. It protects the *address* against the guest going away
 * underneath a console client, which is a different process on a different
 * thread and knows nothing about the run ending. Reading guest memory means
 * walking the guest's page tables, and those are freed when the run is torn
 * down -- so an unsynchronised pump arriving a moment late walks freed pages
 * and bugchecks the host. Clearing the address is not enough on its own:
 * without the lock the clear can land between a pump's check and its walk.
 */
static unsigned long long console_io_va;
static co_os_mutex_t	  console_lock;

co_rc_t co_console_init(void)
{
	return co_os_mutex_create(&console_lock);
}

void co_console_free(void)
{
	if (console_lock) {
		co_os_mutex_destroy(console_lock);
		console_lock = NULL;
	}
}

/*
 * Enable the console for a guest, or -- with zero -- retire it.
 *
 * Called with the guest's address space live on the way in, and before that
 * space is freed on the way out. A pump either completes entirely before the
 * retirement or finds the address already zero.
 */
void co_console_set_address(unsigned long long va)
{
	if (!console_lock) {
		console_io_va = va;
		return;
	}

	co_os_mutex_acquire(console_lock);
	console_io_va = va;
	co_os_mutex_release(console_lock);
}

unsigned long long co_console_get_address(void)
{
	return console_io_va;
}

static co_rc_t co_console_read_u32(co_manager_t* manager, unsigned long offset,
				   unsigned int* out)
{
	return co_kload_read(manager, console_io_va + offset,
			     (unsigned char*)out, sizeof(*out));
}

static co_rc_t co_console_write_u32(co_manager_t* manager, unsigned long offset,
				    unsigned int value)
{
	return co_kload_write(manager, console_io_va + offset,
			      (const unsigned char*)&value, sizeof(value));
}

/*
 * Push keystrokes into the guest, and pull whatever it has printed.
 *
 * Both directions are best-effort by design. Input that does not fit is
 * reported as not taken and the client keeps it for next time; output is
 * bounded by the caller's buffer and the rest waits in the ring. Neither side
 * blocks, because this runs while the guest is live inside another thread's
 * ioctl.
 */
co_rc_t co_console_pump(co_manager_t* manager,
			const char* in, unsigned long in_size, unsigned long* in_taken,
			char* out, unsigned long out_size, unsigned long* out_len)
{
	unsigned int head, tail;
	unsigned long n = 0;
	co_rc_t rc = CO_RC(OK);

	*in_taken = 0;
	*out_len  = 0;

	if (!console_lock)
		return CO_RC(NOT_FOUND);

	/*
	 * Held across every page-table walk below, which is the whole point:
	 * the guest's tables must not be freed between the check and the walk.
	 */
	co_os_mutex_acquire(console_lock);

	if (!console_io_va) {
		co_os_mutex_release(console_lock);
		return CO_RC(NOT_FOUND);
	}

	/* keystrokes in: the host owns in_head */
	if (in_size) {
		if (!CO_OK(co_console_read_u32(manager, CO_CIO_IN_HEAD, &head)) ||
		    !CO_OK(co_console_read_u32(manager, CO_CIO_IN_TAIL, &tail))) {
			rc = CO_RC(ERROR);
			goto out;
		}

		while (n < in_size && (head - tail) < CO_CONSOLE_IN_SIZE) {
			unsigned long slot = head & (CO_CONSOLE_IN_SIZE - 1);

			if (!CO_OK(co_kload_write(manager,
						  console_io_va + CO_CIO_IN + slot,
						  (const unsigned char*)&in[n], 1))) {
				rc = CO_RC(ERROR);
				goto out;
			}
			head++;
			n++;
		}

		/*
		 * The bytes are in the ring before the guest is told, which is
		 * the same ordering the guest uses on the way out.
		 */
		if (n && !CO_OK(co_console_write_u32(manager, CO_CIO_IN_HEAD, head))) {
			rc = CO_RC(ERROR);
			goto out;
		}

		*in_taken = n;
	}

	/* output out: the host owns out_tail */
	if (out_size) {
		if (!CO_OK(co_console_read_u32(manager, CO_CIO_OUT_HEAD, &head)) ||
		    !CO_OK(co_console_read_u32(manager, CO_CIO_OUT_TAIL, &tail))) {
			rc = CO_RC(ERROR);
			goto out;
		}

		/*
		 * A guest that printed more than the ring holds while nobody
		 * was reading has already lost the oldest bytes; it wrote past
		 * them. Skip forward rather than hand back the wrap.
		 */
		if (head - tail > CO_CONSOLE_OUT_SIZE)
			tail = head - CO_CONSOLE_OUT_SIZE;

		n = 0;
		while (n < out_size && tail != head) {
			unsigned long slot = tail & (CO_CONSOLE_OUT_SIZE - 1);

			if (!CO_OK(co_kload_read(manager,
						 console_io_va + CO_CIO_OUT + slot,
						 (unsigned char*)&out[n], 1))) {
				rc = CO_RC(ERROR);
				goto out;
			}
			tail++;
			n++;
		}

		if (n && !CO_OK(co_console_write_u32(manager, CO_CIO_OUT_TAIL, tail))) {
			rc = CO_RC(ERROR);
			goto out;
		}

		*out_len = n;
	}

out:
	co_os_mutex_release(console_lock);
	return rc;
}
