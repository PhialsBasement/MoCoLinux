/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2004 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#include <colinux/os/alloc.h>
#include <colinux/os/kernel/user.h>
#include <colinux/common/libc.h>

#include "manager.h"
#include "debug.h"

static void report_status(const char *event, co_manager_debug_t *debug)
{
#if 0
	co_debug_system("colinux-debug: %s: sections: %d, total: %ld, filled: %ld",
			event, debug->sections_count, debug->sections_total_size,
			debug->sections_total_filled);
#endif
}

static co_rc_t create_section(co_debug_section_t **section_out)
{
	co_debug_section_t *section;
	co_rc_t rc;

	section = co_os_malloc(sizeof(co_debug_section_t));
	if (!section)
		return CO_RC(OUT_OF_MEMORY);

	section->buffer = co_os_malloc(CO_DEBUG_SECTION_BUFFER_START_SIZE);
	if (!section->buffer) {
		co_os_free(section);
		return CO_RC(OUT_OF_MEMORY);
	}

	rc = co_os_mutex_create(&section->mutex);
	if (!CO_OK(rc)) {
		co_os_free(section->buffer);
		co_os_free(section);
		return rc;
	}

	co_list_init(&section->node);
	section->buffer_size = CO_DEBUG_SECTION_BUFFER_START_SIZE;
	section->filled = 0;
	section->refcount = 0;
	section->folded = PFALSE;
	section->peak_size = section->buffer_size;
	section->max_size = CO_DEBUG_SECTION_BUFFER_MAX_SIZE;

	*section_out = section;

	return CO_RC(OK);
}

/*
 * Grow a section that is filling up, shrink one that has gone quiet.
 *
 * This function did nothing at all for twenty-two years, and the way it did
 * nothing is worth keeping in front of whoever reads it next. It computed
 * new_size as either buffer_size * 2 or peak_size / 2 -- both different from
 * buffer_size by construction -- and then returned on `old_size != new_size`.
 * So it returned on every path where a resize had been asked for, and the only
 * way to reach the allocation was for the new size to equal the old one, which
 * would have made it pointless anyway. The test wanted to be `==`: there is
 * nothing to do when the size would not change.
 *
 * The consequence was not "logs are a bit lossy". Every section stayed at
 * 4 KB forever, so any burst that outran the daemon's drain filled it and
 * every record after that was refused by append_to_buffer and dropped with no
 * diagnostic. That is why the debug stream went silent seven minutes before a
 * bugcheck and left nothing from the teardown that caused it -- the log stops
 * exactly when the driver becomes interesting.
 *
 * The shrink path needs the clamp below before it may be enabled. It sets
 * new_size to peak_size / 2 and then copies `filled` bytes into it, and its own
 * branch condition only guarantees filled < START_SIZE. With peak still at
 * START_SIZE that is a 2 KB buffer receiving up to 4095 bytes: a pool overrun
 * out of the logging path. It never fired only because the comparison above
 * disabled the entire function, so repairing that one line without this one
 * would have armed it.
 */
