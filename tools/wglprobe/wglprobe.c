/*
 * wglprobe -- what OpenGL this host will actually give us, measured.
 *
 * R1 of the GPU ladder. Everything downstream assumes the host can hand our own
 * code a modern core-profile context and read pixels back fast enough to matter;
 * this asks, on the real hardware, and prints numbers rather than opinions.
 *
 * It exists because the only GL measurement this project had came through the
 * guest's GLX path, which reported "1.4 (4.6.0 NVIDIA 388.13)" -- the card's
 * driver is 4.6-capable and the GLX wire protocol is the thing capping it. That
 * is an argument, not evidence, until our own process asks WGL directly.
 *
 * Three things are measured and one is observed:
 *
 *   1. The highest core-profile context wglCreateContextAttribsARB will give,
 *      descending 4.6 -> 3.3 the way virglrenderer's own probe does, plus the
 *      vendor/renderer/version strings. "GDI Generic" as the renderer is a
 *      failure whatever the version says -- it means we got Microsoft's software
 *      rasterizer because the pixel format was wrong, and the chosen format
 *      index is printed so that is visible rather than mysterious.
 *
 *   2. A deterministic render (gradient + triangle) into an FBO, CRC32'd over
 *      100 consecutive frames. The check is that the CRC is STABLE on this host,
 *      not that it matches another machine: GL guarantees no cross-implementation
 *      rasterisation invariance, so comparing NVIDIA's CRC against llvmpipe's
 *      would fail for a correct renderer. Interior pixel probes are printed for
 *      the cross-host comparison instead, which is what a human can judge.
 *
 *   3. glReadPixels throughput at 1024x768 and 1920x1080. R6 ships frames to the
 *      X server by reading them back, so this number sets that rung's fps gate
 *      before a line of it is written. GL_BGRA/GL_UNSIGNED_INT_8_8_8_8_REV is the
 *      format NVIDIA hands over without a CPU swizzle, and is what R6 will use.
 *
 *   4. With --hwnd, it draws into a window belonging to the running VcXsrv rather
 *      than its own, so a human can drag and overlap it and see what survives.
 *      That is R6b's spike question asked cheaply, and nothing here depends on
 *      the answer.
 *
 * Build (same recipe as every other userspace binary here -- the Universal CRT
 * does not exist on XP and a Win7 without KB2999226 has not got it either):
 *
 *   x86_64-w64-mingw32-gcc wglprobe.c -o wglprobe.exe -O2 \
 *       -mcrtdll=msvcrt-os -lopengl32 -lgdi32 -luser32
 *
 * Run it at the console. Over RDP the session has no real GPU and you will get
 * the software rasteriser -- which the renderer-string gate catches, but knowing
 * why saves an hour.
 */

#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------ GL plumbing */

/*
 * Windows ships opengl32.dll at version 1.1 and everything since arrives through
 * wglGetProcAddress, so every entry point past 1996 is declared by hand. Tedious
 * and unavoidable; the alternative is a loader library this project would then
 * have to cross-compile for XP.
 */
#define GL_ARRAY_BUFFER                 0x8892
#define GL_STATIC_DRAW                  0x88E4
#define GL_FRAGMENT_SHADER              0x8B30
#define GL_VERTEX_SHADER                0x8B31
#define GL_COMPILE_STATUS               0x8B81
#define GL_LINK_STATUS                  0x8B82
#define GL_FRAMEBUFFER                  0x8D40
#define GL_COLOR_ATTACHMENT0            0x8CE0
#define GL_FRAMEBUFFER_COMPLETE         0x8CD5
#define GL_BGRA                         0x80E1
#define GL_UNSIGNED_INT_8_8_8_8_REV     0x8367
#define GL_MAJOR_VERSION                0x821B
#define GL_MINOR_VERSION                0x821C

#define WGL_CONTEXT_MAJOR_VERSION_ARB   0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB   0x2092
#define WGL_CONTEXT_FLAGS_ARB           0x2094
#define WGL_CONTEXT_PROFILE_MASK_ARB    0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB          0x00000001
#define WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 0x00000002

typedef HGLRC (WINAPI *PFNWGLCREATECONTEXTATTRIBSARB)(HDC, HGLRC, const int *);

