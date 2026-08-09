/*
 * R0/R1: present inside a stock VcXsrv window without patching the server.
 *
 * The first section is R0's synthetic diagnostic. R1 at the bottom connects
 * that proven seam to a live guest-created virglrenderer resource. VcXsrv owns
 * the outer HWND and all X11/window-manager semantics. Cogpu discovers that
 * HWND through the XID property VcXsrv already publishes, then owns an
 * independent, disabled WGL popup positioned immediately above that HWND.
 * It deliberately has neither a parent nor an owner in VcXsrv's process:
 * making cogpu's drawable a cross-process child wedges VcXsrv's modal resize
 * path after repeated resizes. Desktop Z-order supplies occlusion without
 * placing cogpu inside VcXsrv's window hierarchy.
 *
 * If this coexistence probe does not survive move, resize, focus, typing and
 * occlusion, direct presentation stops here. No guest ABI has been committed.
 */

#include <windows.h>
#include <epoxy/gl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../../user/copresent/copresent.h"
#include "present.h"
#include "vrend.h"
#include "wgl_winsys.h"

#define VCXSRV_WINDOW_CLASS "vcxsrv/x X rl"
#define VCXSRV_XID_PROPERTY "vcxsrv_wid_prop_rl"
#define PRESENTER_CLASS     "moco_direct_present"

void logline(const char *fmt, ...);

struct vcxsrv_candidate {
	HWND		window;
	uintptr_t	xid;
	unsigned long	area;
	unsigned long	score;
	int		foreground;
};

struct probe_scene {
	GLuint	program;
	GLuint	vao;
	GLint	time_uniform;
	GLint	size_uniform;
};

static const char probe_vertex_shader[] =
	"#version 330 core\n"
	"out vec2 uv;\n"
	"void main() {\n"
	"  vec2 p = vec2(gl_VertexID == 1 ? 2.0 : 0.0,\n"
	"                gl_VertexID == 2 ? 2.0 : 0.0);\n"
	"  uv = p;\n"
	"  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";

/*
 * A diagnostic animation rather than a clear colour. The static one-pixel
 * grid, checkerboard and colour bars expose scaling or format damage; four
 * unrelated moving silhouettes make cadence, tearing and stale frames visible
 * without needing the guest-rendered resource that arrives in R1.
 */
static const char probe_fragment_shader[] =
	"#version 330 core\n"
	"in vec2 uv;\n"
	"out vec4 pixel;\n"
	"uniform float seconds;\n"
	"uniform vec2 size;\n"
	"float box_sdf(vec2 p, vec2 b) {\n"
	"  vec2 q = abs(p) - b;\n"
	"  return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);\n"
	"}\n"
	"float triangle_sdf(vec2 p, float r) {\n"
	"  const float k = 1.7320508;\n"
	"  p.x = abs(p.x) - r;\n"
	"  p.y = p.y + r / k;\n"
	"  if (p.x + k * p.y > 0.0)\n"
	"    p = vec2(p.x - k * p.y, -k * p.x - p.y) * 0.5;\n"
	"  p.x -= clamp(p.x, -2.0 * r, 0.0);\n"
	"  return -length(p) * sign(p.y);\n"
	"}\n"
	"float shape(float d) {\n"
	"  float edge = 1.5 / size.y;\n"
	"  return 1.0 - smoothstep(-edge, edge, d);\n"
	"}\n"
	"void main() {\n"
	"  vec2 frag = gl_FragCoord.xy;\n"
	"  vec2 p = (frag - size * 0.5) / size.y;\n"
	"  vec3 c = mix(vec3(0.025, 0.040, 0.075),\n"
	"               vec3(0.080, 0.125, 0.190), uv.y);\n"
	"  float dx = min(mod(frag.x, 32.0), 32.0 - mod(frag.x, 32.0));\n"
	"  float dy = min(mod(frag.y, 32.0), 32.0 - mod(frag.y, 32.0));\n"
	"  float grid = 1.0 - smoothstep(0.0, 1.25, min(dx, dy));\n"
	"  c += vec3(0.085, 0.105, 0.130) * grid;\n"
	"  if (uv.x < 0.22 && uv.y > 0.78) {\n"
	"    float check = mod(floor(frag.x / 8.0) + floor(frag.y / 8.0), 2.0);\n"
	"    c = mix(vec3(0.04), vec3(0.96), check);\n"
	"  }\n"
	"  if (uv.y < 0.075) {\n"
	"    float band = floor(uv.x * 7.0);\n"
	"    if (band < 1.0) c = vec3(1.0, 0.12, 0.10);\n"
	"    else if (band < 2.0) c = vec3(1.0, 0.72, 0.08);\n"
	"    else if (band < 3.0) c = vec3(0.95, 0.95, 0.12);\n"
	"    else if (band < 4.0) c = vec3(0.10, 0.90, 0.28);\n"
	"    else if (band < 5.0) c = vec3(0.08, 0.75, 1.0);\n"
	"    else if (band < 6.0) c = vec3(0.26, 0.25, 1.0);\n"
	"    else c = vec3(0.88, 0.18, 1.0);\n"
	"  }\n"
	"  vec2 circle_at = vec2(0.43 * sin(seconds * 1.13), 0.24);\n"
	"  float circle = shape(length(p - circle_at) - 0.105);\n"
	"  c = mix(c, vec3(1.00, 0.18, 0.06), circle);\n"
	"  vec2 square_at = vec2(0.34 * cos(seconds * 0.73), -0.22);\n"
	"  vec2 sq = p - square_at;\n"
	"  float ca = cos(seconds * 1.45), sa = sin(seconds * 1.45);\n"
	"  sq = mat2(ca, -sa, sa, ca) * sq;\n"
	"  float square = shape(box_sdf(sq, vec2(0.095)));\n"
	"  c = mix(c, vec3(0.10, 0.94, 0.44), square);\n"
	"  vec2 tri_at = vec2(-0.37, 0.29 * sin(seconds * 0.91));\n"
	"  vec2 tp = p - tri_at;\n"
	"  float ct = cos(-seconds * 0.82), st = sin(-seconds * 0.82);\n"
	"  tp = mat2(ct, -st, st, ct) * tp;\n"
	"  float triangle = shape(triangle_sdf(tp, 0.115));\n"
	"  c = mix(c, vec3(0.18, 0.48, 1.00), triangle);\n"
	"  vec2 ring_at = vec2(0.39 * cos(seconds * 0.62),\n"
	"                      0.31 * sin(seconds * 0.62));\n"
	"  float ring = shape(abs(length(p - ring_at) - 0.080) - 0.014);\n"
	"  c = mix(c, vec3(1.00, 0.88, 0.12), ring);\n"
	"  float scan_x = mod(seconds * 145.0, size.x);\n"
	"  float scan = 1.0 - smoothstep(0.0, 2.0, abs(frag.x - scan_x));\n"
	"  c = mix(c, vec3(1.0), scan * 0.55);\n"
	"  pixel = vec4(c, 1.0);\n"
	"}\n";

