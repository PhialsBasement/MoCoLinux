/*
 * moco-glbench -- one OpenGL workload for native WGL and guest GLX.
 *
 * The rendering code is deliberately shared.  _WIN32 changes only context,
 * window, event and clock plumbing, so the two reported frame rates compare
 * the same display-list geometry, fixed-function lighting and SwapBuffers.
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <GL/gl.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#else
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <time.h>
#endif

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct options {
	int width;
	int height;
	int spheres;
	int slices;
	int stacks;
	double warmup_seconds;
	double measure_seconds;
	int swap_interval;
	int finish_only;
	int dxgi_direct;
	int no_light;
};

struct platform {
	int running;
	int drawable_width;
	int drawable_height;
#ifdef _WIN32
	HWND window;
	HDC dc;
	HGLRC context;
	ID3D11Device *d3d_device;
	ID3D11DeviceContext *d3d_context;
	IDXGISwapChain *swap_chain;
	IDXGISwapChain3 *swap_chain3;
	ID3D11Texture2D *back_buffer[2];
	HANDLE interop_device;
	HANDLE interop_object[2];
	GLuint interop_texture[2];
	GLuint interop_fbo[2];
	GLuint interop_depth[2];
	unsigned int interop_index;
	int interop_locked;
	int interop_validated;
#else
	Display *display;
	Window window;
	GLXContext context;
	Atom wm_delete;
#endif
};

struct frame_stats {
	double *milliseconds;
	size_t count;
	size_t capacity;
	int allocation_failed;
};

static int compare_double(const void *left, const void *right)
{
	double a = *(const double *)left;
	double b = *(const double *)right;

	return (a > b) - (a < b);
}

static void frame_stats_add(struct frame_stats *stats, double seconds)
{
	if (!stats || stats->allocation_failed)
		return;
	if (stats->count == stats->capacity) {
		size_t capacity = stats->capacity ? stats->capacity * 2 : 16384;
		double *samples = (double *)realloc(stats->milliseconds,
			capacity * sizeof(*samples));

		if (!samples) {
			stats->allocation_failed = 1;
			return;
		}
		stats->milliseconds = samples;
		stats->capacity = capacity;
	}
	stats->milliseconds[stats->count++] = seconds * 1000.0;
}

static double frame_percentile(const struct frame_stats *stats, double p)
{
	size_t index;

	if (!stats->count)
		return 0.0;
	index = (size_t)ceil(p * (double)stats->count);
	if (index == 0)
		index = 1;
	if (index > stats->count)
		index = stats->count;
	return stats->milliseconds[index - 1];
}

static void frame_stats_report(struct frame_stats *stats)
{
	double p50;
	unsigned long long over_2x = 0;
	unsigned long long over_5ms = 0;
	unsigned long long over_16ms = 0;
	unsigned long long over_33ms = 0;
	size_t i;

	if (!stats->count)
		return;
	qsort(stats->milliseconds, stats->count, sizeof(*stats->milliseconds),
	      compare_double);
	p50 = frame_percentile(stats, 0.50);
	for (i = 0; i < stats->count; i++) {
		double ms = stats->milliseconds[i];

		over_2x += ms > p50 * 2.0;
		over_5ms += ms > 5.0;
		over_16ms += ms > 16.667;
		over_33ms += ms > 33.333;
	}
	printf("moco-glbench frametime_ms samples=%zu p50=%.3f p95=%.3f "
	       "p99=%.3f p99_9=%.3f max=%.3f\n",
	       stats->count, p50, frame_percentile(stats, 0.95),
	       frame_percentile(stats, 0.99),
	       frame_percentile(stats, 0.999),
	       stats->milliseconds[stats->count - 1]);
	printf("moco-glbench stalls gt_2x_p50=%llu gt_5ms=%llu "
	       "gt_16_667ms=%llu gt_33_333ms=%llu%s\n",
	       over_2x, over_5ms, over_16ms, over_33ms,
	       stats->allocation_failed ? " samples_truncated=1" : "");
}

static void usage(const char *program)
{
	fprintf(stderr,
		"usage: %s [--width N] [--height N] [--spheres N] "
		"[--slices N] [--stacks N]\n"
		"          [--warmup SECONDS] [--seconds SECONDS] "
			"[--swap-interval 0|1] [--frame-end swap|finish] "
			"[--present wgl|dxgi-direct]\n\n"
		"light: --spheres 61 --slices 32 --stacks 16 "
		"(62,464 triangles/frame)\n"
		"heavy: --spheres 61 --slices 240 --stacks 120 "
		"(3,513,600 triangles/frame)\n",
		program);
}

static int parse_int(const char *name, const char *text, int minimum,
		     int maximum, int *result)
{
	char *end = NULL;
	long value;

	errno = 0;
	value = strtol(text, &end, 10);
	if (errno || !end || *end || value < minimum || value > maximum) {
		fprintf(stderr, "invalid %s: %s\n", name, text);
		return 0;
	}
	*result = (int)value;
	return 1;
}

static int parse_double(const char *name, const char *text, double minimum,
			double maximum, double *result)
{
	char *end = NULL;
	double value;

	errno = 0;
	value = strtod(text, &end);
	if (errno || !end || *end || value < minimum || value > maximum) {
		fprintf(stderr, "invalid %s: %s\n", name, text);
		return 0;
	}
	*result = value;
	return 1;
}

static int parse_options(int argc, char **argv, struct options *options)
{
	int i;

	options->width = 1920;
	options->height = 1080;
	options->spheres = 61;
	options->slices = 32;
	options->stacks = 16;
	options->warmup_seconds = 2.0;
	options->measure_seconds = 10.0;
	options->swap_interval = 0;
	options->finish_only = 0;
	options->dxgi_direct = 0;

	for (i = 1; i < argc; i++) {
		const char *name = argv[i];
		const char *value;

		if (!strcmp(name, "--help") || !strcmp(name, "-h")) {
			usage(argv[0]);
			exit(0);
		}
		if (!strcmp(name, "--no-light")) {
			options->no_light = 1;
			continue;
		}
		if (i + 1 >= argc) {
			fprintf(stderr, "missing value after %s\n", name);
			return 0;
		}
		value = argv[++i];
		if (!strcmp(name, "--width")) {
			if (!parse_int(name, value, 64, 16384, &options->width))
				return 0;
		} else if (!strcmp(name, "--height")) {
			if (!parse_int(name, value, 64, 16384, &options->height))
				return 0;
		} else if (!strcmp(name, "--spheres")) {
			if (!parse_int(name, value, 1, 4096, &options->spheres))
				return 0;
		} else if (!strcmp(name, "--slices")) {
			if (!parse_int(name, value, 3, 2048, &options->slices))
				return 0;
		} else if (!strcmp(name, "--stacks")) {
			if (!parse_int(name, value, 2, 2048, &options->stacks))
				return 0;
		} else if (!strcmp(name, "--warmup")) {
			if (!parse_double(name, value, 0.0, 3600.0,
					  &options->warmup_seconds))
				return 0;
		} else if (!strcmp(name, "--seconds")) {
			if (!parse_double(name, value, 0.1, 3600.0,
					  &options->measure_seconds))
				return 0;
		} else if (!strcmp(name, "--swap-interval")) {
			if (!parse_int(name, value, 0, 1,
				       &options->swap_interval))
				return 0;
		} else if (!strcmp(name, "--frame-end")) {
			if (!strcmp(value, "swap"))
				options->finish_only = 0;
			else if (!strcmp(value, "finish"))
				options->finish_only = 1;
			else {
				fprintf(stderr, "invalid %s: %s\n", name, value);
				return 0;
			}
		} else if (!strcmp(name, "--present")) {
			if (!strcmp(value, "wgl"))
				options->dxgi_direct = 0;
			else if (!strcmp(value, "dxgi-direct"))
				options->dxgi_direct = 1;
			else {
				fprintf(stderr, "invalid %s: %s\n", name, value);
				return 0;
			}
		} else {
			fprintf(stderr, "unknown option: %s\n", name);
			return 0;
		}
	}
	return 1;
}

static double monotonic_seconds(void)
{
#ifdef _WIN32
	LARGE_INTEGER counter;
	LARGE_INTEGER frequency;

	QueryPerformanceCounter(&counter);
	QueryPerformanceFrequency(&frequency);
	return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
#endif
}

#ifdef _WIN32

typedef BOOL (WINAPI *PFNWGLSWAPINTERVALEXTPROC)(int interval);
typedef HANDLE (WINAPI *MOCO_PFNWGLDXOPENDEVICENV)(void *device);
typedef BOOL (WINAPI *MOCO_PFNWGLDXCLOSEDEVICENV)(HANDLE device);
typedef HANDLE (WINAPI *MOCO_PFNWGLDXREGISTEROBJECTNV)(HANDLE device,
	void *object, GLuint name, GLenum type, GLenum access);
typedef BOOL (WINAPI *MOCO_PFNWGLDXUNREGISTEROBJECTNV)(HANDLE device,
	HANDLE object);
typedef BOOL (WINAPI *MOCO_PFNWGLDXLOCKOBJECTSNV)(HANDLE device, GLint count,
	HANDLE *objects);
typedef BOOL (WINAPI *MOCO_PFNWGLDXUNLOCKOBJECTSNV)(HANDLE device, GLint count,
	HANDLE *objects);
typedef void (APIENTRY *MOCO_PFNGLGENFRAMEBUFFERS)(GLsizei count, GLuint *names);
typedef void (APIENTRY *MOCO_PFNGLBINDFRAMEBUFFER)(GLenum target, GLuint name);
typedef void (APIENTRY *MOCO_PFNGLFRAMEBUFFERTEXTURE2D)(GLenum target,
	GLenum attachment, GLenum textarget, GLuint texture, GLint level);
typedef GLenum (APIENTRY *MOCO_PFNGLCHECKFRAMEBUFFERSTATUS)(GLenum target);
typedef void (APIENTRY *MOCO_PFNGLDELETEFRAMEBUFFERS)(GLsizei count,
	const GLuint *names);
typedef void (APIENTRY *MOCO_PFNGLGENRENDERBUFFERS)(GLsizei count, GLuint *names);
typedef void (APIENTRY *MOCO_PFNGLBINDRENDERBUFFER)(GLenum target, GLuint name);
typedef void (APIENTRY *MOCO_PFNGLRENDERBUFFERSTORAGE)(GLenum target,
	GLenum format, GLsizei width, GLsizei height);
typedef void (APIENTRY *MOCO_PFNGLFRAMEBUFFERRENDERBUFFER)(GLenum target,
	GLenum attachment, GLenum renderbuffer_target, GLuint renderbuffer);
typedef void (APIENTRY *MOCO_PFNGLDELETERENDERBUFFERS)(GLsizei count,
	const GLuint *names);

#ifndef WGL_ACCESS_WRITE_DISCARD_NV
#define WGL_ACCESS_READ_WRITE_NV 0x0001
#define WGL_ACCESS_WRITE_DISCARD_NV 0x0002
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif

static MOCO_PFNWGLDXOPENDEVICENV dx_open_device;
static MOCO_PFNWGLDXCLOSEDEVICENV dx_close_device;
static MOCO_PFNWGLDXREGISTEROBJECTNV dx_register_object;
static MOCO_PFNWGLDXUNREGISTEROBJECTNV dx_unregister_object;
static MOCO_PFNWGLDXLOCKOBJECTSNV dx_lock_objects;
static MOCO_PFNWGLDXUNLOCKOBJECTSNV dx_unlock_objects;
static MOCO_PFNGLGENFRAMEBUFFERS moco_gl_gen_framebuffers;
static MOCO_PFNGLBINDFRAMEBUFFER moco_gl_bind_framebuffer;
static MOCO_PFNGLFRAMEBUFFERTEXTURE2D moco_gl_framebuffer_texture_2d;
static MOCO_PFNGLCHECKFRAMEBUFFERSTATUS moco_gl_check_framebuffer_status;
static MOCO_PFNGLDELETEFRAMEBUFFERS moco_gl_delete_framebuffers;
static MOCO_PFNGLGENRENDERBUFFERS moco_gl_gen_renderbuffers;
static MOCO_PFNGLBINDRENDERBUFFER moco_gl_bind_renderbuffer;
static MOCO_PFNGLRENDERBUFFERSTORAGE moco_gl_renderbuffer_storage;
static MOCO_PFNGLFRAMEBUFFERRENDERBUFFER moco_gl_framebuffer_renderbuffer;
static MOCO_PFNGLDELETERENDERBUFFERS moco_gl_delete_renderbuffers;

static int load_gl_proc(const char *name, void *function, size_t bytes)
{
	PROC address = wglGetProcAddress(name);

	if (!address || address == (PROC)1 || address == (PROC)2 ||
	    address == (PROC)3 || address == (PROC)-1 || bytes != sizeof(address))
		return 0;
	memcpy(function, &address, bytes);
	return 1;
}

static int platform_dxgi_create(struct platform *platform,
				const struct options *options)
{
	DXGI_SWAP_CHAIN_DESC swap_desc;
	D3D_FEATURE_LEVEL feature_level;
	HRESULT result;
	GLenum framebuffer_status;
	HANDLE object;
	unsigned int i;

	if (!load_gl_proc("wglDXOpenDeviceNV", &dx_open_device,
			  sizeof(dx_open_device)) ||
	    !load_gl_proc("wglDXCloseDeviceNV", &dx_close_device,
			  sizeof(dx_close_device)) ||
	    !load_gl_proc("wglDXRegisterObjectNV", &dx_register_object,
			  sizeof(dx_register_object)) ||
	    !load_gl_proc("wglDXUnregisterObjectNV", &dx_unregister_object,
			  sizeof(dx_unregister_object)) ||
	    !load_gl_proc("wglDXLockObjectsNV", &dx_lock_objects,
			  sizeof(dx_lock_objects)) ||
	    !load_gl_proc("wglDXUnlockObjectsNV", &dx_unlock_objects,
			  sizeof(dx_unlock_objects)) ||
	    !load_gl_proc("glGenFramebuffers", &moco_gl_gen_framebuffers,
			  sizeof(moco_gl_gen_framebuffers)) ||
	    !load_gl_proc("glBindFramebuffer", &moco_gl_bind_framebuffer,
			  sizeof(moco_gl_bind_framebuffer)) ||
	    !load_gl_proc("glFramebufferTexture2D", &moco_gl_framebuffer_texture_2d,
			  sizeof(moco_gl_framebuffer_texture_2d)) ||
	    !load_gl_proc("glCheckFramebufferStatus",
			  &moco_gl_check_framebuffer_status,
			  sizeof(moco_gl_check_framebuffer_status)) ||
	    !load_gl_proc("glDeleteFramebuffers", &moco_gl_delete_framebuffers,
			  sizeof(moco_gl_delete_framebuffers)) ||
	    !load_gl_proc("glGenRenderbuffers", &moco_gl_gen_renderbuffers,
			  sizeof(moco_gl_gen_renderbuffers)) ||
	    !load_gl_proc("glBindRenderbuffer", &moco_gl_bind_renderbuffer,
			  sizeof(moco_gl_bind_renderbuffer)) ||
	    !load_gl_proc("glRenderbufferStorage", &moco_gl_renderbuffer_storage,
			  sizeof(moco_gl_renderbuffer_storage)) ||
	    !load_gl_proc("glFramebufferRenderbuffer",
			  &moco_gl_framebuffer_renderbuffer,
			  sizeof(moco_gl_framebuffer_renderbuffer)) ||
	    !load_gl_proc("glDeleteRenderbuffers", &moco_gl_delete_renderbuffers,
			  sizeof(moco_gl_delete_renderbuffers))) {
		fprintf(stderr, "WGL_NV_DX_interop2 or framebuffer API unavailable\n");
		return 0;
	}

	memset(&swap_desc, 0, sizeof(swap_desc));
	swap_desc.BufferDesc.Width = (UINT)options->width;
	swap_desc.BufferDesc.Height = (UINT)options->height;
	swap_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swap_desc.SampleDesc.Count = 1;
	swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swap_desc.BufferCount = 1;
	swap_desc.OutputWindow = platform->window;
	swap_desc.Windowed = TRUE;
	swap_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	result = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE,
		NULL, 0, NULL, 0, D3D11_SDK_VERSION, &swap_desc,
		&platform->swap_chain, &platform->d3d_device, &feature_level,
		&platform->d3d_context);
	if (FAILED(result)) {
		fprintf(stderr, "D3D11CreateDeviceAndSwapChain failed: 0x%08lx\n",
			(unsigned long)result);
		return 0;
	}
	result = IDXGISwapChain_QueryInterface(platform->swap_chain,
		&IID_IDXGISwapChain3, (void **)&platform->swap_chain3);
	if (FAILED(result)) {
		fprintf(stderr, "IDXGISwapChain3 unavailable: 0x%08lx\n",
			(unsigned long)result);
		return 0;
	}
	platform->interop_device = dx_open_device(platform->d3d_device);
	if (!platform->interop_device) {
		fprintf(stderr, "wglDXOpenDeviceNV rejected the D3D11 device\n");
		return 0;
	}
	for (i = 0; i < 1; i++) {
		result = IDXGISwapChain3_GetBuffer(platform->swap_chain3, i,
			&IID_ID3D11Texture2D, (void **)&platform->back_buffer[i]);
		if (FAILED(result)) {
			fprintf(stderr, "IDXGISwapChain3::GetBuffer(%u) failed:"
				" 0x%08lx\n", i, (unsigned long)result);
			return 0;
		}
		moco_gl_gen_renderbuffers(1, &platform->interop_texture[i]);
		platform->interop_object[i] = dx_register_object(
			platform->interop_device, platform->back_buffer[i],
			platform->interop_texture[i], GL_RENDERBUFFER,
			WGL_ACCESS_READ_WRITE_NV);
		if (!platform->interop_object[i]) {
			fprintf(stderr, "wglDXRegisterObjectNV rejected swap buffer %u\n",
				i);
			return 0;
		}
		object = platform->interop_object[i];
		if (!dx_lock_objects(platform->interop_device, 1, &object)) {
			fprintf(stderr, "wglDXLockObjectsNV failed during setup\n");
			return 0;
		}
		platform->interop_index = i;
		platform->interop_locked = 1;
		moco_gl_gen_framebuffers(1, &platform->interop_fbo[i]);
		moco_gl_bind_framebuffer(GL_FRAMEBUFFER,
			platform->interop_fbo[i]);
		moco_gl_framebuffer_renderbuffer(GL_FRAMEBUFFER,
			GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
			platform->interop_texture[i]);
		moco_gl_gen_renderbuffers(1, &platform->interop_depth[i]);
		moco_gl_bind_renderbuffer(GL_RENDERBUFFER,
			platform->interop_depth[i]);
		moco_gl_renderbuffer_storage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24,
			options->width, options->height);
		moco_gl_framebuffer_renderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
			GL_RENDERBUFFER, platform->interop_depth[i]);
		framebuffer_status = moco_gl_check_framebuffer_status(GL_FRAMEBUFFER);
		moco_gl_bind_framebuffer(GL_FRAMEBUFFER, 0);
		object = platform->interop_object[i];
		if (!dx_unlock_objects(platform->interop_device, 1, &object)) {
			fprintf(stderr, "wglDXUnlockObjectsNV failed during setup\n");
			return 0;
		}
		platform->interop_locked = 0;
		if (framebuffer_status != GL_FRAMEBUFFER_COMPLETE) {
			fprintf(stderr, "DXGI interop framebuffer %u incomplete: 0x%x\n",
				i, (unsigned int)framebuffer_status);
			return 0;
		}
	}
	return 1;
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
				    LPARAM lparam)
{
	(void)wparam;
	(void)lparam;
	if (message == WM_CLOSE || message == WM_DESTROY) {
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcA(window, message, wparam, lparam);
}

static int platform_create(struct platform *platform,
			   const struct options *options)
{
	WNDCLASSA window_class;
	PIXELFORMATDESCRIPTOR pfd;
	RECT outer = { 0, 0, options->width, options->height };
	int pixel_format;

	memset(platform, 0, sizeof(*platform));
	memset(&window_class, 0, sizeof(window_class));
	window_class.style = CS_OWNDC;
	window_class.lpfnWndProc = window_proc;
	window_class.hInstance = GetModuleHandleA(NULL);
	window_class.hCursor = LoadCursor(NULL, IDC_ARROW);
	window_class.lpszClassName = "MoCoGLBench";
	if (!RegisterClassA(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
		fprintf(stderr, "RegisterClass failed: %lu\n",
			(unsigned long)GetLastError());
		return 0;
	}
	AdjustWindowRect(&outer, WS_OVERLAPPEDWINDOW, FALSE);
	platform->window = CreateWindowA(window_class.lpszClassName,
		options->dxgi_direct ? "MoCoLinux DXGI direct visible test" :
		"MoCoLinux native OpenGL benchmark", WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, outer.right - outer.left,
		outer.bottom - outer.top, NULL, NULL, window_class.hInstance,
		NULL);
	if (!platform->window) {
		fprintf(stderr, "CreateWindow failed: %lu\n",
			(unsigned long)GetLastError());
		return 0;
	}
	platform->dc = GetDC(platform->window);
	memset(&pfd, 0, sizeof(pfd));
	pfd.nSize = sizeof(pfd);
	pfd.nVersion = 1;
	pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	pfd.iPixelType = PFD_TYPE_RGBA;
	pfd.cColorBits = 32;
	pfd.cDepthBits = 24;
	pfd.iLayerType = PFD_MAIN_PLANE;
	pixel_format = ChoosePixelFormat(platform->dc, &pfd);
	if (!pixel_format || !SetPixelFormat(platform->dc, pixel_format, &pfd)) {
		fprintf(stderr, "pixel format setup failed: %lu\n",
			(unsigned long)GetLastError());
		return 0;
	}
	platform->context = wglCreateContext(platform->dc);
	if (!platform->context ||
	    !wglMakeCurrent(platform->dc, platform->context)) {
		fprintf(stderr, "WGL context creation failed: %lu\n",
			(unsigned long)GetLastError());
		return 0;
	}
	if (options->dxgi_direct && !platform_dxgi_create(platform, options))
		return 0;
	ShowWindow(platform->window, SW_SHOW);
	UpdateWindow(platform->window);
	if (!GetClientRect(platform->window, &outer)) {
		fprintf(stderr, "GetClientRect failed: %lu\n",
			(unsigned long)GetLastError());
		return 0;
	}
	platform->drawable_width = outer.right - outer.left;
	platform->drawable_height = outer.bottom - outer.top;
	platform->running = 1;
	return 1;
}

static void platform_set_swap_interval(struct platform *platform, int interval)
{
	PFNWGLSWAPINTERVALEXTPROC swap_interval;
	PROC address;

	(void)platform;
	address = wglGetProcAddress("wglSwapIntervalEXT");
	memcpy(&swap_interval, &address, sizeof(swap_interval));
	if (swap_interval)
		swap_interval(interval);
}

static int platform_pump(struct platform *platform)
{
	MSG message;

	while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
		if (message.message == WM_QUIT)
			platform->running = 0;
		TranslateMessage(&message);
		DispatchMessageA(&message);
	}
	return platform->running;
}

static int platform_begin_frame(struct platform *platform)
{
	HANDLE object;
	const unsigned int index = 0;

	if (!platform->swap_chain3)
		return 1;
	if (!platform->interop_object[index]) {
		fprintf(stderr, "DXGI back-buffer interop object is unavailable\n");
		platform->running = 0;
		return 0;
	}
	platform->interop_index = index;
	object = platform->interop_object[index];
	if (!dx_lock_objects(platform->interop_device, 1, &object)) {
		fprintf(stderr, "wglDXLockObjectsNV failed in frame loop\n");
		platform->running = 0;
		return 0;
	}
	platform->interop_locked = 1;
	moco_gl_bind_framebuffer(GL_FRAMEBUFFER, platform->interop_fbo[index]);
	return 1;
}

static int platform_unlock_interop(struct platform *platform)
{
	HANDLE object;

	if (!platform->interop_locked)
		return 1;
	moco_gl_bind_framebuffer(GL_FRAMEBUFFER, 0);
	object = platform->interop_object[platform->interop_index];
	if (!dx_unlock_objects(platform->interop_device, 1, &object)) {
		fprintf(stderr, "wglDXUnlockObjectsNV failed in frame loop\n");
		platform->running = 0;
		return 0;
	}
	platform->interop_locked = 0;
	return 1;
}

static int platform_validate_interop(struct platform *platform)
{
	D3D11_TEXTURE2D_DESC description;
	D3D11_MAPPED_SUBRESOURCE mapped;
	ID3D11Texture2D *staging = NULL;
	HRESULT result;
	unsigned int x, y;
	unsigned int x_step, y_step;
	unsigned int samples = 0;
	unsigned int coloured = 0;
	unsigned int maximum = 0;

	if (platform->interop_validated)
		return 1;
	ID3D11Texture2D_GetDesc(platform->back_buffer[0], &description);
	description.Usage = D3D11_USAGE_STAGING;
	description.BindFlags = 0;
	description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	description.MiscFlags = 0;
	result = ID3D11Device_CreateTexture2D(platform->d3d_device,
		&description, NULL, &staging);
	if (FAILED(result)) {
		fprintf(stderr, "D3D11 readback texture creation failed: 0x%08lx\n",
			(unsigned long)result);
		return 0;
	}
	ID3D11DeviceContext_CopyResource(platform->d3d_context,
		(ID3D11Resource *)staging,
		(ID3D11Resource *)platform->back_buffer[0]);
	memset(&mapped, 0, sizeof(mapped));
	result = ID3D11DeviceContext_Map(platform->d3d_context,
		(ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped);
	if (FAILED(result)) {
		fprintf(stderr, "D3D11 interop readback failed: 0x%08lx\n",
			(unsigned long)result);
		ID3D11Texture2D_Release(staging);
		return 0;
	}
	x_step = description.Width > 64 ? description.Width / 64 : 1;
	y_step = description.Height > 64 ? description.Height / 64 : 1;
	for (y = 0; y < description.Height; y += y_step) {
		const unsigned char *row = (const unsigned char *)mapped.pData +
			(size_t)y * mapped.RowPitch;

		for (x = 0; x < description.Width; x += x_step) {
			const unsigned char *pixel = row + (size_t)x * 4;
			unsigned int peak = pixel[0];

			if (pixel[1] > peak)
				peak = pixel[1];
			if (pixel[2] > peak)
				peak = pixel[2];
			if (peak > maximum)
				maximum = peak;
			coloured += peak >= 32;
			samples++;
		}
	}
	ID3D11DeviceContext_Unmap(platform->d3d_context,
		(ID3D11Resource *)staging, 0);
	ID3D11Texture2D_Release(staging);
	if (maximum < 32 || coloured == 0) {
		fprintf(stderr, "DXGI validation rejected black/empty buffer: "
			"max_rgb=%u coloured=%u/%u\n", maximum, coloured, samples);
		return 0;
	}
	platform->interop_validated = 1;
	printf("moco-glbench DXGI buffer validated max_rgb=%u coloured=%u/%u\n",
		maximum, coloured, samples);
	fflush(stdout);
	return 1;
}

static void platform_swap(struct platform *platform)
{
	if (!platform->swap_chain3) {
		SwapBuffers(platform->dc);
		return;
	}
	if (!platform_unlock_interop(platform))
		return;
	if (!platform_validate_interop(platform)) {
		platform->running = 0;
		return;
	}
	if (FAILED(IDXGISwapChain3_Present(platform->swap_chain3, 0, 0))) {
		fprintf(stderr, "IDXGISwapChain3::Present failed\n");
		platform->running = 0;
	}
}

static void platform_finish_frame(struct platform *platform)
{
	glFinish();
	(void)platform_unlock_interop(platform);
}

static void platform_destroy(struct platform *platform)
{
	unsigned int i;

	if (platform->interop_locked)
		(void)platform_unlock_interop(platform);
	for (i = 0; i < 2; i++) {
		if (platform->interop_object[i]) {
			HANDLE object = platform->interop_object[i];
			int locked = dx_lock_objects(platform->interop_device, 1,
				&object) != FALSE;

			moco_gl_bind_framebuffer(GL_FRAMEBUFFER, 0);
			if (platform->interop_fbo[i])
				moco_gl_delete_framebuffers(1,
					&platform->interop_fbo[i]);
			if (platform->interop_depth[i])
				moco_gl_delete_renderbuffers(1,
					&platform->interop_depth[i]);
			if (locked)
				dx_unlock_objects(platform->interop_device, 1, &object);
			dx_unregister_object(platform->interop_device,
				platform->interop_object[i]);
			platform->interop_object[i] = NULL;
		}
		if (platform->interop_texture[i])
			moco_gl_delete_renderbuffers(1,
				&platform->interop_texture[i]);
		if (platform->back_buffer[i])
			ID3D11Texture2D_Release(platform->back_buffer[i]);
	}
	if (platform->interop_device) {
		dx_close_device(platform->interop_device);
		platform->interop_device = NULL;
	}
	if (platform->swap_chain3)
		IDXGISwapChain3_Release(platform->swap_chain3);
	if (platform->swap_chain)
		IDXGISwapChain_Release(platform->swap_chain);
	if (platform->d3d_context)
		ID3D11DeviceContext_Release(platform->d3d_context);
	if (platform->d3d_device)
		ID3D11Device_Release(platform->d3d_device);
	if (platform->context) {
		wglMakeCurrent(NULL, NULL);
		wglDeleteContext(platform->context);
	}
	if (platform->dc && platform->window)
		ReleaseDC(platform->window, platform->dc);
	if (platform->window)
		DestroyWindow(platform->window);
}

static const char *platform_backend_name(const struct platform *platform)
{
	return platform->swap_chain3 ? "DXGI/WGL_NV_DX_interop2 direct" : "WGL";
}

#else

typedef void (*PFNGLXSWAPINTERVALEXTPROC)(Display *, GLXDrawable, int);
typedef int (*PFNGLXSWAPINTERVALMESAPROC)(unsigned int);

static int platform_create(struct platform *platform,
			   const struct options *options)
{
	static int visual_attributes[] = {
		GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 24, None
	};
	XVisualInfo *visual;
	XSetWindowAttributes attributes;
	XSizeHints size_hints;
	XWindowAttributes actual;
	Colormap colormap;

	memset(platform, 0, sizeof(*platform));
	platform->display = XOpenDisplay(NULL);
	if (!platform->display) {
		fprintf(stderr, "cannot open DISPLAY\n");
		return 0;
	}
	visual = glXChooseVisual(platform->display,
		DefaultScreen(platform->display), visual_attributes);
	if (!visual) {
		fprintf(stderr, "no double-buffered GLX visual\n");
		return 0;
	}
	colormap = XCreateColormap(platform->display,
		RootWindow(platform->display, visual->screen), visual->visual,
		AllocNone);
	attributes.colormap = colormap;
	attributes.event_mask = StructureNotifyMask | ExposureMask |
		KeyPressMask;
	platform->window = XCreateWindow(platform->display,
		RootWindow(platform->display, visual->screen), 0, 0,
		(unsigned int)options->width, (unsigned int)options->height, 0,
		visual->depth, InputOutput, visual->visual,
		CWColormap | CWEventMask, &attributes);
	XStoreName(platform->display, platform->window,
		   "MoCoLinux guest OpenGL benchmark");
	memset(&size_hints, 0, sizeof(size_hints));
	size_hints.flags = PMinSize | PMaxSize;
	size_hints.min_width = size_hints.max_width = options->width;
	size_hints.min_height = size_hints.max_height = options->height;
	XSetWMNormalHints(platform->display, platform->window, &size_hints);
	platform->wm_delete = XInternAtom(platform->display,
		"WM_DELETE_WINDOW", False);
	XSetWMProtocols(platform->display, platform->window,
		&platform->wm_delete, 1);
	platform->context = glXCreateContext(platform->display, visual, NULL, True);
	if (!platform->context ||
	    !glXMakeCurrent(platform->display, platform->window,
			    platform->context)) {
		fprintf(stderr, "direct GLX context creation failed\n");
		XFree(visual);
		return 0;
	}
	if (!glXIsDirect(platform->display, platform->context)) {
		fprintf(stderr, "GLX context is indirect; refusing incomparable run\n");
		XFree(visual);
		return 0;
	}
	XMapRaised(platform->display, platform->window);
	XSync(platform->display, False);
	if (!XGetWindowAttributes(platform->display, platform->window, &actual)) {
		fprintf(stderr, "could not query mapped GLX window geometry\n");
		XFree(visual);
		return 0;
	}
	platform->drawable_width = actual.width;
	platform->drawable_height = actual.height;
	XFree(visual);
	platform->running = 1;
	return 1;
}

static void platform_set_swap_interval(struct platform *platform, int interval)
{
	PFNGLXSWAPINTERVALEXTPROC swap_interval_ext;
	PFNGLXSWAPINTERVALMESAPROC swap_interval_mesa;

	swap_interval_ext = (PFNGLXSWAPINTERVALEXTPROC)
		glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalEXT");
	if (swap_interval_ext) {
		swap_interval_ext(platform->display, platform->window, interval);
		return;
	}
	swap_interval_mesa = (PFNGLXSWAPINTERVALMESAPROC)
		glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalMESA");
	if (swap_interval_mesa)
		(void)swap_interval_mesa((unsigned int)interval);
}

static int platform_pump(struct platform *platform)
{
	while (XPending(platform->display)) {
		XEvent event;

		XNextEvent(platform->display, &event);
		if (event.type == ClientMessage &&
		    (Atom)event.xclient.data.l[0] == platform->wm_delete)
			platform->running = 0;
		if (event.type == DestroyNotify)
			platform->running = 0;
	}
	return platform->running;
}

static int platform_begin_frame(struct platform *platform)
{
	(void)platform;
	return 1;
}

static void platform_swap(struct platform *platform)
{
	glXSwapBuffers(platform->display, platform->window);
}

static void platform_finish_frame(struct platform *platform)
{
	(void)platform;
	glFinish();
}

static void platform_destroy(struct platform *platform)
{
	if (!platform->display)
		return;
	glXMakeCurrent(platform->display, None, NULL);
	if (platform->context)
		glXDestroyContext(platform->display, platform->context);
	if (platform->window)
		XDestroyWindow(platform->display, platform->window);
	XCloseDisplay(platform->display);
}

static const char *platform_backend_name(const struct platform *platform)
{
	(void)platform;
	return "GLX";
}

#endif

static GLuint make_sphere_list(int slices, int stacks)
{
	GLuint list = glGenLists(1);
	int stack;

	if (!list)
		return 0;
	glNewList(list, GL_COMPILE);
	for (stack = 0; stack < stacks; stack++) {
		double latitude0 = -M_PI * 0.5 + M_PI * stack / stacks;
		double latitude1 = -M_PI * 0.5 + M_PI * (stack + 1) / stacks;
		double z0 = sin(latitude0);
		double z1 = sin(latitude1);
		double radius0 = cos(latitude0);
		double radius1 = cos(latitude1);
		int slice;

		glBegin(GL_TRIANGLE_STRIP);
		for (slice = 0; slice <= slices; slice++) {
			double longitude = 2.0 * M_PI * slice / slices;
			double x = cos(longitude);
			double y = sin(longitude);

			glNormal3d(x * radius0, y * radius0, z0);
			glVertex3d(x * radius0, y * radius0, z0);
			glNormal3d(x * radius1, y * radius1, z1);
			glVertex3d(x * radius1, y * radius1, z1);
		}
		glEnd();
	}
	glEndList();
	return list;
}

static void configure_scene(const struct options *options)
{
	GLfloat light_position[] = { -3.0f, 5.0f, 8.0f, 1.0f };
	GLfloat diffuse[] = { 0.85f, 0.90f, 1.0f, 1.0f };
	double aspect = (double)options->width / options->height;
	double top = tan(60.0 * M_PI / 360.0);

	glViewport(0, 0, options->width, options->height);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glFrustum(-top * aspect, top * aspect, -top, top, 1.0, 100.0);
	glMatrixMode(GL_MODELVIEW);
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);
	/* --no-light isolates the fixed-function lighting's vertex-shader cost:
	 * identical geometry, fetch and raster, minus the per-vertex ALU. */
	if (!options->no_light) {
		glEnable(GL_LIGHTING);
		glEnable(GL_LIGHT0);
		glLightfv(GL_LIGHT0, GL_POSITION, light_position);
		glLightfv(GL_LIGHT0, GL_DIFFUSE, diffuse);
		glEnable(GL_COLOR_MATERIAL);
		glColorMaterial(GL_FRONT, GL_AMBIENT_AND_DIFFUSE);
	}
	glClearColor(0.015f, 0.020f, 0.035f, 1.0f);
}

