/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */
 /* OS independent console support functions */

#include <colinux/os/current/memory.h>
#include <colinux/os/alloc.h>
#include <colinux/common/libc.h>

#include "console.h"

/*
 * Console initialization.
 */

/* Create the console object */
co_rc_t co_console_create(co_console_config_t* config_par,
			  co_console_t**       console_out)
{
	unsigned long      struct_size;
	co_console_t*      console;

	/*
	 * Use only one allocation for the entire console object so it would
	 * be more managable (and less code). All limits are checked in 'config.c'
	 */
	struct_size  = sizeof(co_console_cell_t) * config_par->x * config_par->max_y;
	struct_size += sizeof(co_console_t);

	console = co_os_malloc(struct_size);
	if (console == NULL)
		return CO_RC(OUT_OF_MEMORY);

	co_memset(console, 0, struct_size);

	console->size	= struct_size;
	console->config	= *config_par;
	console->buffer	= (co_console_cell_t*)((char*)console + sizeof(co_console_t));
	/* Remember: console->buffer have a sizeof co_console_cell_t */
	console->screen	= console->buffer + (config_par->max_y - config_par->y) * config_par->x;

	*console_out = console;

	return CO_RC(OK);
}

/* Free memory of the console object */
void co_console_destroy(co_console_t* console)
{
	co_os_free(console);
}

/*
 * Console message processor.
 *
 * These messages come from the cocon driver of the coLinux kernel:
 *   CO_OPERATION_CONSOLE_PUTC		- Put single char-attr pair
 *   CO_OPERATION_CONSOLE_PUTCS		- Put char-attr pair array
 *   CO_OPERATION_CONSOLE_CURSOR_MOVE	- Move cursor
 *   CO_OPERATION_CONSOLE_CURSOR_DRAW	- Move cursor
 *   CO_OPERATION_CONSOLE_CURSOR_ERASE  - Move cursor
 *   CO_OPERATION_CONSOLE_SCROLL_UP	- Scroll lines up
 *   CO_OPERATION_CONSOLE_SCROLL_DOWN	- Scroll lines down
 *   CO_OPERATION_CONSOLE_BMOVE		- Move region up/down
 *   CO_OPERATION_CONSOLE_CLEAR		- Clear region
 *   CO_OPERATION_CONSOLE_STARTUP	- Set screen dimension, ignored in all consoles, used only in cocon.c
 *   CO_OPERATION_CONSOLE_BLANK		- Ignored here and in all consoles
 *   CO_OPERATION_CONSOLE_SWITCH	- Ignored here and in all consoles
 *   CO_OPERATION_CONSOLE_FONT_OP	- Ignored here and in all consoles
 *   CO_OPERATION_CONSOLE_SET_PALETTE	- Ignored here and in all consoles
 *   CO_OPERATION_CONSOLE_SCROLLDELTA	- Ignored here and in all consoles
 *   CO_OPERATION_CONSOLE_SET_ORIGIN	- Ignored here and in all consoles
 *   CO_OPERATION_CONSOLE_SAVE_SCREEN	- Ignored here and in all consoles
 *   CO_OPERATION_CONSOLE_INVERT_REGION	- Ignored here and in all consoles
 *   CO_OPERATION_CONSOLE_CONFIG	- Ignored here and in all consoles
 *   CO_OPERATION_CONSOLE_INIT		- Ignored here and in all consoles
 *   CO_OPERATION_CONSOLE_DEINIT	- Ignored here and in all consoles
 */
