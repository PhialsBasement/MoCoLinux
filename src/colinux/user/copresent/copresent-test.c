/*
 * R2 acceptance producer: two exported virgl textures, no VGL and no pixels
 * sent through X11.
 *
 * X11 creates only the control-plane window whose XID the host matches. EGL
 * renders into surfaceless FBO textures on renderD128, exports each as a
 * dma-buf for BIND, and exports a native fence for every PRESENT. The broker
 * returns RELEASE before a texture is reused.
 */

#define _GNU_SOURCE
#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "copresent.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TEST_BUFFERS 2
#define GEAR_TEETH   24
#define GEAR_STEPS   (GEAR_TEETH * 4)

struct test_buffer {
	GLuint texture;
	EGLImageKHR image;
	int bound;
	int busy;
};

struct app {
	Display *x_display;
	Window window;
	Atom wm_delete;
	int width;
	int height;
	int socket_fd;

	EGLDisplay egl_display;
	EGLContext egl_context;
	EGLSurface egl_surface;
	PFNEGLCREATEIMAGEKHRPROC create_image;
	PFNEGLDESTROYIMAGEKHRPROC destroy_image;
	PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC export_query;
	PFNEGLEXPORTDMABUFIMAGEMESAPROC export_image;
	PFNEGLCREATESYNCKHRPROC create_sync;
	PFNEGLDESTROYSYNCKHRPROC destroy_sync;
	PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_fence;

	GLuint framebuffer;
	GLuint program;
	GLuint vao;
	GLuint vbo;
	GLint angle_uniform;
	GLint scale_uniform;
	GLint offset_uniform;
	GLint colour_uniform;
	GLsizei vertex_count;
	struct test_buffer buffers[TEST_BUFFERS];
	uint32_t serial;
	unsigned long long submitted;
	unsigned long long released;
};

static const char vertex_shader[] =
	"#version 330 core\n"
	"layout(location=0) in vec2 position;\n"
	"uniform float angle;\n"
	"uniform float scale;\n"
	"uniform vec2 offset;\n"
	"void main() {\n"
	"  float c=cos(angle), s=sin(angle);\n"
	"  vec2 p=mat2(c,-s,s,c)*position*scale+offset;\n"
	"  gl_Position=vec4(p,0.0,1.0);\n"
	"}\n";

static const char fragment_shader[] =
	"#version 330 core\n"
	"uniform vec3 colour;\n"
	"out vec4 pixel;\n"
	"void main() {\n"
	"  float shine=0.80+0.20*sin(gl_FragCoord.x*0.035+gl_FragCoord.y*0.021);\n"
	"  pixel=vec4(colour*shine,1.0);\n"
	"}\n";

static double now_seconds(void)
{
	struct timespec time;

	clock_gettime(CLOCK_MONOTONIC, &time);
	return (double)time.tv_sec + (double)time.tv_nsec / 1000000000.0;
}

static int connect_broker(void)
{
	struct sockaddr_un address;
	int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);

	if (fd < 0)
		return -1;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strncpy(address.sun_path, COPRESENT_LOCAL_PATH,
		sizeof(address.sun_path) - 1);
	if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int send_record_fd(int socket_fd, const struct copresent_record *record,
			  int passed_fd)
{
	struct iovec iov = { .iov_base = (void *)record,
		.iov_len = sizeof(*record) };
	char control[CMSG_SPACE(sizeof(int))];
	struct msghdr message;

	memset(&message, 0, sizeof(message));
	message.msg_iov = &iov;
	message.msg_iovlen = 1;
	if (passed_fd >= 0) {
		struct cmsghdr *cmsg;

		memset(control, 0, sizeof(control));
		message.msg_control = control;
		message.msg_controllen = sizeof(control);
		cmsg = CMSG_FIRSTHDR(&message);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(cmsg), &passed_fd, sizeof(passed_fd));
	}
	return sendmsg(socket_fd, &message, MSG_NOSIGNAL) == sizeof(*record) ?
		0 : -1;
}

