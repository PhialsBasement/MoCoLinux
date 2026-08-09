/* The WGL winsys virglrenderer borrows. See wgl_winsys.c. */
#ifndef MOCO_WGL_WINSYS_H
#define MOCO_WGL_WINSYS_H

#include <windows.h>
#include <virgl/virglrenderer.h>

/* A drawable context in the same GL share group as virglrenderer. */
struct wgl_window_context {
	HWND	window;
	HDC	dc;
	HGLRC	context;
};

/* The exact renderer context current before a presentation context borrows
 * the thread. Restoring merely ws_base is not equivalent: vrend may have left
 * a guest resource context current and expects to continue in that context. */
struct wgl_current_context {
	HDC	dc;
	HGLRC	context;
};

int  wgl_winsys_init(void);
void wgl_winsys_fini(void);

const char *wgl_winsys_renderer(void);
const char *wgl_winsys_version(void);

int  wgl_winsys_window_context_create(HWND window,
				      struct wgl_window_context *context);
int  wgl_winsys_window_context_make_current(
				      const struct wgl_window_context *context);
int  wgl_winsys_swap_interval(int interval);
void wgl_winsys_current_save(struct wgl_current_context *saved);
int  wgl_winsys_current_restore(const struct wgl_current_context *saved);
int  wgl_winsys_restore(void);
void wgl_winsys_window_context_destroy(struct wgl_window_context *context);

virgl_renderer_gl_context wgl_create_context(void *cookie, int scanout,
					     struct virgl_renderer_gl_ctx_param *p);
void wgl_destroy_context(void *cookie, virgl_renderer_gl_context ctx);
int  wgl_make_current(void *cookie, int scanout, virgl_renderer_gl_context ctx);

#endif