static void resize_section(co_manager_debug_t *debug, co_debug_section_t *section)
{
	long new_size = 0, old_size = 0;
	char *new_buffer, *old_buffer;

	/*
	 * Disabled again, deliberately, and this is not the inverted comparison
	 * coming back.
	 *
	 * Repairing the comparison made this function allocate and free for the
	 * first time since 2004. With a debug daemon draining hard enough for
	 * sections to grow, the machine took a bugcheck 0x4E, PFN_LIST_CORRUPT,
	 * parameter one 0x99 -- the host's page-frame database, corrupt -- and
	 * the stack was co_debug_ -> co_debug_buf -> co_debug_writev ->
	 * put_section, freeing a section whose pool header was already wrong.
	 * Every other allocation in the debug path is twenty years old and
	 * unchanged; this is the only new one.
	 *
	 * That is not proof, and the code above reads correctly to me: both
	 * branches produce a new_size no smaller than `filled`, the copy is
	 * bounded by `filled`, and callers re-read section->buffer afterwards.
	 * Which is exactly why it stays off rather than being tweaked -- an
	 * allocation path that corrupts the PFN list is not something to iterate
	 * on against a machine that has to keep working, and the only thing it
	 * buys is a debug buffer that grows under load.
	 *
	 * What it costs while off is what it cost for twenty-two years: a burst
	 * of logging that outruns the reader fills the 4 KB section and
	 * append_to_buffer drops the rest. Lossy, silent, and survivable.
	 *
	 * To re-enable: delete this return, and reproduce with Driver Verifier's
	 * special pool on linux.sys so an overrun is caught at the moment it
	 * happens rather than at the next free.
	 */
	return;

	if (section->filled >= section->buffer_size / 2) {
		/*
		 * If the buffer is half full, increase it's size by a factor
		 * of two. This reduces the chance of losing logs due to write
		 * bursts but keeps the buffers short.
		 */

		new_size = section->buffer_size * 2;
		if (new_size > section->max_size)
			return;
	} else if (section->filled < CO_DEBUG_SECTION_BUFFER_START_SIZE) {
		/*
		 * Reduce to half the size of the peak size if the buffer is
		 * quite empty. Never below the starting size, which is also
		 * what keeps this larger than `filled` -- this branch is only
		 * taken when filled is below START_SIZE, so a floor of
		 * START_SIZE makes the copy below safe by construction.
		 */
		new_size = section->peak_size / 2;
		if (new_size < CO_DEBUG_SECTION_BUFFER_START_SIZE)
			new_size = CO_DEBUG_SECTION_BUFFER_START_SIZE;
	}

	if (new_size == 0)
		return;

	old_size = section->buffer_size;
	if (old_size == new_size)
		return;

	/*
	 * Belt and braces, because the cost of being wrong here is a pool
	 * overrun in the one code path every other diagnosis depends on.
	 */
	if (new_size < section->filled)
		return;

	new_buffer = co_os_malloc(new_size);
	if (!new_buffer)
		return;

	old_buffer = section->buffer;
	co_memcpy(new_buffer, old_buffer, section->filled);

	debug->sections_total_size += new_size - old_size;
	section->buffer_size = new_size;
	section->buffer = new_buffer;

	if (section->peak_size < new_size)
		section->peak_size = new_size;

	report_status("section resize", debug);
	co_os_free(old_buffer);
}

static void memcpy_vector(char *dest, co_debug_write_vector_t *vec, int vec_size)
{
	while (vec_size--) {
		if (vec->vec_size)
			memcpy_vector(dest, vec->vec, vec->vec_size);
		else
			co_memcpy(dest, vec->ptr, vec->size);
		dest += vec->size;
		vec++;
	}
}

static inline long co_debug_write_vector_size(co_debug_write_vector_t *vec, int vec_size)
{
	long size = 0;
	while (vec_size--) {
		size += vec->size;
		vec++;
	}
	return size;
}

static co_rc_t append_to_buffer(co_manager_debug_t *debug, co_debug_section_t *section,
				co_debug_write_vector_t *vec, int vec_size)
{
	co_rc_t rc = CO_RC(OK);
	long vec_length = 0;

	resize_section(debug, section);

	vec_length = co_debug_write_vector_size(vec, vec_size);

	/* Check if there's not enough space left in the buffer */
	if (section->buffer_size - section->filled < vec_length) {
		rc = CO_RC(ERROR);
	} else {
		memcpy_vector(&section->buffer[section->filled], vec, vec_size);
		section->filled += vec_length;
		debug->sections_total_filled += vec_length;
	}

	return rc;
}

static void get_section(struct co_debug_section *section)
{
	section->refcount++;

	co_os_mutex_acquire_critical(section->mutex);
}

static bool_t put_section(co_manager_debug_t *debug,
			  struct co_debug_section *section)
{
	section->refcount--;
	if (section->refcount == 0) {
		debug->sections_count--;
		debug->sections_total_size -= section->buffer_size;
		co_os_mutex_release_critical(section->mutex);
		co_os_mutex_destroy(section->mutex);
		co_os_free(section->buffer);
		co_os_free(section);
		report_status("section release", debug);
		return PFALSE;
	}