static int receive_response(struct app *app, struct copresent_record *response)
{
	ssize_t got = recv(app->socket_fd, response, sizeof(*response), 0);

	if (got != sizeof(*response))
		return -1;
	if (response->version != COPRESENT_VERSION)
		return -1;
	if (response->buffer < TEST_BUFFERS &&
	    response->opcode == COPRESENT_RELEASE) {
		app->buffers[response->buffer].busy = 0;
		app->released++;
	}
	if (response->status != COPRESENT_OK) {
		fprintf(stderr, "copresent-test: opcode 0x%x buffer %u status %u\n",
			response->opcode, response->buffer, response->status);
	}
	return 0;
}

static int wait_for_response(struct app *app, uint32_t opcode, uint32_t buffer)
{
	for (;;) {
		struct copresent_record response;

		if (receive_response(app, &response) != 0)
			return -1;
		if (response.opcode == opcode && response.buffer == buffer)
			return response.status == COPRESENT_OK ? 0 : -1;
	}
}

static GLuint compile_shader(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	GLint ok;

	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[2048];

		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		fprintf(stderr, "copresent-test: shader: %s\n", log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static int make_scene(struct app *app)
{
	GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_shader);
	GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_shader);
	GLint linked;
	float *mesh;
	unsigned int i;
	unsigned int out = 0;

	if (!vertex || !fragment)
		return -1;
	app->program = glCreateProgram();
	glAttachShader(app->program, vertex);
	glAttachShader(app->program, fragment);
	glLinkProgram(app->program);
	glGetProgramiv(app->program, GL_LINK_STATUS, &linked);
	glDeleteShader(vertex);
	glDeleteShader(fragment);
	if (!linked)
		return -1;
	app->angle_uniform = glGetUniformLocation(app->program, "angle");
	app->scale_uniform = glGetUniformLocation(app->program, "scale");
	app->offset_uniform = glGetUniformLocation(app->program, "offset");
	app->colour_uniform = glGetUniformLocation(app->program, "colour");

	/* Two triangles per angular step form a ring. Alternating outer radii
	 * make four angular samples per tooth read as a real gear silhouette. */
	app->vertex_count = GEAR_STEPS * 6;
	mesh = calloc((size_t)app->vertex_count * 2, sizeof(*mesh));
	if (!mesh)
		return -1;
	for (i = 0; i < GEAR_STEPS; i++) {
		float a0 = (float)(2.0 * M_PI * i / GEAR_STEPS);
		float a1 = (float)(2.0 * M_PI * (i + 1) / GEAR_STEPS);
		float inner = 0.34f;
		float outer0 = (i % 4 == 1 || i % 4 == 2) ? 1.0f : 0.82f;
		float outer1 = ((i + 1) % 4 == 1 || (i + 1) % 4 == 2) ?
			1.0f : 0.82f;
		float points[12] = {
			inner * cosf(a0), inner * sinf(a0),
			outer0 * cosf(a0), outer0 * sinf(a0),
			outer1 * cosf(a1), outer1 * sinf(a1),
			inner * cosf(a0), inner * sinf(a0),
			outer1 * cosf(a1), outer1 * sinf(a1),
			inner * cosf(a1), inner * sinf(a1)
		};

		memcpy(&mesh[out], points, sizeof(points));
		out += 12;
	}
	glGenVertexArrays(1, &app->vao);
	glBindVertexArray(app->vao);
	glGenBuffers(1, &app->vbo);
	glBindBuffer(GL_ARRAY_BUFFER, app->vbo);
	glBufferData(GL_ARRAY_BUFFER,
		(size_t)app->vertex_count * 2 * sizeof(float), mesh, GL_STATIC_DRAW);
	free(mesh);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), NULL);
	glEnableVertexAttribArray(0);
	glGenFramebuffers(1, &app->framebuffer);
	return 0;
}