typedef void (APIENTRY *PFNGENFRAMEBUFFERS)(GLsizei, GLuint *);
typedef void (APIENTRY *PFNBINDFRAMEBUFFER)(GLenum, GLuint);
typedef void (APIENTRY *PFNFRAMEBUFFERTEXTURE2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (APIENTRY *PFNCHECKFRAMEBUFFERSTATUS)(GLenum);
typedef void (APIENTRY *PFNGENVERTEXARRAYS)(GLsizei, GLuint *);
typedef void (APIENTRY *PFNBINDVERTEXARRAY)(GLuint);
typedef void (APIENTRY *PFNGENBUFFERS)(GLsizei, GLuint *);
typedef void (APIENTRY *PFNBINDBUFFER)(GLenum, GLuint);
typedef void (APIENTRY *PFNBUFFERDATA)(GLenum, ptrdiff_t, const void *, GLenum);
typedef GLuint (APIENTRY *PFNCREATESHADER)(GLenum);
typedef void (APIENTRY *PFNSHADERSOURCE)(GLuint, GLsizei, const char * const *, const GLint *);
typedef void (APIENTRY *PFNCOMPILESHADER)(GLuint);
typedef void (APIENTRY *PFNGETSHADERIV)(GLuint, GLenum, GLint *);
typedef void (APIENTRY *PFNGETSHADERINFOLOG)(GLuint, GLsizei, GLsizei *, char *);
typedef GLuint (APIENTRY *PFNCREATEPROGRAM)(void);
typedef void (APIENTRY *PFNATTACHSHADER)(GLuint, GLuint);
typedef void (APIENTRY *PFNLINKPROGRAM)(GLuint);
typedef void (APIENTRY *PFNGETPROGRAMIV)(GLuint, GLenum, GLint *);
typedef void (APIENTRY *PFNUSEPROGRAM)(GLuint);
typedef void (APIENTRY *PFNVERTEXATTRIBPOINTER)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
typedef void (APIENTRY *PFNENABLEVERTEXATTRIBARRAY)(GLuint);
typedef void (APIENTRY *PFNGETINTEGERV_)(GLenum, GLint *);

static PFNGENFRAMEBUFFERS         pGenFramebuffers;
static PFNBINDFRAMEBUFFER         pBindFramebuffer;
static PFNFRAMEBUFFERTEXTURE2D    pFramebufferTexture2D;
static PFNCHECKFRAMEBUFFERSTATUS  pCheckFramebufferStatus;
static PFNGENVERTEXARRAYS         pGenVertexArrays;
static PFNBINDVERTEXARRAY         pBindVertexArray;
static PFNGENBUFFERS              pGenBuffers;
static PFNBINDBUFFER              pBindBuffer;
static PFNBUFFERDATA              pBufferData;
static PFNCREATESHADER            pCreateShader;
static PFNSHADERSOURCE            pShaderSource;
static PFNCOMPILESHADER           pCompileShader;
static PFNGETSHADERIV             pGetShaderiv;
static PFNGETSHADERINFOLOG        pGetShaderInfoLog;
static PFNCREATEPROGRAM           pCreateProgram;
static PFNATTACHSHADER            pAttachShader;
static PFNLINKPROGRAM             pLinkProgram;
static PFNGETPROGRAMIV            pGetProgramiv;
static PFNUSEPROGRAM              pUseProgram;
static PFNVERTEXATTRIBPOINTER     pVertexAttribPointer;
static PFNENABLEVERTEXATTRIBARRAY pEnableVertexAttribArray;

static void *gl_get(const char *name)
{
	void *p = (void *)wglGetProcAddress(name);

	/*
	 * wglGetProcAddress returns NULL for GL 1.1 entry points -- they live in
	 * opengl32.dll itself -- and on some drivers returns 1, 2, 3 or -1 rather
	 * than NULL for genuinely missing ones. Both quirks are older than this
	 * project and both still bite.
	 */
	if (p == NULL || p == (void *)1 || p == (void *)2 ||
	    p == (void *)3 || p == (void *)-1) {
		HMODULE m = GetModuleHandleA("opengl32.dll");

		p = m ? (void *)GetProcAddress(m, name) : NULL;
	}
	return p;
}

static int load_gl(void)
{
	pGenFramebuffers         = (PFNGENFRAMEBUFFERS)gl_get("glGenFramebuffers");
	pBindFramebuffer         = (PFNBINDFRAMEBUFFER)gl_get("glBindFramebuffer");
	pFramebufferTexture2D    = (PFNFRAMEBUFFERTEXTURE2D)gl_get("glFramebufferTexture2D");
	pCheckFramebufferStatus  = (PFNCHECKFRAMEBUFFERSTATUS)gl_get("glCheckFramebufferStatus");
	pGenVertexArrays         = (PFNGENVERTEXARRAYS)gl_get("glGenVertexArrays");
	pBindVertexArray         = (PFNBINDVERTEXARRAY)gl_get("glBindVertexArray");
	pGenBuffers              = (PFNGENBUFFERS)gl_get("glGenBuffers");
	pBindBuffer              = (PFNBINDBUFFER)gl_get("glBindBuffer");
	pBufferData              = (PFNBUFFERDATA)gl_get("glBufferData");
	pCreateShader            = (PFNCREATESHADER)gl_get("glCreateShader");
	pShaderSource            = (PFNSHADERSOURCE)gl_get("glShaderSource");
	pCompileShader           = (PFNCOMPILESHADER)gl_get("glCompileShader");
	pGetShaderiv             = (PFNGETSHADERIV)gl_get("glGetShaderiv");
	pGetShaderInfoLog        = (PFNGETSHADERINFOLOG)gl_get("glGetShaderInfoLog");
	pCreateProgram           = (PFNCREATEPROGRAM)gl_get("glCreateProgram");
	pAttachShader            = (PFNATTACHSHADER)gl_get("glAttachShader");
	pLinkProgram             = (PFNLINKPROGRAM)gl_get("glLinkProgram");
	pGetProgramiv            = (PFNGETPROGRAMIV)gl_get("glGetProgramiv");
	pUseProgram              = (PFNUSEPROGRAM)gl_get("glUseProgram");
	pVertexAttribPointer     = (PFNVERTEXATTRIBPOINTER)gl_get("glVertexAttribPointer");
	pEnableVertexAttribArray = (PFNENABLEVERTEXATTRIBARRAY)gl_get("glEnableVertexAttribArray");

	return pGenFramebuffers && pBindFramebuffer && pFramebufferTexture2D &&
	       pCheckFramebufferStatus && pGenVertexArrays && pBindVertexArray &&
	       pGenBuffers && pBindBuffer && pBufferData && pCreateShader &&
	       pShaderSource && pCompileShader && pCreateProgram && pAttachShader &&
	       pLinkProgram && pUseProgram && pVertexAttribPointer &&
	       pEnableVertexAttribArray;
}

/* ------------------------------------------------------------------- CRC32 */

static unsigned int crc_table[256];

static void crc_init(void)
{
	unsigned int i, j, c;

	for (i = 0; i < 256; i++) {
		c = i;
		for (j = 0; j < 8; j++)
			c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
		crc_table[i] = c;
	}
}

static unsigned int crc32(const void *data, size_t n)
{
	const unsigned char *p = (const unsigned char *)data;
	unsigned int c = 0xffffffffu;
	size_t i;

	for (i = 0; i < n; i++)
		c = crc_table[(c ^ p[i]) & 0xff] ^ (c >> 8);
	return c ^ 0xffffffffu;
}

/* ------------------------------------------------------------- the context */

static const char *VS =
	"#version 330 core\n"
	"layout(location=0) in vec2 pos;\n"
	"out vec2 uv;\n"
	"void main(){ uv = pos*0.5+0.5; gl_Position = vec4(pos,0.0,1.0); }\n";

/*
 * Deterministic on purpose: no time, no randomness, no depth test. The same
 * frame every frame is what makes a stable CRC mean "the pipeline is
 * repeatable" rather than "nothing moved".
 */
static const char *FS =
	"#version 330 core\n"
	"in vec2 uv;\n"
	"out vec4 col;\n"
	"void main(){ col = vec4(uv.x, uv.y, 1.0-uv.x*uv.y, 1.0); }\n";

static GLuint make_shader(GLenum kind, const char *src, const char *what)
{
	GLuint s = pCreateShader(kind);
	GLint ok = 0;

	pShaderSource(s, 1, &src, NULL);
	pCompileShader(s);
	if (pGetShaderiv) {
		pGetShaderiv(s, GL_COMPILE_STATUS, &ok);
		if (!ok) {
			char log[1024] = {0};

			if (pGetShaderInfoLog)
				pGetShaderInfoLog(s, sizeof(log) - 1, NULL, log);
			printf("  %s shader failed to compile: %s\n", what, log);
			return 0;
		}
	}
	return s;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	return DefWindowProc(h, m, w, l);
}

struct ctx {
	HWND  wnd;
	HDC   dc;
	HGLRC rc;
	int   pixel_format;
	int   major, minor;
	int   core;          /* 1 if a core profile was obtained */
};

/*
 * The two-step every Windows GL program performs: a throwaway 1.1 context
 * exists only so wglCreateContextAttribsARB can be looked up, because the
 * function that creates a modern context can only be found through an old one.
 */
static int make_context(struct ctx *c, HWND target, int want_core)
{
	PIXELFORMATDESCRIPTOR pfd;
	HGLRC tmp;
	PFNWGLCREATECONTEXTATTRIBSARB createattribs;
	static const int versions[][2] = {
		{4,6},{4,5},{4,4},{4,3},{4,2},{4,1},{4,0},{3,3}
	};
	int i;

	ZeroMemory(&pfd, sizeof(pfd));
	pfd.nSize = sizeof(pfd);
	pfd.nVersion = 1;
	pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	pfd.iPixelType = PFD_TYPE_RGBA;
	pfd.cColorBits = 32;
	pfd.cDepthBits = 24;

	c->wnd = target;
	c->dc = GetDC(c->wnd);
	if (!c->dc) {
		printf("  GetDC failed (%lu)\n", (unsigned long)GetLastError());
		return 0;
	}

	c->pixel_format = ChoosePixelFormat(c->dc, &pfd);
	if (!c->pixel_format) {
		printf("  ChoosePixelFormat failed (%lu)\n", (unsigned long)GetLastError());
		return 0;
	}
	/*
	 * SetPixelFormat can only be done ONCE per window, ever. That is the whole
	 * reason R6b's "draw into the X server's window" spike is delicate: if
	 * VcXsrv has already set a format on that HWND, ours is refused, and if we
	 * set one first the server's own -wgl path is the one that loses.
	 */
	if (!SetPixelFormat(c->dc, c->pixel_format, &pfd)) {
		printf("  SetPixelFormat(%d) failed (%lu) -- another process may own"
		       " this window's format\n",
		       c->pixel_format, (unsigned long)GetLastError());
		return 0;
	}

	tmp = wglCreateContext(c->dc);
	if (!tmp || !wglMakeCurrent(c->dc, tmp)) {
		printf("  legacy wglCreateContext failed (%lu)\n",
		       (unsigned long)GetLastError());
		return 0;
	}

	createattribs = (PFNWGLCREATECONTEXTATTRIBSARB)
		wglGetProcAddress("wglCreateContextAttribsARB");
	if (!createattribs) {
		printf("  no wglCreateContextAttribsARB: this is a pre-3.0 driver or"
		       " the software rasteriser\n");
		c->rc = tmp;
		c->major = 1; c->minor = 1; c->core = 0;
		return 1;
	}

	for (i = 0; i < (int)(sizeof(versions) / sizeof(versions[0])); i++) {
		int attribs[] = {
			WGL_CONTEXT_MAJOR_VERSION_ARB, versions[i][0],
			WGL_CONTEXT_MINOR_VERSION_ARB, versions[i][1],
			WGL_CONTEXT_PROFILE_MASK_ARB,
				want_core ? WGL_CONTEXT_CORE_PROFILE_BIT_ARB
					  : WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB,
			0
		};
		HGLRC rc = createattribs(c->dc, NULL, attribs);

		if (rc) {
			wglMakeCurrent(NULL, NULL);
			wglDeleteContext(tmp);
			wglMakeCurrent(c->dc, rc);
			c->rc = rc;
			c->major = versions[i][0];
			c->minor = versions[i][1];
			c->core = want_core;
			return 1;
		}
	}

	printf("  no core context at any version 4.6..3.3\n");
	c->rc = tmp;
	c->major = 1; c->minor = 1; c->core = 0;
	return 1;
}

/* ----------------------------------------------------------- the measurement */

static double now_ms(void)
{
	LARGE_INTEGER f, t;

	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t);
	return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

static int measure(int w, int h, int frames)
{
	GLuint tex = 0, fbo = 0, vao = 0, vbo = 0, prog = 0, vs, fs;
	static const float verts[] = {
		-0.9f, -0.9f,   0.9f, -0.9f,   0.0f,  0.9f
	};
	unsigned char *pixels;
	unsigned int first_crc = 0;
	int stable = 1, i;
	double t0, t1;
	size_t bytes = (size_t)w * h * 4;

	pixels = (unsigned char *)malloc(bytes);
	if (!pixels) {
		printf("  out of memory for %dx%d readback\n", w, h);
		return 0;
	}

	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
		     GL_UNSIGNED_BYTE, NULL);

	pGenFramebuffers(1, &fbo);
	pBindFramebuffer(GL_FRAMEBUFFER, fbo);
	pFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			      GL_TEXTURE_2D, tex, 0);
	if (pCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		printf("  framebuffer incomplete at %dx%d\n", w, h);
		free(pixels);
		return 0;
	}

	pGenVertexArrays(1, &vao);
	pBindVertexArray(vao);
	pGenBuffers(1, &vbo);
	pBindBuffer(GL_ARRAY_BUFFER, vbo);
	pBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
	pVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	pEnableVertexAttribArray(0);

	vs = make_shader(GL_VERTEX_SHADER, VS, "vertex");
	fs = make_shader(GL_FRAGMENT_SHADER, FS, "fragment");
	if (!vs || !fs) {
		free(pixels);
		return 0;
	}
	prog = pCreateProgram();
	pAttachShader(prog, vs);
	pAttachShader(prog, fs);
	pLinkProgram(prog);
	pUseProgram(prog);

	glViewport(0, 0, w, h);

	/* One warm-up frame, outside the timing: the first draw pays for shader
	 * upload and buffer residency and would otherwise land in the average. */
	glClearColor(0.10f, 0.12f, 0.15f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glFinish();

	/*
	 * Two loops, because one loop measured the wrong thing.
	 *
	 * The first version timed render + readback + CRC together, and a bytewise
	 * CRC32 over 8 MB a frame is several milliseconds of single-threaded CPU --
	 * at 1080p it was most of the budget, so the "readback MB/s" it printed was
	 * really "readback and checksum MB/s" and understated the transport R6
	 * depends on. R6 ships frames; it does not checksum them. So the timed loop
	 * does exactly what R6 will do, and correctness is established separately in
	 * an untimed loop.
	 */
	t0 = now_ms();
	for (i = 0; i < frames; i++) {
		glClearColor(0.10f, 0.12f, 0.15f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glReadPixels(0, 0, w, h, GL_BGRA,
			     GL_UNSIGNED_INT_8_8_8_8_REV, pixels);
	}
	glFinish();
	t1 = now_ms();

	/* Stability, untimed: every frame must be bit-identical to the first. */
	for (i = 0; i < frames; i++) {
		unsigned int c;

		glClearColor(0.10f, 0.12f, 0.15f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glReadPixels(0, 0, w, h, GL_BGRA,
			     GL_UNSIGNED_INT_8_8_8_8_REV, pixels);

		c = crc32(pixels, bytes);
		if (i == 0)
			first_crc = c;
		else if (c != first_crc)
			stable = 0;
	}

	{
		double secs = (t1 - t0) / 1000.0;
		double mb = (double)bytes * frames / (1024.0 * 1024.0);
		/* Interior probes, for comparing two different renderers by eye --
		 * a CRC cannot be compared across implementations, a pixel can. */
		size_t c_off = ((size_t)(h / 2) * w + (w / 2)) * 4;
		size_t q_off = ((size_t)(h / 4) * w + (w / 4)) * 4;

		printf("  %dx%d  %d frames in %.1f ms  =  %.1f MB/s readback, %.1f fps\n",
		       w, h, frames, t1 - t0, mb / secs, frames / secs);
		printf("    frame CRC32 %08x, %s across all %d frames\n",
		       first_crc, stable ? "IDENTICAL" : "UNSTABLE -- FAIL", frames);
		printf("    probe centre  BGRA %02x %02x %02x %02x\n",
		       pixels[c_off], pixels[c_off+1], pixels[c_off+2], pixels[c_off+3]);
		printf("    probe quarter BGRA %02x %02x %02x %02x\n",
		       pixels[q_off], pixels[q_off+1], pixels[q_off+2], pixels[q_off+3]);
	}

	free(pixels);
	return stable;
}

/* ------------------------------------------------------------ the X window */

struct find_ctx {
	HWND found;
	char cls[256];
};

static BOOL CALLBACK find_vcxsrv(HWND h, LPARAM p)
{
	struct find_ctx *f = (struct find_ctx *)p;
	char cls[256] = {0};

	if (!IsWindowVisible(h))
		return TRUE;
	GetClassNameA(h, cls, sizeof(cls) - 1);
	/* The fork names its top-level windows for the X window they carry; the
	 * class string is what identifies them from outside the server. */
	if (strstr(cls, "cygwin/x") || strstr(cls, "vcxsrv/x") ||
	    strstr(cls, "VcXsrv") || strstr(cls, "XWin")) {
		f->found = h;
		lstrcpynA(f->cls, cls, sizeof(f->cls));
		return FALSE;
	}
	return TRUE;
}

int main(int argc, char **argv)
{
	struct ctx c;
	WNDCLASSA wc;
	HWND wnd;
	int want_hwnd = 0, want_compat = 0, i, ok = 1;
	const char *vendor, *renderer, *version, *glsl;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--hwnd"))
			want_hwnd = 1;
		else if (!strcmp(argv[i], "--compat"))
			want_compat = 1;
		else {
			printf("usage: wglprobe [--hwnd] [--compat]\n");
			return 2;
		}
	}

	crc_init();
	ZeroMemory(&c, sizeof(c));
	ZeroMemory(&wc, sizeof(wc));
	wc.lpfnWndProc = wndproc;
	wc.hInstance = GetModuleHandle(NULL);
	wc.lpszClassName = "wglprobe";
	RegisterClassA(&wc);

	printf("wglprobe -- R1, what this host's OpenGL actually is\n\n");

	if (want_hwnd) {
		struct find_ctx f;

		ZeroMemory(&f, sizeof(f));
		EnumWindows(find_vcxsrv, (LPARAM)&f);
		if (!f.found) {
			printf("no VcXsrv window found -- start the X server and open an"
			       " X client first\n");
			return 1;
		}
		printf("targeting a live X server window: HWND %p, class \"%s\"\n\n",
		       (void *)f.found, f.cls);
		wnd = f.found;
	} else {
		wnd = CreateWindowExA(0, "wglprobe", "wglprobe",
				      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
				      CW_USEDEFAULT, 640, 480, NULL, NULL,
				      wc.hInstance, NULL);
		if (!wnd) {
			printf("CreateWindow failed (%lu)\n",
			       (unsigned long)GetLastError());
			return 1;
		}
		/* Deliberately not shown: the render goes to an FBO and a visible
		 * window would only invite the compositor into the measurement. */
	}

	if (!make_context(&c, wnd, !want_compat))
		return 1;

	vendor   = (const char *)glGetString(GL_VENDOR);
	renderer = (const char *)glGetString(GL_RENDERER);
	version  = (const char *)glGetString(GL_VERSION);
	glsl     = (const char *)glGetString(0x8B8C /* SHADING_LANGUAGE_VERSION */);

	printf("pixel format index : %d\n", c.pixel_format);
	printf("context requested  : %d.%d %s\n", c.major, c.minor,
	       c.core ? "core" : "compatibility/legacy");
	printf("GL_VENDOR          : %s\n", vendor ? vendor : "(null)");
	printf("GL_RENDERER        : %s\n", renderer ? renderer : "(null)");
	printf("GL_VERSION         : %s\n", version ? version : "(null)");
	printf("GL_SHADING_LANGUAGE: %s\n", glsl ? glsl : "(null)");
	printf("\n");

	if (renderer && strstr(renderer, "GDI Generic")) {
		printf("FAIL: this is Microsoft's software rasteriser, not the card.\n"
		       "      Run at the console (not RDP) and check the driver.\n");
		return 1;
	}
	if (!c.core || c.major < 3 || (c.major == 3 && c.minor < 3)) {
		printf("FAIL: no core profile at 3.3 or better.\n");
		ok = 0;
	}

	if (!load_gl()) {
		printf("FAIL: the modern entry points this needs are missing.\n");
		return 1;
	}

	printf("rendering and reading back:\n");
	if (!measure(1024, 768, 100))
		ok = 0;
	if (!measure(1920, 1080, 100))
		ok = 0;

	if (want_hwnd) {
		/* Leave something on the glass to drag around: this is the whole
		 * point of --hwnd, and it must be on the front buffer to survive
		 * the server's own painting long enough to observe. */
		pBindFramebuffer(GL_FRAMEBUFFER, 0);
		glClearColor(0.9f, 0.2f, 0.4f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		SwapBuffers(c.dc);
		printf("\ndrew into the X server's window. Drag it, overlap it, and\n"
		       "watch what survives -- that observation is R6b's input.\n");
		Sleep(20000);
	}

	printf("\n%s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
