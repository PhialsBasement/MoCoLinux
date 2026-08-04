/* The WGL winsys virglrenderer borrows. See wgl_winsys.c. */
#ifndef MOCO_WGL_WINSYS_H
#define MOCO_WGL_WINSYS_H

#include <virgl/virglrenderer.h>

int  wgl_winsys_init(void);
void wgl_winsys_fini(void);

const char *wgl_winsys_renderer(void);
const char *wgl_winsys_version(void);

virgl_renderer_gl_context wgl_create_context(void *cookie, int scanout,
					     struct virgl_renderer_gl_ctx_param *p);
void wgl_destroy_context(void *cookie, virgl_renderer_gl_context ctx);
int  wgl_make_current(void *cookie, int scanout, virgl_renderer_gl_context ctx);

#endif