static int init_egl(struct app *app)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display;
	EGLConfig config;
	EGLint count;
	EGLint config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8, EGL_NONE
	};
	EGLint surface_attribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };

	get_platform_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
		eglGetProcAddress("eglGetPlatformDisplayEXT");
	if (!get_platform_display)
		return -1;
	app->egl_display = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA,
		EGL_DEFAULT_DISPLAY, NULL);
	if (app->egl_display == EGL_NO_DISPLAY ||
	    !eglInitialize(app->egl_display, NULL, NULL) ||
	    !eglBindAPI(EGL_OPENGL_API) ||
	    !eglChooseConfig(app->egl_display, config_attribs, &config, 1, &count) ||
	    count != 1)
		return -1;
	app->egl_context = eglCreateContext(app->egl_display, config,
		EGL_NO_CONTEXT, NULL);
	app->egl_surface = eglCreatePbufferSurface(app->egl_display, config,
		surface_attribs);
	if (app->egl_context == EGL_NO_CONTEXT ||
	    app->egl_surface == EGL_NO_SURFACE ||
	    !eglMakeCurrent(app->egl_display, app->egl_surface, app->egl_surface,
		app->egl_context))
		return -1;

	app->create_image = (PFNEGLCREATEIMAGEKHRPROC)
		eglGetProcAddress("eglCreateImageKHR");
	app->destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)
		eglGetProcAddress("eglDestroyImageKHR");
	app->export_query = (PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC)
		eglGetProcAddress("eglExportDMABUFImageQueryMESA");
	app->export_image = (PFNEGLEXPORTDMABUFIMAGEMESAPROC)
		eglGetProcAddress("eglExportDMABUFImageMESA");
	app->create_sync = (PFNEGLCREATESYNCKHRPROC)
		eglGetProcAddress("eglCreateSyncKHR");
	app->destroy_sync = (PFNEGLDESTROYSYNCKHRPROC)
		eglGetProcAddress("eglDestroySyncKHR");
	app->dup_fence = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)
		eglGetProcAddress("eglDupNativeFenceFDANDROID");
	if (!app->create_image || !app->destroy_image || !app->export_query ||
	    !app->export_image || !app->create_sync || !app->destroy_sync ||
	    !app->dup_fence)
		return -1;
	fprintf(stderr, "copresent-test: %s\n", glGetString(GL_RENDERER));
	return make_scene(app);
}