	co_os_mutex_release_critical(section->mutex);

	return PTRUE;
}

static co_rc_t co_debug_writev(co_manager_debug_t *debug,
			struct co_debug_section **section_ptr,
			co_debug_write_vector_t *vec, int vec_size)
{
	co_rc_t rc;
	co_debug_section_t *section;

	if (debug->sections_total_filled > CO_DEBUG_MAX_FILL)
		return CO_RC(OK);

	if (debug->freeing)
		return CO_RC(OK);

	section = *section_ptr;
	if (!section) {
		rc = create_section(&section);
		if (!CO_OK(rc))
			goto out;

		debug->sections_count++;
		debug->sections_total_size += section->buffer_size;

		/*
		 * freeing is tested again here, inside the lock, and it is not
		 * redundant with the test above.
		 *
		 * That one is a plain read of a flag another thread sets, so a
		 * writer can pass it and then be descheduled while co_debug_free
		 * empties the list and frees every section. The add that follows
		 * links the new section against a stale sections.prev -- a
		 * section that has been returned to the pool -- and the store to
		 * old_prev->next is a write into freed memory. With Driver
		 * Verifier's special pool that is bugcheck 0xD5 at exactly this
		 * instruction, which is how it was found; without Verifier it is
		 * silent corruption of whatever now owns that page.
		 *
		 * co_debug_free takes the same lock to set the flag and to tear
		 * the list down, so a writer that gets here either arrives
		 * before the teardown and is linked into a live list, or arrives
		 * after and is refused.
		 */
		co_os_mutex_acquire_critical(debug->mutex);
		if (debug->freeing) {
			co_os_mutex_release_critical(debug->mutex);
			co_os_mutex_destroy(section->mutex);
			co_os_free(section->buffer);
			co_os_free(section);
			debug->sections_count--;
			debug->sections_total_size -= CO_DEBUG_SECTION_BUFFER_START_SIZE;
			return CO_RC(OK);
		}
		co_list_add_tail(&section->node, &debug->sections);
		co_os_mutex_release_critical(debug->mutex);

		report_status("section created", debug);
		get_section(section);

		*section_ptr = section;
		section->refcount++;
	} else {
		get_section(section);
	}

	rc = append_to_buffer(debug, section, vec, vec_size);

	if (!put_section(debug, section))
		*section_ptr = NULL;

	if (CO_OK(rc)) {
		co_os_wait_wakeup(debug->read_wait);
	}

out:
	return rc;
}

static int debug_driver_index;
static co_debug_tlv_t debug_tlv_index = {
	.type = CO_DEBUG_TYPE_DRIVER_INDEX,
	.length = sizeof(debug_driver_index),
};

static co_debug_write_vector_t driver_local_vec[2] = {
	{
	.size = sizeof(debug_tlv_index),
	},
	{
	.size = sizeof(debug_driver_index),
	}
};

co_rc_t co_debug_write_log(co_manager_debug_t *debug,
			   struct co_debug_section **section_ptr,
			   co_debug_write_vector_t *vec, int vec_size)
{
	unsigned long size;
	co_debug_tlv_t tlv;
	co_debug_write_vector_t local_vec[3];

	size = co_debug_write_vector_size(vec, vec_size);
	if (size > 0xf8)
		return CO_RC(OK);

	debug_driver_index++;

	tlv.type = CO_DEBUG_TYPE_TLV;
	tlv.length = size + sizeof(debug_tlv_index) + sizeof(debug_driver_index);

	local_vec[0].vec_size = 0;
	local_vec[0].size = sizeof(tlv);
	local_vec[0].ptr = (void *)&tlv;
	local_vec[1].vec_size = vec_size;
	local_vec[1].size = size;
	local_vec[1].vec = vec;
	local_vec[2].vec_size = 2;
	local_vec[2].size = sizeof(debug_tlv_index) + sizeof(debug_driver_index);
	local_vec[2].vec = driver_local_vec;
	driver_local_vec[0].ptr = (void *)&debug_tlv_index;
	driver_local_vec[1].ptr = (void *)&debug_driver_index;

	return co_debug_writev(debug, section_ptr, local_vec, 3);
}

