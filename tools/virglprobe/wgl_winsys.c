/*
 * The WGL winsys virglrenderer borrows instead of bringing its own.
 *
 * virglrenderer normally initialises EGL or GLX for itself. Neither exists on
 * Windows, but it does not have to: virgl_renderer_callbacks lets the caller
 * supply create_gl_context / destroy_gl_context / make_current, and then the
 * library never touches a windowing system at all. That is the whole of the
 * Windows port -- the renderer proper is platform-agnostic.
 *
 * The context recipe is R1's, verified on both hosts: a throwaway 1.1 context
 * exists only so wglCreateContextAttribsARB can be found, because the function
 * that creates a modern context can only be looked up through an old one.
 *
 * Everything shares one hidden window and one DC. That is deliberate and it is
 * not a limitation: SetPixelFormat can be applied to a window exactly once,
 * ever, so a design that made a window per context would be one SetPixelFormat
 * failure away from a mysterious software fallback. Contexts are shared with a
 * base context via wglShareLists when virglrenderer asks for sharing, which it
 * does for its own resource contexts.
 *
 * Reused as-is by the cogpu daemon at R4; kept in its own file for that reason.
 */

#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>

#include "wgl_winsys.h"

#define WGL_CONTEXT_MAJOR_VERSION_ARB   0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB   0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB    0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB          0x00000001
#define WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 0x00000002

typedef HGLRC (WINAPI *PFNWGLCREATECONTEXTATTRIBSARB)(HDC, HGLRC, const int *);

static HWND  ws_wnd;
static HDC   ws_dc;
static HGLRC ws_base;          /* the share source, and the fallback current */
static PFNWGLCREATECONTEXTATTRIBSARB ws_createattribs;

static LRESULT CALLBACK ws_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	return DefWindowProc(h, m, w, l);
}

int wgl_winsys_init(void)
{
	WNDCLASSA wc;
	PIXELFORMATDESCRIPTOR pfd;
	int fmt;

	if (ws_base)
		return 0;

	ZeroMemory(&wc, sizeof(wc));
	wc.lpfnWndProc = ws_wndproc;
	wc.hInstance = GetModuleHandle(NULL);
	wc.lpszClassName = "moco_wgl_winsys";
	RegisterClassA(&wc);

	/* Never shown. virglrenderer renders into FBOs and the guest's frames are
	 * read back or presented elsewhere; this window exists only to own a
	 * pixel format, which is what a WGL context legally requires. */
	ws_wnd = CreateWindowExA(0, "moco_wgl_winsys", "moco_wgl_winsys",
				 WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
				 NULL, NULL, wc.hInstance, NULL);
	if (!ws_wnd) {
		fprintf(stderr, "winsys: CreateWindow failed (%lu)\n",
			(unsigned long)GetLastError());
		return -1;
	}

	ws_dc = GetDC(ws_wnd);
	if (!ws_dc) {
		fprintf(stderr, "winsys: GetDC failed\n");
		return -1;
	}

	ZeroMemory(&pfd, sizeof(pfd));
	pfd.nSize = sizeof(pfd);
	pfd.nVersion = 1;
	pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	pfd.iPixelType = PFD_TYPE_RGBA;
	pfd.cColorBits = 32;
	pfd.cDepthBits = 24;

	fmt = ChoosePixelFormat(ws_dc, &pfd);
	if (!fmt || !SetPixelFormat(ws_dc, fmt, &pfd)) {
		fprintf(stderr, "winsys: pixel format %d rejected (%lu)\n", fmt,
			(unsigned long)GetLastError());
		return -1;
	}

	{
		HGLRC tmp = wglCreateContext(ws_dc);

		if (!tmp || !wglMakeCurrent(ws_dc, tmp)) {
			fprintf(stderr, "winsys: legacy context failed (%lu)\n",
				(unsigned long)GetLastError());
			return -1;
		}

		ws_createattribs = (PFNWGLCREATECONTEXTATTRIBSARB)
			wglGetProcAddress("wglCreateContextAttribsARB");
		if (!ws_createattribs) {
			fprintf(stderr, "winsys: no wglCreateContextAttribsARB --"
				" software rasteriser or a pre-3.0 driver\n");
			return -1;
		}

		/* The base context is 3.3 core: it is only a share source and a
		 * safe thing to make current, so the lowest version virgl needs
		 * is the least surprising choice. */
		{
			int attribs[] = {
				WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
				WGL_CONTEXT_MINOR_VERSION_ARB, 3,
				WGL_CONTEXT_PROFILE_MASK_ARB,
					WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
				0
			};

			ws_base = ws_createattribs(ws_dc, NULL, attribs);
		}

		wglMakeCurrent(NULL, NULL);
		wglDeleteContext(tmp);

		if (!ws_base) {
			fprintf(stderr, "winsys: no 3.3 core context\n");
			return -1;
		}
		wglMakeCurrent(ws_dc, ws_base);
	}

	return 0;
}