co_rc_t co_console_op(co_console_t* console, co_console_message_t* message)
{
	switch (message->type)
	{
	case CO_OPERATION_CONSOLE_SCROLL_UP:
	{
		unsigned long lines = message->scroll.lines;
		unsigned long t     = message->scroll.top;
		unsigned long b     = message->scroll.bottom + 1;
		unsigned long config_x = console->config.x;
		unsigned long t_count  = config_x * t;
		unsigned long s_count  = config_x * lines; /* Filled with spaces */
		unsigned long offs; /* Offset in buffer between scroll and fill area */
		co_console_cell_t blank = *(co_console_cell_t*)(&message->scroll.charattr);
		co_console_cell_t *dest;

		if(t || (b!=console->config.y))
		{
			// i.e., t is non-zero, meaning scroll inside an editor for example
			if (b > console->config.max_y)
				return CO_RC(ERROR);
			if (t + lines >= console->config.max_y)
				return CO_RC(ERROR);

			offs = config_x * b - s_count;

			co_memmove(console->screen + t_count,
				console->screen + t_count + s_count,
				(offs - t_count) * sizeof(co_console_cell_t));

			dest = console->screen + offs;
		}
		else
		{
			// regular scroll, i.e., shell ; now we scroll the buffer as well
			offs = config_x * console->config.max_y - s_count;

			co_memmove(console->buffer,
				console->buffer + s_count,
				offs * sizeof(co_console_cell_t));

			// here we can use the screen or the buffer, it does not matter but we have to use proper indexing
			dest = console->buffer + offs;
		}

		// Fill new lines with spaces
		for (; s_count > 0; s_count--)
			*dest++ = blank;

		break;
	}

	case CO_OPERATION_CONSOLE_SCROLL_DOWN:
	{
		unsigned long lines = message->scroll.lines;
		unsigned long t     = message->scroll.top;
		unsigned long b     = message->scroll.bottom + 1;
		unsigned long config_x = console->config.x;
		unsigned long s_count  = config_x * lines; /* Rest filled with spaces */
		co_console_cell_t blank = *(co_console_cell_t*)(&message->scroll.charattr);
		co_console_cell_t *dest;

		if(t || (b!=console->config.y))
		{
			unsigned long t_count = config_x * t;

			// i.e., t is non-zero, meaning scroll inside an editor for example
			if (b > console->config.max_y)
				return CO_RC(ERROR);

			if (t + lines >= console->config.max_y)
				return CO_RC(ERROR);

			dest = console->screen + t_count;
			co_memmove(dest + s_count,
				dest,
				(config_x * b - t_count - s_count) * sizeof(co_console_cell_t));

		}
		else
		{
			// regular scroll, i.e., shell ; now we scroll the buffer as well
			dest = console->screen;
			co_memmove(dest + s_count,
				dest,
				(config_x * console->config.y - s_count) * sizeof(co_console_cell_t));
		}

		// Fill new lines with spaces
		for (; s_count > 0; s_count--)
			*dest++ = blank;

		break;
	}
	case CO_OPERATION_CONSOLE_PUTCS: {
		/* Copy array of char-attr pairs to screen */
		int                x     = message->putcs.x;
		int                y     = message->putcs.y;
		int                count = message->putcs.count;
		unsigned long   config_x = console->config.x;
		co_console_cell_t* cells = (co_console_cell_t *)&message->putcs.data;
		co_console_cell_t* dest;

		if (y >= console->config.y || x >= config_x)
			return CO_RC(ERROR);

		if (count > config_x - x)
			count = config_x - x;

		dest = console->screen + config_x * y + x;
		while (count-- > 0)
			*dest++ = *cells++;

		break;
	}
	case CO_OPERATION_CONSOLE_PUTC: {
		/* Copy single char-attr pair to screen */
		int x = message->putc.x;
		int y = message->putc.y;
		unsigned long config_x = console->config.x;

		if (y >= console->config.y || x >= config_x)
			return CO_RC(ERROR);

		console->screen[config_x * y + x] =
			*(co_console_cell_t*)(&message->putc.charattr);

		break;
	}
	case CO_OPERATION_CONSOLE_CURSOR_DRAW:
	case CO_OPERATION_CONSOLE_CURSOR_ERASE:
	case CO_OPERATION_CONSOLE_CURSOR_MOVE: {
		console->cursor = message->cursor;
		break;
	}

	case CO_OPERATION_CONSOLE_CLEAR: {
		int		   t = message->clear.top;
		int		   b = message->clear.bottom;
		int		   r = message->clear.right;
		int 		   l;
		co_console_cell_t* cell;
		co_console_cell_t blank = *(co_console_cell_t*)(&message->clear.charattr);
		unsigned long config_x = console->config.x;

		while (t <= b) {
			l    = message->clear.left;
			cell = console->screen + config_x * t + l;
			while (l++ <= r)
				*cell++ = blank;
			t++;
		}
		break;
	}

	case CO_OPERATION_CONSOLE_BMOVE: {
		int y = message->bmove.row;
		int x = message->bmove.column;
		int t = message->bmove.top;
		int l = message->bmove.left;
		int b = message->bmove.bottom;
		int r = message->bmove.right;
		unsigned long config_x = console->config.x;

		if (y < t) {
			while (t <= b) {
				co_memmove(console->screen + config_x * y + x,
					console->screen + config_x * t + l,
					(r - l + 1) * sizeof(co_console_cell_t));
				t++;
				y++;
			}
		} else	{
			y += b-t;
			while (t <= b) {
				co_memmove(console->screen + config_x * y + x,
					console->screen + config_x * b + l,
					(r - l + 1) * sizeof(co_console_cell_t));
				b--;
				y--;
			}
		}
		break;
	}

	case CO_OPERATION_CONSOLE_STARTUP:
		message->type        = CO_OPERATION_CONSOLE_CONFIG;
		message->config.cols = console->config.x;
		message->config.rows = console->config.y;
		message->config.attr = console->config.attr;
		break;

	case CO_OPERATION_CONSOLE_INIT_SCROLLBUFFER:
	{
		/* Copy array of char-attr pairs to screen */
		int                x     = message->putcs.x;
		int                y     = message->putcs.y;
		int                count = message->putcs.count;
		unsigned long   config_x = console->config.x;
		co_console_cell_t* cells = (co_console_cell_t *)&message->putcs.data;
		co_console_cell_t* dest;

		if (y >= console->config.max_y || x >= config_x)
			return CO_RC(ERROR);

		if (count > config_x - x)
			count = config_x - x;

		dest = console->buffer + config_x * y + x;
		while (count-- > 0)
			*dest++ = *cells++;

		break;
	}

	case CO_OPERATION_CONSOLE_SWITCH:
		// clear scroll buffer
		if (console == NULL)
			return CO_RC(OUT_OF_MEMORY);
		co_memset(console->buffer, 0,
			sizeof(co_console_cell_t) * console->config.x * (console->config.max_y-console->config.y));
		break;

	case CO_OPERATION_CONSOLE_INIT:
	case CO_OPERATION_CONSOLE_DEINIT:
	case CO_OPERATION_CONSOLE_BLANK:
	case CO_OPERATION_CONSOLE_FONT_OP:
	case CO_OPERATION_CONSOLE_SET_PALETTE:
	case CO_OPERATION_CONSOLE_SCROLLDELTA:
	case CO_OPERATION_CONSOLE_SET_ORIGIN:
	case CO_OPERATION_CONSOLE_SAVE_SCREEN:
	case CO_OPERATION_CONSOLE_INVERT_REGION:
	case CO_OPERATION_CONSOLE_CONFIG:
		break;
	}

	return CO_RC(OK);
}

#if defined UNUSED

void co_console_pickle(co_console_t* console)
{
	console->screen  = (co_console_cell_t*)(((char*)console->screen)  - (unsigned long)console);
	console->backlog = (co_console_cell_t*)(((char*)console->backlog) - (unsigned long)console);
}

void co_console_unpickle(co_console_t* console)
{
	console->screen  = (co_console_cell_t*)(((char*)console->screen)  + (unsigned long)console);
	console->backlog = (co_console_cell_t*)(((char*)console->backlog) + (unsigned long)console);
}

#endif