co_rc_t co_debug_read(co_manager_debug_t *debug,
		      char *buf, unsigned long size,
		      unsigned long *read_size)
{
	co_rc_t rc;
	co_debug_section_t *section, *section_new;
	long user_filled = 0;

	co_os_wait_sleep(debug->read_wait);
	co_os_mutex_acquire(debug->mutex);

	co_list_each_entry_safe(section, section_new, &debug->sections, node) {
		bool_t saved = PFALSE;
		get_section(section);

		if (!(size - user_filled < section->filled)) {
			rc = co_copy_to_user(&buf[user_filled], section->buffer, section->filled);
			if (CO_OK(rc)) {
				user_filled += section->filled;
				debug->sections_total_filled -= section->filled;
				section->filled = 0;
				saved = PTRUE;
			}
		}

		if (saved  &&  section->folded) {
			co_list_del(&section->node);
			section->refcount--;
			section->folded = PFALSE;
		}

		if (!section->folded)
			resize_section(debug, section);

		put_section(debug, section);
	}

	co_os_mutex_release(debug->mutex);

	*read_size = user_filled;

	return CO_RC(OK);
}


co_rc_t co_debug_fold(co_manager_debug_t *debug, co_debug_section_t *section)
{
	char *shrunk_buffer;
	co_rc_t rc = CO_RC(OK);

	co_os_mutex_acquire(debug->mutex);
	get_section(section);

	shrunk_buffer = co_os_malloc(section->filled + 1);
	if (!shrunk_buffer) {
		rc = CO_RC(OUT_OF_MEMORY);
	} else {
		co_memcpy(shrunk_buffer, section->buffer, section->filled);
		co_os_free(section->buffer);

		debug->sections_total_size -= section->buffer_size;
		section->buffer = shrunk_buffer;
		section->buffer_size = section->filled;
		debug->sections_total_size += section->buffer_size;
	}

	section->folded = PTRUE;

	put_section(debug, section);
	co_os_mutex_release(debug->mutex);

	return rc;
}

co_rc_t co_debug_init(co_manager_debug_t *debug)
{
	co_rc_t rc;

	debug->freeing = PFALSE;
	debug->section = NULL;
	debug->sections_total_filled = 0;
	debug->sections_total_size = 0;

	co_list_init(&debug->sections);

	rc = co_os_mutex_create(&debug->mutex);
	if (!CO_OK(rc))
		goto out;

	rc = co_os_wait_create(&debug->read_wait);
	if (!CO_OK(rc)) {
		co_os_mutex_destroy(debug->mutex);
		goto out;
	}

	debug->ready = PTRUE;

out:
	return rc;
}

co_rc_t co_debug_free(co_manager_debug_t *debug)
{
	co_debug_section_t *section, *section_new;

	report_status("freeing all sections", debug);
	debug->ready = PFALSE;

	/*
	 * The flag and the teardown under one lock, because writers add to this
	 * list under the same lock. Setting the flag outside it left a window
	 * in which a writer had already decided the debug system was alive and
	 * was about to link a section into a list this loop was dismantling.
	 */
	co_os_mutex_acquire(debug->mutex);
	debug->freeing = PTRUE;

	co_list_each_entry_safe(section, section_new, &debug->sections, node) {
		get_section(section);
		co_list_del(&section->node);
		section->refcount--;
		put_section(debug, section);
	}
	co_os_mutex_release(debug->mutex);

	co_os_wait_destroy(debug->read_wait);
	co_os_mutex_destroy(debug->mutex);
	report_status("done freeing", debug);

	return CO_RC(OK);
}

void co_debug_buf(const char *buf, long size)
{
	co_debug_write_vector_t vec;

	if (!co_global_manager)
		return;

	if (!co_global_manager->debug.ready)
		return;

	vec.vec_size = 0;
	vec.size = size;
	vec.ptr = buf;

	co_debug_write_log(&co_global_manager->debug, &co_global_manager->debug.section, &vec, 1);
}