void wgl_winsys_fini(void)
{
	wglMakeCurrent(NULL, NULL);
	if (ws_base) {
		wglDeleteContext(ws_base);
		ws_base = NULL;
	}
	if (ws_dc && ws_wnd) {
		ReleaseDC(ws_wnd, ws_dc);
		ws_dc = NULL;
	}
	if (ws_wnd) {
		DestroyWindow(ws_wnd);
		ws_wnd = NULL;
	}
}

const char *wgl_winsys_renderer(void)
{
	const char *s = (const char *)glGetString(GL_RENDERER);

	return s ? s : "(none)";
}

const char *wgl_winsys_version(void)
{
	const char *s = (const char *)glGetString(GL_VERSION);

	return s ? s : "(none)";
}

/* --------------------------------------------- the three virgl callbacks */

virgl_renderer_gl_context wgl_create_context(void *cookie, int scanout,
					     struct virgl_renderer_gl_ctx_param *p)
{
	int attribs[9];
	int n = 0;
	HGLRC rc;

	(void)cookie; (void)scanout;

	if (!ws_createattribs)
		return NULL;

	attribs[n++] = WGL_CONTEXT_MAJOR_VERSION_ARB;
	attribs[n++] = p->major_ver > 0 ? p->major_ver : 3;
	attribs[n++] = WGL_CONTEXT_MINOR_VERSION_ARB;
	attribs[n++] = p->major_ver > 0 ? p->minor_ver : 3;
	attribs[n++] = WGL_CONTEXT_PROFILE_MASK_ARB;
	/*
	 * compat_ctx is honoured rather than ignored: vrend asks for a
	 * compatibility profile for some resource paths, and quietly handing it
	 * a core context turns those into GL errors a long way from here.
	 */
	attribs[n++] = p->compat_ctx ? WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB
				     : WGL_CONTEXT_CORE_PROFILE_BIT_ARB;
	attribs[n++] = 0;

	rc = ws_createattribs(ws_dc, p->shared ? ws_base : NULL, attribs);
	if (!rc) {
		/* Asking for a version the driver will not give is normal --
		 * vrend probes downwards -- so this is not an error to shout
		 * about, only to report honestly. */
		return NULL;
	}

	return (virgl_renderer_gl_context)rc;
}

void wgl_destroy_context(void *cookie, virgl_renderer_gl_context ctx)
{
	(void)cookie;

	if (!ctx)
		return;
	/* A context cannot be deleted while it is current on this thread. */
	if (wglGetCurrentContext() == (HGLRC)ctx)
		wglMakeCurrent(ws_dc, ws_base);
	wglDeleteContext((HGLRC)ctx);
}

int wgl_make_current(void *cookie, int scanout, virgl_renderer_gl_context ctx)
{
	(void)cookie; (void)scanout;

	if (!wglMakeCurrent(ws_dc, ctx ? (HGLRC)ctx : ws_base))
		return -1;
	return 0;
}