static void draw_frame(GLuint sphere, const struct options *options,
		       unsigned long long frame)
{
	int columns = (int)ceil(sqrt((double)options->spheres));
	int rows = (options->spheres + columns - 1) / columns;
	float spacing = 1.45f;
	float scene_width = (columns - 1) * spacing;
	float scene_height = (rows - 1) * spacing;
	float distance = 1.25f * (scene_width > scene_height ?
				 scene_width : scene_height) + 4.0f;
	int i;

	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glTranslatef(0.0f, 0.0f, -distance);
	glRotatef((float)(frame % 3600) * 0.10f, 0.25f, 1.0f, 0.0f);
	for (i = 0; i < options->spheres; i++) {
		int column = i % columns;
		int row = i / columns;
		float x = column * spacing - scene_width * 0.5f;
		float y = row * spacing - scene_height * 0.5f;
		float z = (float)((i * 17) % 7 - 3) * 0.18f;
		float red = 0.25f + 0.65f * (float)((i * 37) % 101) / 100.0f;
		float green = 0.25f + 0.65f * (float)((i * 53) % 101) / 100.0f;
		float blue = 0.25f + 0.65f * (float)((i * 71) % 101) / 100.0f;

		glPushMatrix();
		glTranslatef(x, y, z);
		glScalef(0.58f, 0.58f, 0.58f);
		glColor3f(red, green, blue);
		glCallList(sphere);
		glPopMatrix();
	}
}