static GLuint compile_shader(const char *owner, GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	GLint ok = 0;

	if (!shader)
		return 0;
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char message[2048];
		GLsizei length = 0;

		message[0] = '\0';
		glGetShaderInfoLog(shader, sizeof(message) - 1, &length, message);
		message[sizeof(message) - 1] = '\0';
		logline("%s: shader compile failed: %s\n", owner, message);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static int probe_scene_init(struct probe_scene *scene)
{
	GLuint vertex = 0, fragment = 0;
	GLint ok = 0;
	char message[2048];
	GLsizei length = 0;

	ZeroMemory(scene, sizeof(*scene));
	vertex = compile_shader("present probe", GL_VERTEX_SHADER,
				probe_vertex_shader);
	fragment = compile_shader("present probe", GL_FRAGMENT_SHADER,
				  probe_fragment_shader);
	if (!vertex || !fragment)
		goto fail;

	scene->program = glCreateProgram();
	if (!scene->program)
		goto fail;
	glAttachShader(scene->program, vertex);
	glAttachShader(scene->program, fragment);
	glLinkProgram(scene->program);
	glGetProgramiv(scene->program, GL_LINK_STATUS, &ok);
	if (!ok) {
		message[0] = '\0';
		glGetProgramInfoLog(scene->program, sizeof(message) - 1,
				    &length, message);
		message[sizeof(message) - 1] = '\0';
		logline("present probe: shader link failed: %s\n", message);
		goto fail;
	}

	scene->time_uniform = glGetUniformLocation(scene->program, "seconds");
	scene->size_uniform = glGetUniformLocation(scene->program, "size");
	glGenVertexArrays(1, &scene->vao);
	if (!scene->vao || scene->time_uniform < 0 || scene->size_uniform < 0)
		goto fail;

	glDeleteShader(vertex);
	glDeleteShader(fragment);
	return 0;

fail:
	if (vertex)
		glDeleteShader(vertex);
	if (fragment)
		glDeleteShader(fragment);
	if (scene->program)
		glDeleteProgram(scene->program);
	ZeroMemory(scene, sizeof(*scene));
	return -1;
}

static void probe_scene_draw(const struct probe_scene *scene, int width,
			     int height, float seconds)
{
	glViewport(0, 0, width, height);
	glUseProgram(scene->program);
	glUniform1f(scene->time_uniform, seconds);
	glUniform2f(scene->size_uniform, (float)width, (float)height);
	glBindVertexArray(scene->vao);
	glDrawArrays(GL_TRIANGLES, 0, 3);
}

static void probe_scene_fini(struct probe_scene *scene)
{
	if (scene->vao)
		glDeleteVertexArrays(1, &scene->vao);
	if (scene->program)
		glDeleteProgram(scene->program);
	ZeroMemory(scene, sizeof(*scene));
}

static BOOL CALLBACK find_vcxsrv_window(HWND window, LPARAM opaque)
{
	struct vcxsrv_candidate *best = (struct vcxsrv_candidate *)opaque;
	struct vcxsrv_candidate candidate;
	RECT rect;
	char class_name[64];
	HANDLE xid;
	LONG width, height;

	if (!IsWindowVisible(window))
		return TRUE;
	if (!GetClassNameA(window, class_name, sizeof(class_name)) ||
	    strcmp(class_name, VCXSRV_WINDOW_CLASS) != 0)
		return TRUE;

	xid = GetPropA(window, VCXSRV_XID_PROPERTY);
	if (!xid || !GetClientRect(window, &rect))
		return TRUE;
	width = rect.right - rect.left;
	height = rect.bottom - rect.top;
	if (width <= 0 || height <= 0)
		return TRUE;

	candidate.window = window;
	candidate.xid = (uintptr_t)xid;
	candidate.area = (unsigned long)width * (unsigned long)height;
	candidate.score = 0;
	candidate.foreground = GetForegroundWindow() == window;

	/* Prefer the active X window. If the probe was launched from a console,
	 * choose the largest visible X client rather than EnumWindows ordering. */
	if (!best->window ||
	    (candidate.foreground && !best->foreground) ||
	    (candidate.foreground == best->foreground &&
	     candidate.area > best->area))
		*best = candidate;

	return TRUE;
}

static LRESULT CALLBACK presenter_wndproc(HWND window, UINT message,
					  WPARAM wparam, LPARAM lparam)
{
	(void)wparam;
	(void)lparam;

	if (message == WM_ERASEBKGND)
		return 1;
	if (message == WM_MOUSEACTIVATE)
		return MA_NOACTIVATE;
	if (message == WM_NCHITTEST)
		return HTTRANSPARENT;
	if (message == WM_PAINT) {
		PAINTSTRUCT paint;

		BeginPaint(window, &paint);
		EndPaint(window, &paint);
		return 0;
	}
	return DefWindowProc(window, message, wparam, lparam);
}

static int register_presenter_class(void)
{
	WNDCLASSA wc;

	ZeroMemory(&wc, sizeof(wc));
	wc.style = CS_OWNDC;
	wc.lpfnWndProc = presenter_wndproc;
	wc.hInstance = GetModuleHandle(NULL);
	wc.lpszClassName = PRESENTER_CLASS;
	if (RegisterClassA(&wc))
		return 0;
	if (GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
		return 0;
	return -1;
}

static int pump_messages(void)
{
	MSG message;

	while (PeekMessage(&message, NULL, 0, 0, PM_REMOVE)) {
		if (message.message == WM_QUIT)
			return -1;
		TranslateMessage(&message);
		DispatchMessage(&message);
	}
	return 0;
}

int cogpu_present_probe(unsigned int duration_ms)
{
	struct vcxsrv_candidate target;
	struct wgl_window_context context;
	struct probe_scene scene;
	HWND child = NULL;
	RECT rect;
	DWORD start;
	DWORD now;
	DWORD elapsed;
	DWORD pid = 0;
	int width, height;
	int rc = 1;
	unsigned int frames = 0;

	ZeroMemory(&target, sizeof(target));
	ZeroMemory(&context, sizeof(context));
	ZeroMemory(&scene, sizeof(scene));
	EnumWindows(find_vcxsrv_window, (LPARAM)&target);
	if (!target.window) {
		logline("present probe: no visible stock VcXsrv multiwindow HWND found\n");
		logline("present probe: open an X window on :0, then run this again\n");
		return 1;
	}

	GetWindowThreadProcessId(target.window, &pid);
	logline("present probe: VcXsrv HWND %p, XID 0x%lx, pid %lu\n",
		(void *)target.window, (unsigned long)target.xid,
		(unsigned long)pid);

	if (register_presenter_class() != 0) {
		logline("present probe: RegisterClass failed (%lu)\n",
			(unsigned long)GetLastError());
		return 1;
	}
	if (!GetClientRect(target.window, &rect)) {
		logline("present probe: target disappeared before create\n");
		return 1;
	}
	width = rect.right - rect.left;
	height = rect.bottom - rect.top;

	/* This child belongs to cogpu but its parent belongs to VcXsrv. Disabled
	 * is essential: keyboard and mouse ownership must not move out of X11. */
	child = CreateWindowExA(WS_EX_NOPARENTNOTIFY,
		PRESENTER_CLASS, "", WS_CHILD | WS_VISIBLE | WS_DISABLED |
		WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
		0, 0, width, height, target.window, NULL,
		GetModuleHandle(NULL), NULL);
	if (!child) {
		logline("present probe: cross-process child create failed (%lu)\n",
			(unsigned long)GetLastError());
		return 1;
	}

	SetWindowPos(child, HWND_TOP, 0, 0, width, height,
		SWP_NOACTIVATE | SWP_SHOWWINDOW);
	if (wgl_winsys_window_context_create(child, &context) != 0) {
		logline("present probe: shared WGL drawable could not be created\n");
		goto out;
	}
	if (wgl_winsys_window_context_make_current(&context) != 0) {
		logline("present probe: shared WGL drawable could not be made current\n");
		goto out;
	}
	if (probe_scene_init(&scene) != 0) {
		logline("present probe: diagnostic scene could not be created\n");
		goto out;
	}

	logline("present probe: diagnostic animation for %u ms; move, resize,"
		" type into and"
		" occlude this X window now\n", duration_ms);
	start = GetTickCount();
	for (;;) {
		now = GetTickCount();
		elapsed = now - start;
		if (elapsed >= duration_ms) {
			rc = 0;
			break;
		}
		if (pump_messages() != 0 || !IsWindow(target.window)) {
			logline("present probe: target closed during test\n");
			break;
		}
		if (!GetClientRect(target.window, &rect)) {
			logline("present probe: target rect became unavailable\n");
			break;
		}
		width = rect.right - rect.left;
		height = rect.bottom - rect.top;
		if (width <= 0 || height <= 0) {
			Sleep(16);
			continue;
		}
		SetWindowPos(child, HWND_TOP, 0, 0, width, height,
			SWP_NOACTIVATE | SWP_SHOWWINDOW);

		probe_scene_draw(&scene, width, height,
				 (float)elapsed / 1000.0f);
		if (!SwapBuffers(context.dc)) {
			logline("present probe: SwapBuffers failed (%lu)\n",
				(unsigned long)GetLastError());
			break;
		}
		frames++;
		/* SwapBuffers normally provides the display cadence. Cap an immediate
		 * implementation to 60 Hz as well, so the probe is comparable and does
		 * not turn a visual test into a busy-loop benchmark. */
		{
			DWORD target_ms = (DWORD)(((uint64_t)frames * 1000) / 60);
			DWORD actual_ms = GetTickCount() - start;

			if (target_ms > actual_ms)
				Sleep(target_ms - actual_ms);
		}
	}

out:
	if (frames) {
		DWORD total_ms = GetTickCount() - start;
		double fps = total_ms ? (double)frames * 1000.0 / total_ms : 0.0;

		logline("present probe: %u frames in %.2f s, %.1f fps\n",
			frames, (double)total_ms / 1000.0, fps);
	}
	probe_scene_fini(&scene);
	wgl_winsys_window_context_destroy(&context);
	if (child)
		DestroyWindow(child);
	if (IsWindow(target.window))
		InvalidateRect(target.window, NULL, TRUE);
	if (rc == 0)
		logline("present probe: API PASS; child removed, VcXsrv owns the window"
			" again; confirm the visual/input checks above\n");
	else
		logline("present probe: FAIL; child removed\n");
	return rc;
}

/* ------------------------------------------------ actual guest texture (R1) */

/* Gallium values retained from RESOURCE_CREATE_3D. Keep these local rather
 * than making cogpu depend on virglrenderer's private Gallium headers. */
#define COGPU_PIPE_TEXTURE_2D          2u
#define COGPU_PIPE_BIND_DEPTH_STENCIL (1u << 0)
#define COGPU_PIPE_BIND_RENDER_TARGET (1u << 1)
#define COGPU_VIRGL_RESOURCE_Y_0_TOP  (1u << 0)

struct r1_scene {
	GLuint program;
	GLuint vao;
	GLuint sampler;
	GLint texture_uniform;
	GLint flip_uniform;
};

struct r1_presenter {
	int enabled;
	int rung;
	HWND parent;
	HWND overlay;
	HWINEVENTHOOK movesize_hook;
	uintptr_t xid;
	struct wgl_window_context context;
	struct r1_scene scene;
	uint32_t resource_id;
	uint32_t texture_id;
	int drawable_x;
	int drawable_y;
	int drawable_width;
	int drawable_height;
	int resize_width;
	int resize_height;
	int resize_pending;
	int live_sizing;
	int surface_hidden;
	unsigned int frames;
	DWORD first_frame;
	DWORD last_report;
	DWORD last_miss_report;
	DWORD resize_last_change;
	DWORD last_poll;
};

#define R2_MAX_BINDINGS 64
struct r2_binding {
	int active;
	uint32_t generation;
	uint32_t connection;
	uint32_t buffer;
	uint32_t xid;
	struct cogpu_vrend_resource_info resource;
};

struct vcxsrv_match {
	struct vcxsrv_candidate best;
	unsigned int width;
	unsigned int height;
};

static struct r1_presenter r1;
static struct r2_binding r2_bindings[R2_MAX_BINDINGS];
static int r2_enabled;
static uint32_t r2_generation;

static const char *present_rung(void)
{
	return r1.rung == 2 ? "R2" : "R1";
}

static void r1_hide_surface(void)
{
	if (!r1.surface_hidden && r1.overlay && IsWindow(r1.overlay))
		ShowWindow(r1.overlay, SW_HIDE);
	r1.surface_hidden = 1;
}

static void CALLBACK r1_movesize_event(HWINEVENTHOOK hook, DWORD event,
				       HWND window, LONG object_id,
				       LONG child_id, DWORD thread_id,
				       DWORD event_time)
{
	(void)hook;
	(void)object_id;
	(void)child_id;
	(void)thread_id;
	(void)event_time;

	if (window != r1.parent)
		return;
	if (event == EVENT_SYSTEM_MOVESIZESTART) {
		r1.live_sizing = 1;
		r1.resize_pending = 1;
		r1.resize_width = r1.drawable_width;
		r1.resize_height = r1.drawable_height;
		r1.resize_last_change = GetTickCount();
		r1_hide_surface();
		logline("present %s: live move/resize started; presentation paused\n",
			present_rung());
	} else if (event == EVENT_SYSTEM_MOVESIZEEND) {
		r1.live_sizing = 0;
		r1.resize_last_change = GetTickCount();
		logline("present %s: live move/resize ended; waiting for stable"
			" drawable dimensions\n", present_rung());
	}
}

static const char r1_vertex_shader[] =
	"#version 330 core\n"
	"out vec2 uv;\n"
	"void main() {\n"
	"  vec2 p = vec2(gl_VertexID == 1 ? 2.0 : 0.0,\n"
	"                gl_VertexID == 2 ? 2.0 : 0.0);\n"
	"  uv = p;\n"
	"  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";

static const char r1_fragment_shader[] =
	"#version 330 core\n"
	"in vec2 uv;\n"
	"out vec4 pixel;\n"
	"uniform sampler2D guest_frame;\n"
	"uniform int flip_y;\n"
	"void main() {\n"
	"  vec2 source = vec2(uv.x, flip_y != 0 ? 1.0 - uv.y : uv.y);\n"
	"  pixel = texture(guest_frame, source);\n"
	"}\n";

static int r1_scene_init(struct r1_scene *scene)
{
	GLuint vertex = 0, fragment = 0;
	GLint ok = 0;
	char message[2048];
	GLsizei length = 0;

	ZeroMemory(scene, sizeof(*scene));
	vertex = compile_shader("present R1", GL_VERTEX_SHADER,
				r1_vertex_shader);
	fragment = compile_shader("present R1", GL_FRAGMENT_SHADER,
				  r1_fragment_shader);
	if (!vertex || !fragment)
		goto fail;

	scene->program = glCreateProgram();
	if (!scene->program)
		goto fail;
	glAttachShader(scene->program, vertex);
	glAttachShader(scene->program, fragment);
	glLinkProgram(scene->program);
	glGetProgramiv(scene->program, GL_LINK_STATUS, &ok);
	if (!ok) {
		message[0] = '\0';
		glGetProgramInfoLog(scene->program, sizeof(message) - 1,
				    &length, message);
		message[sizeof(message) - 1] = '\0';
		logline("present %s: shader link failed: %s\n",
			present_rung(), message);
		goto fail;
	}

	scene->texture_uniform = glGetUniformLocation(scene->program,
						"guest_frame");
	scene->flip_uniform = glGetUniformLocation(scene->program, "flip_y");
	glGenVertexArrays(1, &scene->vao);
	glGenSamplers(1, &scene->sampler);
	if (!scene->vao || !scene->sampler || scene->texture_uniform < 0 ||
	    scene->flip_uniform < 0)
		goto fail;

	/* A sampler object overrides filtering without mutating Mesa's texture
	 * object. The guest remains the sole owner of that object's state. */
	glSamplerParameteri(scene->sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glSamplerParameteri(scene->sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glSamplerParameteri(scene->sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glSamplerParameteri(scene->sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glDeleteShader(vertex);
	glDeleteShader(fragment);
	return 0;

fail:
	if (vertex)
		glDeleteShader(vertex);
	if (fragment)
		glDeleteShader(fragment);
	if (scene->sampler)
		glDeleteSamplers(1, &scene->sampler);
	if (scene->vao)
		glDeleteVertexArrays(1, &scene->vao);
	if (scene->program)
		glDeleteProgram(scene->program);
	ZeroMemory(scene, sizeof(*scene));
	return -1;
}

static void r1_scene_fini(struct r1_scene *scene)
{
	/* Drop the binding first. OpenGL deletion is deferred while a texture is
	 * bound in another context; this is R1's presentation reference. */
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindSampler(0, 0);
	if (scene->sampler)
		glDeleteSamplers(1, &scene->sampler);
	if (scene->vao)
		glDeleteVertexArrays(1, &scene->vao);
	if (scene->program)
		glDeleteProgram(scene->program);
	ZeroMemory(scene, sizeof(*scene));
}

static unsigned long dimension_delta(LONG actual, unsigned int wanted)
{
	LONG delta = actual - (LONG)wanted;

	return (unsigned long)(delta < 0 ? -delta : delta);
}

static BOOL CALLBACK match_vcxsrv_window(HWND window, LPARAM opaque)
{
	struct vcxsrv_match *match = (struct vcxsrv_match *)opaque;
	struct vcxsrv_candidate candidate;
	RECT rect;
	char class_name[64];
	HANDLE xid;
	LONG width, height;

	if (!IsWindowVisible(window))
		return TRUE;
	if (!GetClassNameA(window, class_name, sizeof(class_name)) ||
	    strcmp(class_name, VCXSRV_WINDOW_CLASS) != 0)
		return TRUE;
	xid = GetPropA(window, VCXSRV_XID_PROPERTY);
	if (!xid || !GetClientRect(window, &rect))
		return TRUE;

	width = rect.right - rect.left;
	height = rect.bottom - rect.top;
	if (width <= 0 || height <= 0)
		return TRUE;

	candidate.window = window;
	candidate.xid = (uintptr_t)xid;
	candidate.area = (unsigned long)width * (unsigned long)height;
	candidate.score = dimension_delta(width, match->width) +
			  dimension_delta(height, match->height);
	candidate.foreground = GetForegroundWindow() == window;

	if (!match->best.window || candidate.score < match->best.score ||
	    (candidate.score == match->best.score &&
	     candidate.foreground && !match->best.foreground) ||
	    (candidate.score == match->best.score &&
	     candidate.foreground == match->best.foreground &&
	     candidate.area > match->best.area))
		match->best = candidate;
	return TRUE;
}

static int r1_find_target(unsigned int width, unsigned int height,
			  struct vcxsrv_candidate *target)
{
	struct vcxsrv_match match;
	unsigned long limit;

	ZeroMemory(&match, sizeof(match));
	match.width = width;
	match.height = height;
	EnumWindows(match_vcxsrv_window, (LPARAM)&match);
	if (!match.best.window)
		return -1;

	/* Resource and X client extents match exactly in the working path. Four
	 * pixels allow decoration/rounding noise without ever rebinding the old
	 * resource after a real resize. */
	limit = 4;
	if (match.best.score > limit)
		return -1;
	*target = match.best;
	return 0;
}

struct vcxsrv_xid_match {
	uintptr_t xid;
	struct vcxsrv_candidate target;
};

static BOOL CALLBACK match_vcxsrv_xid(HWND window, LPARAM opaque)
{
	struct vcxsrv_xid_match *match = (struct vcxsrv_xid_match *)opaque;
	RECT rect;
	char class_name[64];
	HANDLE xid;
	LONG width, height;

	if (!IsWindowVisible(window) ||
	    !GetClassNameA(window, class_name, sizeof(class_name)) ||
	    strcmp(class_name, VCXSRV_WINDOW_CLASS) != 0)
		return TRUE;
	xid = GetPropA(window, VCXSRV_XID_PROPERTY);
	if ((uintptr_t)xid != match->xid || !GetClientRect(window, &rect))
		return TRUE;
	width = rect.right - rect.left;
	height = rect.bottom - rect.top;
	if (width <= 0 || height <= 0)
		return TRUE;

	match->target.window = window;
	match->target.xid = (uintptr_t)xid;
	match->target.area = (unsigned long)width * (unsigned long)height;
	match->target.score = 0;
	match->target.foreground = GetForegroundWindow() == window;
	return FALSE;
}

static int r2_find_target(uint32_t xid, struct vcxsrv_candidate *target)
{
	struct vcxsrv_xid_match match;

	ZeroMemory(&match, sizeof(match));
	match.xid = (uintptr_t)xid;
	EnumWindows(match_vcxsrv_xid, (LPARAM)&match);
	if (!match.target.window)
		return -1;
	*target = match.target;
	return 0;
}

static void r1_drop_target(void)
{
	struct wgl_current_context saved;
	HWND parent = r1.parent;

	if (r1.movesize_hook) {
		UnhookWinEvent(r1.movesize_hook);
		r1.movesize_hook = NULL;
	}
	if (r1.context.context) {
		wgl_winsys_current_save(&saved);
		if (wgl_winsys_window_context_make_current(&r1.context) == 0) {
			r1_scene_fini(&r1.scene);
			if (wgl_winsys_current_restore(&saved) != 0)
				wgl_winsys_restore();
		}
		wgl_winsys_window_context_destroy(&r1.context);
	}
	if (r1.overlay && IsWindow(r1.overlay))
		DestroyWindow(r1.overlay);
	if (parent && IsWindow(parent))
		InvalidateRect(parent, NULL, TRUE);

	r1.parent = NULL;
	r1.overlay = NULL;
	r1.xid = 0;
	r1.resource_id = 0;
	r1.texture_id = 0;
	r1.drawable_x = 0;
	r1.drawable_y = 0;
	r1.drawable_width = 0;
	r1.drawable_height = 0;
	r1.resize_width = 0;
	r1.resize_height = 0;
	r1.resize_pending = 0;
	r1.live_sizing = 0;
	r1.surface_hidden = 1;
	r1.frames = 0;
	r1.first_frame = 0;
	r1.last_report = 0;
	r1.resize_last_change = 0;
	r1.last_poll = 0;
	r1.rung = 0;
}

/* Return 1 for a drawable parent, 0 while it is hidden/minimised, and -1
 * when its client geometry cannot be obtained. Coordinates are desktop
 * coordinates because R1's surface is intentionally not parented to VcXsrv. */
static int r1_parent_geometry(HWND parent, int *x, int *y,
			      int *width, int *height)
{
	RECT rect;
	POINT origin;

	if (!IsWindow(parent))
		return -1;
	if (!IsWindowVisible(parent) || IsIconic(parent))
		return 0;
	if (!GetClientRect(parent, &rect))
		return -1;
	*width = rect.right - rect.left;
	*height = rect.bottom - rect.top;
	if (*width <= 0 || *height <= 0)
		return 0;
	origin.x = 0;
	origin.y = 0;
	if (!ClientToScreen(parent, &origin))
		return -1;
	*x = origin.x;
	*y = origin.y;
	return 1;
}

/* Keep the independent popup immediately above VcXsrv and below whatever
 * occludes VcXsrv. SetWindowPos is issued only for a real geometry/Z-order
 * transition; running it on every guest frame needlessly couples rendering to
 * USER32 and was one of the dangerous properties of the original probe. */
static int r1_place_surface(int x, int y, int width, int height)
{
	HWND above;
	HWND insert_after;
	UINT flags = SWP_NOACTIVATE;
	int geometry_changed;

	if (!r1.parent || !r1.overlay)
		return -1;
	above = GetWindow(r1.parent, GW_HWNDPREV);
	geometry_changed = x != r1.drawable_x || y != r1.drawable_y ||
		width != r1.drawable_width || height != r1.drawable_height;
	if (above == r1.overlay) {
		insert_after = NULL;
		flags |= SWP_NOZORDER;
	} else {
		/* hWndInsertAfter precedes this window. Choosing the window just
		 * above VcXsrv places us between the two without raising VcXsrv. */
		insert_after = above ? above : HWND_TOP;
	}
	if (r1.surface_hidden)
		flags |= SWP_SHOWWINDOW;
	if (!geometry_changed && above == r1.overlay && !r1.surface_hidden)
		return 0;
	if (!SetWindowPos(r1.overlay, insert_after, x, y, width, height,
			  flags)) {
		logline("present %s: overlay placement failed (%lu)\n",
			present_rung(), (unsigned long)GetLastError());
		r1_hide_surface();
		return -1;
	}
	r1.drawable_x = x;
	r1.drawable_y = y;
	r1.drawable_width = width;
	r1.drawable_height = height;
	r1.surface_hidden = 0;
	return 0;
}

static int r1_create_target(const struct vcxsrv_candidate *target,
			    unsigned int resource_width,
			    unsigned int resource_height, int rung)
{
	struct wgl_current_context saved;
	int x, y, width, height;
	int scene_ready = 0;

	if (register_presenter_class() != 0 ||
	    r1_parent_geometry(target->window, &x, &y, &width, &height) != 1)
		return -1;
	r1.rung = rung;

	/* No HWND parent and no owner: this surface must never enter VcXsrv's
	 * cross-process child enumeration/resize path. Disabled + NOACTIVATE and
	 * HTTRANSPARENT leave all keyboard and pointer ownership with X11. */
	r1.overlay = CreateWindowExA(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW |
		WS_EX_TRANSPARENT, PRESENTER_CLASS, "",
		WS_POPUP | WS_DISABLED | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
		x, y, width, height, NULL, NULL,
		GetModuleHandle(NULL), NULL);
	if (!r1.overlay) {
		logline("present %s: independent overlay create failed (%lu)\n",
			present_rung(), (unsigned long)GetLastError());
		r1.rung = 0;
		return -1;
	}

	r1.parent = target->window;
	r1.xid = target->xid;
	r1.drawable_x = x;
	r1.drawable_y = y;
	r1.drawable_width = width;
	r1.drawable_height = height;
	r1.surface_hidden = 1;
	r1.movesize_hook = SetWinEventHook(EVENT_SYSTEM_MOVESIZESTART,
		EVENT_SYSTEM_MOVESIZEEND, NULL, r1_movesize_event, 0, 0,
		WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
	if (!r1.movesize_hook)
		logline("present %s: WinEvent resize hook unavailable (%lu);"
			" rectangle-change fallback remains active\n",
			present_rung(), (unsigned long)GetLastError());
	if (wgl_winsys_window_context_create(r1.overlay, &r1.context) != 0)
		goto fail;

	wgl_winsys_current_save(&saved);
	if (wgl_winsys_window_context_make_current(&r1.context) != 0)
		goto restore_fail;
	if (r1_scene_init(&r1.scene) != 0)
		goto restore_fail;
	scene_ready = 1;
	if (rung == 2 && wgl_winsys_swap_interval(0) != 0)
		logline("present R2: WGL swap interval control unavailable;"
			" presentation may remain refresh-paced\n");
	if (wgl_winsys_current_restore(&saved) != 0) {
		logline("present %s: could not restore virglrenderer's WGL context\n",
			present_rung());
		wgl_winsys_restore();
		goto fail;
	}
	if (r1_place_surface(x, y, width, height) != 0)
		goto fail;

	logline("present %s: independent overlay matched VcXsrv XID 0x%lx"
		" (%dx%d) to guest resource %ux%u\n",
		present_rung(), (unsigned long)r1.xid, width, height, resource_width,
		resource_height);
	return 0;

restore_fail:
	if (scene_ready)
		r1_scene_fini(&r1.scene);
	if (wgl_winsys_current_restore(&saved) != 0)
		wgl_winsys_restore();
fail:
	r1_drop_target();
	return -1;
}

/*
 * A live resize must not drive SwapBuffers while VcXsrv's thread is inside its
 * modal sizing loop and the X client is waiting on this same GPU command.
 *
 * Hide immediately, keep completing renderer commands without presenting,
 * and discard the old binding only after the client rectangle has stayed put
 * for 200 ms. The following frame can then match the new-size resource. If a
 * move was cancelled or changed no dimensions, reveal the existing overlay.
 */
static int r1_parent_ready(int width, int height)
{
	DWORD now = GetTickCount();

	if (r1.live_sizing)
		return 0;

	if (width != r1.drawable_width || height != r1.drawable_height) {
		if (!r1.resize_pending || width != r1.resize_width ||
		    height != r1.resize_height) {
			if (!r1.resize_pending)
				r1_hide_surface();
			r1.resize_pending = 1;
			r1.resize_width = width;
			r1.resize_height = height;
			r1.resize_last_change = now;
			return 0;
		}
		if (now - r1.resize_last_change < 200)
			return 0;

		logline("present %s: drawable settled from %dx%d to %dx%d;"
			" releasing the old presentation binding\n",
			present_rung(), r1.drawable_width, r1.drawable_height,
			width, height);
		r1_drop_target();
		return -1;
	}

	if (r1.resize_pending) {
		if (now - r1.resize_last_change < 200)
			return 0;
		r1.resize_pending = 0;
		logline("present %s: move/resize kept %dx%d; presentation resumed\n",
			present_rung(), width, height);
	}
	return 1;
}

/* Pump WinEvent independently of guest rendering. An X client can stop
 * submitting while VcXsrv is in its modal move/resize loop, which is exactly
 * when relying on frame callbacks to deliver MOVESIZESTART is too late. */
static int present_surface_poll(void)
{
	DWORD now;
	int geometry;
	int ready;
	int x, y, width, height;

	now = GetTickCount();
	if (now - r1.last_poll < 16)
		return r1.parent && !r1.surface_hidden;
	r1.last_poll = now;

	if (pump_messages() != 0)
		return -1;
	if (!r1.parent)
		return 0;
	if (!IsWindow(r1.parent) || !r1.overlay || !IsWindow(r1.overlay)) {
		r1_drop_target();
		return 0;
	}

	geometry = r1_parent_geometry(r1.parent, &x, &y, &width, &height);
	if (geometry <= 0) {
		r1_hide_surface();
		if (geometry < 0 && !IsWindow(r1.parent))
			r1_drop_target();
		return 0;
	}
	ready = r1_parent_ready(width, height);
	if (ready <= 0)
		return 0;
	if (r1_place_surface(x, y, width, height) != 0)
		return -1;
	return 1;
}

int cogpu_present_r1_poll(void)
{
	return r1.enabled ? present_surface_poll() : 0;
}

int cogpu_present_r2_poll(void)
{
	return r2_enabled ? present_surface_poll() : 0;
}

void cogpu_present_r1_enable(void)
{
	r1.enabled = 1;
	logline("present R1: enabled; waiting for a guest-rendered 2D colour"
		" readback matching a VcXsrv client\n");
}

int cogpu_present_r1_frame(const struct cogpu_vrend_resource_info *resource,
			   unsigned int level,
			   unsigned int x, unsigned int y,
			   unsigned int width, unsigned int height)
{
	struct vcxsrv_candidate target;
	struct wgl_current_context saved;
	GLsync ready = NULL;
	GLenum error;
	GLint red_type = GL_NONE;
	DWORD now;
	int window_width, window_height;
	int rc = 0;

	(void)x;
	(void)y;
	(void)width;
	(void)height;

	if (!r1.enabled || !resource || level != 0)
		return 0;
	if (resource->target != COGPU_PIPE_TEXTURE_2D ||
	    resource->nr_samples > 1 || resource->depth != 1 ||
	    resource->width < 64 || resource->height < 64 ||
	    !(resource->bind & COGPU_PIPE_BIND_RENDER_TARGET) ||
	    (resource->bind & COGPU_PIPE_BIND_DEPTH_STENCIL))
		return 0;

	if (cogpu_present_r1_poll() < 0)
		return -1;
	if (r1.parent) {
		if (r1.surface_hidden || r1.live_sizing || r1.resize_pending)
			return 0;
		if (r1.resource_id != resource->resource_id)
			return 0;
	}

	if (!r1.parent) {
		ZeroMemory(&target, sizeof(target));
		if (r1_find_target(resource->width, resource->height, &target) != 0) {
			now = GetTickCount();
			if (now - r1.last_miss_report >= 2000) {
				r1.last_miss_report = now;
				logline("present R1: candidate res %u tex %u fmt %u"
					" %ux%u has no similarly-sized VcXsrv client\n",
					resource->resource_id, resource->tex_id,
					resource->format, resource->width,
					resource->height);
			}
			return 0;
		}
		if (r1_create_target(&target, resource->width,
				     resource->height, 1) != 0)
			return 0;
		r1.resource_id = resource->resource_id;
		r1.texture_id = resource->tex_id;
		logline("present R1: bound res %u, host texture %u, format %u,"
			" bind 0x%x, flags 0x%x\n", resource->resource_id,
			resource->tex_id, resource->format, resource->bind,
			resource->flags);
	}

	window_width = r1.drawable_width;
	window_height = r1.drawable_height;
	if (window_width <= 0 || window_height <= 0)
		return 0;

	/* Producer and presenter are different contexts in one share group. A GL
	 * fence is the acquire operation; same-thread execution alone does not make
	 * cross-context texture writes visible. */
	wgl_winsys_current_save(&saved);
	if (!saved.context) {
		logline("present R1: renderer left no WGL context current\n");
		return 0;
	}
	ready = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	if (ready)
		glFlush();
	else
		glFinish();

	if (wgl_winsys_window_context_make_current(&r1.context) != 0) {
		logline("present R1: could not make presentation context current\n");
		goto restore;
	}
	if (ready)
		glWaitSync(ready, 0, GL_TIMEOUT_IGNORED);

	while (glGetError() != GL_NO_ERROR)
		;
	if (!glIsTexture(resource->tex_id)) {
		logline("present R1: renderer name %u is not a shared GL texture\n",
			resource->tex_id);
		goto restore;
	}
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, resource->tex_id);
	error = glGetError();
	if (error != GL_NO_ERROR) {
		logline("present R1: texture %u rejected as GL_TEXTURE_2D"
			" (GL 0x%x)\n", resource->tex_id, (unsigned int)error);
		goto restore;
	}
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_RED_TYPE,
				 &red_type);
	if (glGetError() == GL_NO_ERROR &&
	    (red_type == GL_INT || red_type == GL_UNSIGNED_INT)) {
		logline("present R1: integer colour resource %u needs a typed shader;"
			" refusing to misinterpret it\n", resource->resource_id);
		goto restore;
	}

	glViewport(0, 0, window_width, window_height);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_FRAMEBUFFER_SRGB);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glUseProgram(r1.scene.program);
	glUniform1i(r1.scene.texture_uniform, 0);
	glUniform1i(r1.scene.flip_uniform,
		(resource->flags & COGPU_VIRGL_RESOURCE_Y_0_TOP) != 0);
	glBindSampler(0, r1.scene.sampler);
	glBindVertexArray(r1.scene.vao);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	if (!SwapBuffers(r1.context.dc)) {
		logline("present R1: SwapBuffers failed (%lu)\n",
			(unsigned long)GetLastError());
		goto restore;
	}
	rc = 1;

	now = GetTickCount();
	if (r1.frames++ == 0) {
		r1.first_frame = now;
		r1.last_report = now;
		logline("present R1: first guest texture frame displayed\n");
	} else if (now - r1.last_report >= 5000) {
		double fps = (now != r1.first_frame) ?
			(double)r1.frames * 1000.0 / (now - r1.first_frame) : 0.0;

		r1.last_report = now;
		logline("present R1: %u direct frames, %.1f fps\n",
			r1.frames, fps);
	}

restore:
	if (ready)
		glDeleteSync(ready);
	if (wgl_winsys_current_restore(&saved) != 0) {
		logline("present R1: LOST the renderer's exact WGL context;"
			" falling back to base\n");
		wgl_winsys_restore();
		rc = -1;
	}
	if (pump_messages() != 0)
		rc = -1;
	return rc;
}

void cogpu_present_r1_resource_unref(unsigned int resource_id)
{
	if (r1.resource_id != resource_id)
		return;
	logline("present R1: guest released displayed resource %u after %u"
		" frame(s); removing overlay before renderer deletion\n",
		resource_id, r1.frames);
	r1_drop_target();
}

void cogpu_present_r1_fini(void)
{
	if (r1.parent)
		r1_drop_target();
	r1.enabled = 0;
}

/* ------------------------------------------------ fenced metadata path (R2) */

static struct r2_binding *r2_find_binding(const struct copresent_record *record)
{
	unsigned int i;

	for (i = 0; i < R2_MAX_BINDINGS; i++)
		if (r2_bindings[i].active &&
		    r2_bindings[i].generation == record->generation &&
		    r2_bindings[i].connection == record->connection &&
		    r2_bindings[i].buffer == record->buffer)
			return &r2_bindings[i];
	return NULL;
}

static struct r2_binding *r2_alloc_binding(void)
{
	unsigned int i;

	for (i = 0; i < R2_MAX_BINDINGS; i++)
		if (!r2_bindings[i].active)
			return &r2_bindings[i];
	return NULL;
}

void cogpu_present_r2_enable(void)
{
	r2_enabled = 1;
	logline("present R2: enabled; waiting for fenced CoPresent metadata\n");
}

int cogpu_present_r2_bind(const struct copresent_record *record,
			  const struct cogpu_vrend_resource_info *resource)
{
	struct r2_binding *binding;

	if (!r2_enabled || !record || !resource || record->xid == 0 ||
	    record->width == 0 || record->height == 0 ||
	    record->resource_id != resource->resource_id ||
	    record->width != resource->width ||
	    record->height != resource->height ||
	    resource->target != COGPU_PIPE_TEXTURE_2D ||
	    resource->nr_samples > 1 || resource->depth != 1 ||
	    !(resource->bind & COGPU_PIPE_BIND_RENDER_TARGET) ||
	    (resource->bind & COGPU_PIPE_BIND_DEPTH_STENCIL))
		return COPRESENT_BAD_RECORD;
	if (r2_generation && r2_generation != record->generation)
		return COPRESENT_STALE;

	binding = r2_find_binding(record);
	if (!binding)
		binding = r2_alloc_binding();
	if (!binding)
		return COPRESENT_PRESENT_FAILED;
	if (binding->active && r1.rung == 2 &&
	    r1.resource_id == binding->resource.resource_id)
		r1_drop_target();

	ZeroMemory(binding, sizeof(*binding));
	binding->active = 1;
	binding->generation = record->generation;
	binding->connection = record->connection;
	binding->buffer = record->buffer;
	binding->xid = record->xid;
	binding->resource = *resource;
	r2_generation = record->generation;
	logline("present R2: BIND gen %u connection %u buffer %u, XID 0x%x,"
		" resource %u texture %u %ux%u\n", record->generation,
		record->connection, record->buffer, record->xid,
		resource->resource_id, resource->tex_id, resource->width,
		resource->height);
	return COPRESENT_OK;
}

/*
 * Where a presented frame's host time goes, accumulated per 2 s report.
 * Diagnostic only: the question this answers is which serial segment of the
 * synchronous present starves the GPU between frames -- context switches,
 * the finish drain, the swap, or none of them.
 */
struct r2_segments {
	LARGE_INTEGER frequency;
	uint64_t make_current;
	uint64_t draw;
	uint64_t finish;
	uint64_t swap;
	uint64_t restore;
	uint64_t samples;
};

static struct r2_segments r2_segments;

static double r2_segment_us(uint64_t ticks)
{
	if (!r2_segments.samples || !r2_segments.frequency.QuadPart)
		return 0.0;
	return (double)ticks * 1000000.0 /
		((double)r2_segments.frequency.QuadPart * r2_segments.samples);
}

static int r2_draw_resource(const struct cogpu_vrend_resource_info *resource,
			    uint32_t present_flags)
{
	struct wgl_current_context saved;
	GLenum error;
	GLint red_type = GL_NONE;
	LARGE_INTEGER t0, t1, t2, t3, t4, t5;
	DWORD now;
	int rc = COPRESENT_PRESENT_FAILED;

	QueryPerformanceCounter(&t0);
	wgl_winsys_current_save(&saved);
	if (!saved.context) {
		logline("present R2: renderer left no WGL context current\n");
		return COPRESENT_PRESENT_FAILED;
	}
	if (wgl_winsys_window_context_make_current(&r1.context) != 0) {
		logline("present R2: could not make presentation context current\n");
		goto restore;
	}
	QueryPerformanceCounter(&t1);

	while (glGetError() != GL_NO_ERROR)
		;
	if (!glIsTexture(resource->tex_id)) {
		logline("present R2: renderer name %u is not a shared GL texture\n",
			resource->tex_id);
		goto restore;
	}
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, resource->tex_id);
	error = glGetError();
	if (error != GL_NO_ERROR) {
		logline("present R2: texture %u rejected as GL_TEXTURE_2D"
			" (GL 0x%x)\n", resource->tex_id, (unsigned int)error);
		goto unbind;
	}
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_RED_TYPE,
				 &red_type);
	if (glGetError() == GL_NO_ERROR &&
	    (red_type == GL_INT || red_type == GL_UNSIGNED_INT)) {
		logline("present R2: integer colour resource %u is unsupported\n",
			resource->resource_id);
		goto unbind;
	}

	glViewport(0, 0, r1.drawable_width, r1.drawable_height);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_FRAMEBUFFER_SRGB);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glUseProgram(r1.scene.program);
	glUniform1i(r1.scene.texture_uniform, 0);
	glUniform1i(r1.scene.flip_uniform,
		((resource->flags & COGPU_VIRGL_RESOURCE_Y_0_TOP) != 0) ^
		((present_flags & COPRESENT_F_Y_0_TOP) != 0));
	glBindSampler(0, r1.scene.sampler);
	glBindVertexArray(r1.scene.vao);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	QueryPerformanceCounter(&t2);

	/* RELEASE means the producer may immediately render into this texture.
	 * Finish the sampling draw before publishing it. Swap then only consumes
	 * the overlay's back buffer, not the guest texture. */
	glFinish();
	QueryPerformanceCounter(&t3);
	if (!SwapBuffers(r1.context.dc)) {
		logline("present R2: SwapBuffers failed (%lu)\n",
			(unsigned long)GetLastError());
		goto unbind;
	}
	QueryPerformanceCounter(&t4);
	rc = COPRESENT_OK;

	now = GetTickCount();
	if (r1.frames++ == 0) {
		r1.first_frame = now;
		r1.last_report = now;
		logline("present R2: first fenced no-copy frame displayed\n");
	} else if (now - r1.last_report >= 2000) {
		double fps = (now != r1.first_frame) ?
			(double)r1.frames * 1000.0 / (now - r1.first_frame) : 0.0;

		r1.last_report = now;
		logline("present R2: %u released no-copy frames, %.1f fps;"
			" avg us: current %.0f draw %.0f finish %.0f swap %.0f"
			" restore %.0f\n",
			r1.frames, fps,
			r2_segment_us(r2_segments.make_current),
			r2_segment_us(r2_segments.draw),
			r2_segment_us(r2_segments.finish),
			r2_segment_us(r2_segments.swap),
			r2_segment_us(r2_segments.restore));
	}

unbind:
	glBindSampler(0, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
restore:
	if (wgl_winsys_current_restore(&saved) != 0) {
		logline("present R2: lost the renderer's exact WGL context\n");
		wgl_winsys_restore();
		rc = COPRESENT_PRESENT_FAILED;
	}
	if (pump_messages() != 0)
		rc = COPRESENT_PRESENT_FAILED;
	if (rc == COPRESENT_OK) {
		QueryPerformanceCounter(&t5);
		if (!r2_segments.frequency.QuadPart)
			QueryPerformanceFrequency(&r2_segments.frequency);
		r2_segments.make_current += (uint64_t)(t1.QuadPart - t0.QuadPart);
		r2_segments.draw += (uint64_t)(t2.QuadPart - t1.QuadPart);
		r2_segments.finish += (uint64_t)(t3.QuadPart - t2.QuadPart);
		r2_segments.swap += (uint64_t)(t4.QuadPart - t3.QuadPart);
		r2_segments.restore += (uint64_t)(t5.QuadPart - t4.QuadPart);
		r2_segments.samples++;
	}
	return rc;
}

int cogpu_present_r2_present(const struct copresent_record *record)
{
	struct r2_binding *binding;
	struct vcxsrv_candidate target;
	RECT rect;
	int width, height;
	int poll_rc;

	if (!r2_enabled || !record ||
	    !(record->flags & COPRESENT_F_ACQUIRE_WAITED))
		return COPRESENT_BAD_RECORD;
	binding = r2_find_binding(record);
	if (!binding || binding->resource.resource_id != record->resource_id ||
	    binding->xid != record->xid)
		return COPRESENT_STALE;

	if (r1.parent && (r1.rung != 2 || r1.xid != binding->xid))
		r1_drop_target();
	poll_rc = present_surface_poll();
	if (poll_rc < 0)
		return COPRESENT_PRESENT_FAILED;

	if (!r1.parent) {
		ZeroMemory(&target, sizeof(target));
		if (r2_find_target(binding->xid, &target) != 0)
			return COPRESENT_NO_WINDOW;
		if (!GetClientRect(target.window, &rect))
			return COPRESENT_NO_WINDOW;
		width = rect.right - rect.left;
		height = rect.bottom - rect.top;
		if (width != (int)binding->resource.width ||
		    height != (int)binding->resource.height)
			return COPRESENT_STALE;
		if (r1_create_target(&target, binding->resource.width,
				     binding->resource.height, 2) != 0)
			return COPRESENT_PRESENT_FAILED;
	}
	if (r1.surface_hidden || r1.live_sizing || r1.resize_pending)
		return COPRESENT_OK; /* Deliberate mailbox drop; buffer is reusable. */
	if (r1.drawable_width != (int)binding->resource.width ||
	    r1.drawable_height != (int)binding->resource.height)
		return COPRESENT_STALE;

	r1.resource_id = binding->resource.resource_id;
	r1.texture_id = binding->resource.tex_id;
	return r2_draw_resource(&binding->resource, record->flags);
}

int cogpu_present_r2_unbind(const struct copresent_record *record)
{
	struct r2_binding *binding;
	uint32_t resource_id;

	if (!record)
		return COPRESENT_BAD_RECORD;
	binding = r2_find_binding(record);
	if (!binding)
		return COPRESENT_STALE;
	resource_id = binding->resource.resource_id;
	if (r1.rung == 2 && r1.resource_id == resource_id)
		r1_drop_target();
	ZeroMemory(binding, sizeof(*binding));
	logline("present R2: UNBIND connection %u buffer %u resource %u\n",
		record->connection, record->buffer, resource_id);
	return COPRESENT_OK;
}

void cogpu_present_r2_resource_unref(unsigned int resource_id)
{
	unsigned int i;
	int found = 0;

	for (i = 0; i < R2_MAX_BINDINGS; i++) {
		if (!r2_bindings[i].active ||
		    r2_bindings[i].resource.resource_id != resource_id)
			continue;
		ZeroMemory(&r2_bindings[i], sizeof(r2_bindings[i]));
		found = 1;
	}
	if (found) {
		if (r1.rung == 2 && r1.resource_id == resource_id)
			r1_drop_target();
		logline("present R2: resource %u vanished before UNBIND;"
			" stale bindings retired\n", resource_id);
	}
}

void cogpu_present_r2_reset(unsigned int generation)
{
	unsigned int i;

	if (r1.rung == 2)
		r1_drop_target();
	for (i = 0; i < R2_MAX_BINDINGS; i++)
		ZeroMemory(&r2_bindings[i], sizeof(r2_bindings[i]));
	r2_generation = generation;
	logline("present R2: reset for broker generation %u\n", generation);
}

void cogpu_present_r2_fini(void)
{
	if (r1.rung == 2)
		r1_drop_target();
	ZeroMemory(r2_bindings, sizeof(r2_bindings));
	r2_generation = 0;
	r2_enabled = 0;
}