static int create_buffer(struct app *app, unsigned int index)
{
	struct test_buffer *buffer = &app->buffers[index];
	EGLint image_attribs[] = { EGL_GL_TEXTURE_LEVEL_KHR, 0, EGL_NONE };
	int fourcc = 0;
	int planes = 0;
	EGLuint64KHR modifier = 0;
	int dma_fds[4] = { -1, -1, -1, -1 };
	EGLint strides[4] = { 0, 0, 0, 0 };
	EGLint offsets[4] = { 0, 0, 0, 0 };
	struct copresent_record record;

	glGenTextures(1, &buffer->texture);
	glBindTexture(GL_TEXTURE_2D, buffer->texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, app->width, app->height, 0,
		GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glBindFramebuffer(GL_FRAMEBUFFER, app->framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		GL_TEXTURE_2D, buffer->texture, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		return -1;
	glClearColor(0.02f, 0.035f, 0.07f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glFlush();

	buffer->image = app->create_image(app->egl_display, app->egl_context,
		EGL_GL_TEXTURE_2D_KHR,
		(EGLClientBuffer)(uintptr_t)buffer->texture, image_attribs);
	if (buffer->image == EGL_NO_IMAGE_KHR ||
	    !app->export_query(app->egl_display, buffer->image, &fourcc,
		&planes, &modifier) || planes != 1 ||
	    !app->export_image(app->egl_display, buffer->image, dma_fds,
		strides, offsets) || dma_fds[0] < 0) {
		fprintf(stderr, "copresent-test: texture %u dma-buf export failed"
			" (EGL 0x%x, planes %d)\n", index, eglGetError(), planes);
		return -1;
	}

	memset(&record, 0, sizeof(record));
	record.opcode = COPRESENT_BIND;
	record.version = COPRESENT_VERSION;
	record.buffer = index;
	record.xid = (uint32_t)app->window;
	record.width = (uint32_t)app->width;
	record.height = (uint32_t)app->height;
	if (send_record_fd(app->socket_fd, &record, dma_fds[0]) != 0) {
		close(dma_fds[0]);
		return -1;
	}
	close(dma_fds[0]);
	if (wait_for_response(app, COPRESENT_BOUND, index) != 0)
		return -1;
	buffer->bound = 1;
	return 0;
}

static int destroy_buffer(struct app *app, unsigned int index)
{
	struct test_buffer *buffer = &app->buffers[index];
	struct copresent_record record;

	while (buffer->busy) {
		struct copresent_record response;

		if (receive_response(app, &response) != 0)
			return -1;
	}
	if (buffer->bound) {
		memset(&record, 0, sizeof(record));
		record.opcode = COPRESENT_UNBIND;
		record.version = COPRESENT_VERSION;
		record.buffer = index;
		if (send_record_fd(app->socket_fd, &record, -1) != 0 ||
		    wait_for_response(app, COPRESENT_UNBOUND, index) != 0)
			return -1;
	}
	if (buffer->image != EGL_NO_IMAGE_KHR)
		app->destroy_image(app->egl_display, buffer->image);
	if (buffer->texture)
		glDeleteTextures(1, &buffer->texture);
	memset(buffer, 0, sizeof(*buffer));
	return 0;
}

static int create_buffers(struct app *app)
{
	unsigned int i;

	for (i = 0; i < TEST_BUFFERS; i++)
		if (create_buffer(app, i) != 0)
			return -1;
	return 0;
}

static int destroy_buffers(struct app *app)
{
	unsigned int i;

	for (i = 0; i < TEST_BUFFERS; i++)
		if (destroy_buffer(app, i) != 0)
			return -1;
	return 0;
}

static void draw_gear(struct app *app, float angle, float scale,
		      float x, float y, float r, float g, float b)
{
	glUniform1f(app->angle_uniform, angle);
	glUniform1f(app->scale_uniform, scale);
	glUniform2f(app->offset_uniform, x, y);
	glUniform3f(app->colour_uniform, r, g, b);
	glDrawArrays(GL_TRIANGLES, 0, app->vertex_count);
}

static int submit_frame(struct app *app, unsigned int index, double seconds)
{
	struct copresent_record record;
	EGLSyncKHR fence;
	int fence_fd;

	glBindFramebuffer(GL_FRAMEBUFFER, app->framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		GL_TEXTURE_2D, app->buffers[index].texture, 0);
	glViewport(0, 0, app->width, app->height);
	glClearColor(0.018f, 0.030f, 0.065f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glUseProgram(app->program);
	glBindVertexArray(app->vao);
	draw_gear(app, (float)(seconds * 1.45), 0.32f, -0.34f, -0.20f,
		0.98f, 0.18f, 0.06f);
	draw_gear(app, (float)(-seconds * 2.17), 0.24f, 0.32f, -0.16f,
		0.08f, 0.88f, 0.34f);
	draw_gear(app, (float)(seconds * 0.91), 0.21f, -0.03f, 0.40f,
		0.16f, 0.46f, 1.00f);

	fence = app->create_sync(app->egl_display,
		EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
	if (fence == EGL_NO_SYNC_KHR)
		return -1;
	glFlush();
	fence_fd = app->dup_fence(app->egl_display, fence);
	app->destroy_sync(app->egl_display, fence);
	if (fence_fd == EGL_NO_NATIVE_FENCE_FD_ANDROID)
		return -1;

	memset(&record, 0, sizeof(record));
	record.opcode = COPRESENT_PRESENT;
	record.version = COPRESENT_VERSION;
	record.buffer = index;
	record.serial = ++app->serial;
	record.damage_width = (uint32_t)app->width;
	record.damage_height = (uint32_t)app->height;
	if (send_record_fd(app->socket_fd, &record, fence_fd) != 0) {
		close(fence_fd);
		return -1;
	}
	close(fence_fd);
	app->buffers[index].busy = 1;
	app->submitted++;
	return 0;
}

static int free_buffer(struct app *app)
{
	for (;;) {
		unsigned int i;

		for (i = 0; i < TEST_BUFFERS; i++)
			if (!app->buffers[i].busy)
				return (int)i;
		{
			struct copresent_record response;

			if (receive_response(app, &response) != 0)
				return -1;
		}
	}
}

static int create_x_window(struct app *app, int width, int height)
{
	int screen;

	app->x_display = XOpenDisplay(NULL);
	if (!app->x_display)
		return -1;
	screen = DefaultScreen(app->x_display);
	app->window = XCreateSimpleWindow(app->x_display,
		RootWindow(app->x_display, screen), 40, 40, width, height, 0,
		BlackPixel(app->x_display, screen), BlackPixel(app->x_display, screen));
	XStoreName(app->x_display, app->window, "MoCoLinux R2 No-Copy Gears");
	XSelectInput(app->x_display, app->window,
		StructureNotifyMask | ExposureMask | KeyPressMask);
	app->wm_delete = XInternAtom(app->x_display, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(app->x_display, app->window, &app->wm_delete, 1);
	XMapRaised(app->x_display, app->window);
	XSync(app->x_display, False);
	{
		XWindowAttributes attributes;

		XGetWindowAttributes(app->x_display, app->window, &attributes);
		app->width = attributes.width;
		app->height = attributes.height;
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct app app;
	int requested_width = argc > 1 ? atoi(argv[1]) : 1600;
	int requested_height = argc > 2 ? atoi(argv[2]) : 837;
	double duration = argc > 3 ? atof(argv[3]) : 20.0;
	double start;
	double last_report;
	unsigned long long last_submitted = 0;
	unsigned long long last_released = 0;
	int running = 1;

	memset(&app, 0, sizeof(app));
	app.socket_fd = -1;
	app.egl_display = EGL_NO_DISPLAY;
	app.egl_context = EGL_NO_CONTEXT;
	app.egl_surface = EGL_NO_SURFACE;
	if (requested_width <= 0 || requested_height <= 0 || duration <= 0.0) {
		fprintf(stderr, "usage: copresent-test [width height seconds]\n");
		return 2;
	}
	if (create_x_window(&app, requested_width, requested_height) != 0) {
		fprintf(stderr, "copresent-test: cannot create X control window\n");
		return 1;
	}
	app.socket_fd = connect_broker();
	if (app.socket_fd < 0) {
		perror("copresent-test: connect " COPRESENT_LOCAL_PATH);
		return 1;
	}
	if (init_egl(&app) != 0 || create_buffers(&app) != 0) {
		fprintf(stderr, "copresent-test: EGL/export/BIND setup failed"
			" (EGL 0x%x, GL 0x%x)\n", eglGetError(), glGetError());
		return 1;
	}
	fprintf(stderr, "copresent-test: XID 0x%lx, %dx%d, two fenced buffers\n",
		app.window, app.width, app.height);

	start = last_report = now_seconds();
	while (running && now_seconds() - start < duration) {
		double now = now_seconds();
		int index;

		while (XPending(app.x_display)) {
			XEvent event;

			XNextEvent(app.x_display, &event);
			if (event.type == ClientMessage &&
			    (Atom)event.xclient.data.l[0] == app.wm_delete)
				running = 0;
			else if (event.type == KeyPress)
				running = 0;
			else if (event.type == ConfigureNotify &&
				 (event.xconfigure.width != app.width ||
				  event.xconfigure.height != app.height)) {
				int width = event.xconfigure.width;
				int height = event.xconfigure.height;

				if (width > 0 && height > 0) {
					if (destroy_buffers(&app) != 0)
						return 1;
					app.width = width;
					app.height = height;
					if (create_buffers(&app) != 0)
						return 1;
					fprintf(stderr, "copresent-test: rebound %dx%d\n",
						width, height);
				}
			}
		}
		if (!running)
			break;
		index = free_buffer(&app);
		if (index < 0 || submit_frame(&app, (unsigned int)index,
				now - start) != 0)
			return 1;
		if (now - last_report >= 2.0) {
			double elapsed = now - last_report;

			fprintf(stderr, "copresent-test: submit %.1f fps, release %.1f fps"
				" (%dx%d)\n",
				(app.submitted - last_submitted) / elapsed,
				(app.released - last_released) / elapsed,
				app.width, app.height);
			last_submitted = app.submitted;
			last_released = app.released;
			last_report = now;
		}
	}
	if (destroy_buffers(&app) != 0)
		return 1;
	fprintf(stderr, "copresent-test: %llu submitted, %llu released in %.2f s"
		" = %.1f no-copy fps\n", app.submitted, app.released,
		now_seconds() - start,
		app.submitted / (now_seconds() - start));
	close(app.socket_fd);
	XDestroyWindow(app.x_display, app.window);
	XCloseDisplay(app.x_display);
	return 0;
}