static unsigned long long run_for(struct platform *platform, GLuint sphere,
				  const struct options *options,
				  double seconds,
				  unsigned long long first_frame,
				  struct frame_stats *stats)
{
	double deadline = monotonic_seconds() + seconds;
	double previous = monotonic_seconds();
	unsigned long long frame = first_frame;

	while (platform_pump(platform) && monotonic_seconds() < deadline) {
		double now;

		if (!platform_begin_frame(platform))
			break;
		draw_frame(sphere, options, frame++);
		if (options->finish_only)
			platform_finish_frame(platform);
		else
			platform_swap(platform);
		now = monotonic_seconds();
		frame_stats_add(stats, now - previous);
		previous = now;
	}
	return frame;
}

int main(int argc, char **argv)
{
	struct options options;
	struct platform platform;
	struct frame_stats stats;
	GLuint sphere;
	unsigned long long frame = 0;
	unsigned long long measured_start;
	unsigned long long measured_frames;
	unsigned long long triangles_per_frame;
	double start;
	double end;
	double elapsed;
	double fps;

	if (!parse_options(argc, argv, &options)) {
		usage(argv[0]);
		return 2;
	}
#ifndef _WIN32
	if (options.dxgi_direct) {
		fprintf(stderr, "--present dxgi-direct is a Windows host benchmark\n");
		return 2;
	}
#endif
	memset(&stats, 0, sizeof(stats));
	if (!platform_create(&platform, &options))
		return 1;
	if (platform.drawable_width != options.width ||
	    platform.drawable_height != options.height) {
		fprintf(stderr,
			"requested %dx%d drawable was constrained to %dx%d; "
			"choose a size that fits both systems\n",
			options.width, options.height, platform.drawable_width,
			platform.drawable_height);
		platform_destroy(&platform);
		return 1;
	}
	platform_set_swap_interval(&platform, options.swap_interval);
	configure_scene(&options);
	sphere = make_sphere_list(options.slices, options.stacks);
	if (!sphere) {
		fprintf(stderr, "could not create sphere display list\n");
		platform_destroy(&platform);
		return 1;
	}
	triangles_per_frame = (unsigned long long)options.spheres *
		(unsigned long long)options.slices *
		(unsigned long long)options.stacks * 2ULL;

	printf("moco-glbench backend=%s renderer=\"%s\" version=\"%s\"\n",
		platform_backend_name(&platform),
		(const char *)glGetString(GL_RENDERER),
		(const char *)glGetString(GL_VERSION));
	printf("moco-glbench resolution=%dx%d spheres=%d slices=%d stacks=%d "
		"triangles_per_frame=%llu swap_interval=%d frame_end=%s\n",
		options.width, options.height, options.spheres, options.slices,
		options.stacks, triangles_per_frame, options.swap_interval,
		options.finish_only ? "finish" : "swap");
	fflush(stdout);

	frame = run_for(&platform, sphere, &options, options.warmup_seconds,
			frame, NULL);
	glFinish();
	measured_start = frame;
	start = monotonic_seconds();
	frame = run_for(&platform, sphere, &options, options.measure_seconds,
			frame, &stats);
	glFinish();
	end = monotonic_seconds();
	elapsed = end - start;
	measured_frames = frame - measured_start;
	if (elapsed <= 0.0 || measured_frames == 0) {
		fprintf(stderr, "benchmark window closed before a frame was measured\n");
		glDeleteLists(sphere, 1);
		platform_destroy(&platform);
		return 1;
	}
	fps = (double)measured_frames / elapsed;
	printf("moco-glbench result frames=%llu seconds=%.6f fps=%.3f "
	       "mpixels_per_second=%.3f mtriangles_per_second=%.3f\n",
	       measured_frames, elapsed, fps,
	       (double)options.width * (double)options.height * fps / 1000000.0,
	       (double)triangles_per_frame * fps / 1000000.0);
	frame_stats_report(&stats);
	fflush(stdout);

	free(stats.milliseconds);
	glDeleteLists(sphere, 1);
	platform_destroy(&platform);
	return 0;
}
