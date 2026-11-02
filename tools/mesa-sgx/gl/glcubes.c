// SPDX-License-Identifier: GPL-2.0-only
/* glcubes -- a GPU load and frame-rate benchmark: N x N x N textured cubes
 * in front of the camera, through surfaceless EGL and OpenGL ES 2.0.
 *
 *   sgx-gl glcubes                     # 10x10x10 cubes, 300 frames
 *   sgx-gl glcubes --frames 1000
 *   sgx-gl glcubes --frames 0          # setup/load time only
 *   sgx-gl glcubes --grid 12 --width 1920 --height 1080
 *
 * Every cube comes from three tightly packed vertex buffers (position, uv,
 * face shade) and a 64x64 procedural texture; one draw call renders the
 * whole grid.  The grid turns about two axes, so the frame is new every
 * time while the geometry and the draw count do not change.
 *
 * "load" is wall time from process start to the first frame finished: EGL,
 * context, FBO, texture, buffers and the driver's shader compile (which
 * happens at the first draw).  "fps" is measured over --frames frames,
 * each ended with glFinish() so the number is GPU time, not queue depth.
 * There is no swap: this is the renderer's throughput, not the display's
 * refresh.
 *
 * The same EGL/GBM path on HDMI is exercised by gbmtest; this program is a
 * repeatable load, and prints one machine-readable glcubes_result line.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEFAULT_WIDTH	1280
#define DEFAULT_HEIGHT	720
#define DEFAULT_GRID	10
#define DEFAULT_FRAMES	300
#define DEFAULT_WARMUP	5

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int fail(const char *what)
{
	fprintf(stderr, "glcubes: %s (EGL 0x%x, GL 0x%x)\n", what, eglGetError(),
		glGetError());
	return 1;
}

/* ---- 4x4 matrices, column-major as GL takes them ------------------------ */

static void mat_mul(float *out, const float *a, const float *b)
{
	float r[16];

	for (int c = 0; c < 4; c++)
		for (int row = 0; row < 4; row++) {
			float v = 0;

			for (int k = 0; k < 4; k++)
				v += a[k * 4 + row] * b[c * 4 + k];
			r[c * 4 + row] = v;
		}
	memcpy(out, r, sizeof(r));
}

static void mat_perspective(float *m, float fovy_deg, float aspect,
			    float near, float far)
{
	float f = 1.0f / tanf(fovy_deg * (float)M_PI / 360.0f);

	memset(m, 0, 16 * sizeof(float));
	m[0] = f / aspect;
	m[5] = f;
	m[10] = (far + near) / (near - far);
	m[11] = -1.0f;
	m[14] = (2.0f * far * near) / (near - far);
}

static void mat_translate(float *m, float x, float y, float z)
{
	memset(m, 0, 16 * sizeof(float));
	m[0] = m[5] = m[10] = m[15] = 1.0f;
	m[12] = x;
	m[13] = y;
	m[14] = z;
}

static void mat_rotate_x(float *m, float a)
{
	float c = cosf(a), s = sinf(a);

	memset(m, 0, 16 * sizeof(float));
	m[0] = m[15] = 1.0f;
	m[5] = c;
	m[6] = s;
	m[9] = -s;
	m[10] = c;
}

static void mat_rotate_y(float *m, float a)
{
	float c = cosf(a), s = sinf(a);

	memset(m, 0, 16 * sizeof(float));
	m[5] = m[15] = 1.0f;
	m[0] = c;
	m[2] = -s;
	m[8] = s;
	m[10] = c;
}

/* ---- GL helpers --------------------------------------------------------- */

static GLuint make_shader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	GLint ok = 0;
	char log[1024];

	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		fprintf(stderr, "glcubes: %s shader: %s\n",
			type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
		glDeleteShader(s);
		return 0;
	}
	return s;
}

static GLuint make_program(void)
{
	static const char *vs =
		"attribute vec3 a_pos;\n"
		"attribute vec2 a_uv;\n"
		"attribute float a_shade;\n"
		"uniform mat4 u_mvp;\n"
		"varying vec2 v_uv;\n"
		"varying float v_shade;\n"
		"void main() {\n"
		"  v_uv = a_uv;\n"
		"  v_shade = a_shade;\n"
		"  gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
		"}\n";
	static const char *fs =
		"precision mediump float;\n"
		"uniform sampler2D u_tex;\n"
		"varying vec2 v_uv;\n"
		"varying float v_shade;\n"
		"void main() {\n"
		"  gl_FragColor = texture2D(u_tex, v_uv) *\n"
		"                 vec4(v_shade, v_shade, v_shade, 1.0);\n"
		"}\n";
	GLuint v = make_shader(GL_VERTEX_SHADER, vs);
	GLuint f = make_shader(GL_FRAGMENT_SHADER, fs);
	GLuint p;
	GLint ok = 0;
	char log[1024];

	if (!v || !f)
		return 0;
	p = glCreateProgram();
	glAttachShader(p, v);
	glAttachShader(p, f);
	glBindAttribLocation(p, 0, "a_pos");
	glBindAttribLocation(p, 1, "a_uv");
	glBindAttribLocation(p, 2, "a_shade");
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		glGetProgramInfoLog(p, sizeof(log), NULL, log);
		fprintf(stderr, "glcubes: link: %s\n", log);
		glDeleteProgram(p);
		p = 0;
	}
	glDeleteShader(v);
	glDeleteShader(f);
	return p;
}

/* 64x64 RGBA: an 8x8 checkerboard with a coloured border and a diagonal, so
 * minification, mipmaps and the wrap mode all show. */
static void make_texture(uint8_t *px, unsigned size)
{
	for (unsigned y = 0; y < size; y++) {
		for (unsigned x = 0; x < size; x++) {
			uint8_t *p = px + 4 * (y * size + x);
			unsigned c = ((x / 8) + (y / 8)) & 1;
			int border = x < 3 || y < 3 || x >= size - 3 || y >= size - 3;
			int diag = (x / 4) == (y / 4);

			if (border) {
				p[0] = 20;
				p[1] = 20;
				p[2] = 20;
			} else if (diag) {
				p[0] = 240;
				p[1] = 210;
				p[2] = 40;
			} else {
				p[0] = c ? 220 : 60;
				p[1] = c ? 90 : 60;
				p[2] = c ? 60 : 200;
			}
			p[3] = 255;
		}
	}
}

/* ---- the grid ----------------------------------------------------------- */

/* Four corners of each face, counter-clockwise seen from outside. */
static const float face_pos[6][4][3] = {
	{ { -1, -1,  1 }, {  1, -1,  1 }, {  1,  1,  1 }, { -1,  1,  1 } },
	{ {  1, -1, -1 }, { -1, -1, -1 }, { -1,  1, -1 }, {  1,  1, -1 } },
	{ {  1, -1,  1 }, {  1, -1, -1 }, {  1,  1, -1 }, {  1,  1,  1 } },
	{ { -1, -1, -1 }, { -1, -1,  1 }, { -1,  1,  1 }, { -1,  1, -1 } },
	{ { -1,  1,  1 }, {  1,  1,  1 }, {  1,  1, -1 }, { -1,  1, -1 } },
	{ { -1, -1, -1 }, {  1, -1, -1 }, {  1, -1,  1 }, { -1, -1,  1 } },
};
static const float face_uv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
static const float face_shade[6] = { 1.0f, 0.55f, 0.80f, 0.45f, 0.90f, 0.35f };

/* Three tightly packed arrays: position (3 floats a vertex), uv (2) and face
 * shade (1), all n^3 * 36 vertices long. */
static int build_cubes(unsigned n, float **pos, float **uv, float **shade,
		       unsigned *count)
{
	unsigned nv = n * n * n * 36, at = 0;
	float half = 0.4f, off = (n - 1) * 0.5f;

	*pos = malloc((size_t)nv * 3 * sizeof(float));
	*uv = malloc((size_t)nv * 2 * sizeof(float));
	*shade = malloc((size_t)nv * sizeof(float));
	if (!*pos || !*uv || !*shade) {
		free(*pos);
		free(*uv);
		free(*shade);
		return 0;
	}
	for (unsigned i = 0; i < n; i++) {
		for (unsigned j = 0; j < n; j++) {
			for (unsigned k = 0; k < n; k++) {
				float cx = (i - off) * 1.05f;
				float cy = (j - off) * 1.05f;
				float cz = (k - off) * 1.05f;

				for (unsigned f = 0; f < 6; f++) {
					static const unsigned order[6] = { 0, 1, 2, 0, 2, 3 };

					for (unsigned q = 0; q < 6; q++) {
						unsigned c = order[q];

						(*pos)[at * 3 + 0] = cx + face_pos[f][c][0] * half;
						(*pos)[at * 3 + 1] = cy + face_pos[f][c][1] * half;
						(*pos)[at * 3 + 2] = cz + face_pos[f][c][2] * half;
						(*uv)[at * 2 + 0] = face_uv[c][0];
						(*uv)[at * 2 + 1] = face_uv[c][1];
						(*shade)[at] = face_shade[f];
						at++;
					}
				}
			}
		}
	}
	*count = nv;
	return 1;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [--grid N] [--frames N] [--warmup N] "
		"[--width N] [--height N] [--nocull]\n"
		"\n"
		"  --grid N     cubes on a side (default %u; N^3 cubes)\n"
		"  --frames N   measured frames (default %u; 0: load only)\n"
		"  --warmup N   frames before measuring (default %u)\n"
		"  --width N    render target width (default %u)\n"
		"  --height N   render target height (default %u)\n"
		"  --nocull     draw both sides of every face\n",
		argv0, DEFAULT_GRID, DEFAULT_FRAMES, DEFAULT_WARMUP,
		DEFAULT_WIDTH, DEFAULT_HEIGHT);
}

/* How many of three 32x32 samples across the middle are not the clear
 * colour.  A 10x10x10 grid is square, so the middle row is the part that is
 * covered at every rotation. */
static unsigned coverage(unsigned w, unsigned h, unsigned *out_of)
{
	const unsigned xs[3] = { w / 2, w / 3, 2 * w / 3 };
	uint8_t *p = malloc(32 * 32 * 4);
	unsigned n = 0;

	*out_of = 0;
	if (!p)
		return 0;
	for (unsigned b = 0; b < 3; b++) {
		unsigned x = xs[b], y = h / 2;

		if (x < 32 || y < 32 || x + 32 > w || y + 32 > h)
			continue;
		glReadPixels(x, y, 32, 32, GL_RGBA, GL_UNSIGNED_BYTE, p);
		for (unsigned i = 0; i < 32 * 32; i++)
			if (p[4 * i] != 10 || p[4 * i + 1] != 13 || p[4 * i + 2] != 23)
				n++;
		*out_of += 32 * 32;
	}
	free(p);
	return n;
}

int main(int argc, char **argv)
{
	unsigned grid = DEFAULT_GRID, frames = DEFAULT_FRAMES, warmup = DEFAULT_WARMUP;
	unsigned width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT;
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	unsigned nverts = 0, nonclear = 0, sampled = 0;
	float *pos = NULL, *uv = NULL, *shade = NULL;
	EGLDisplay dpy;
	EGLContext ctx;
	GLuint prog = 0, vbo[3] = { 0, 0, 0 }, tex = 0, color = 0, depth = 0, fbo = 0;
	GLint u_mvp, u_tex;
	double t0, t_egl, t_setup, t_ready, t_start, t_end;
	double min_ms = 1e30, max_ms = 0, sum_ms = 0;
	int cull = 1;
	int ret = 1;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--grid") && i + 1 < argc)
			grid = strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
			frames = strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--warmup") && i + 1 < argc)
			warmup = strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--width") && i + 1 < argc)
			width = strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--height") && i + 1 < argc)
			height = strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--nocull"))
			cull = 0;
		else {
			usage(argv[0]);
			return 2;
		}
	}
	if (!grid || !width || !height || width > 4096 || height > 4096) {
		usage(argv[0]);
		return 2;
	}

	t0 = now();

	/* ---- EGL and the GLES 2 context ---- */
	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
		eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ?
		get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) :
		NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
		return fail("no EGL");
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT ||
	    !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fail("no GLES 2 context");
	t_egl = now();

	/* ---- programs, geometry, texture, render target ---- */
	prog = make_program();
	if (!prog)
		goto out;
	u_mvp = glGetUniformLocation(prog, "u_mvp");
	u_tex = glGetUniformLocation(prog, "u_tex");

	if (!build_cubes(grid, &pos, &uv, &shade, &nverts)) {
		fprintf(stderr, "glcubes: no memory for %u cubes\n", grid * grid * grid);
		goto out;
	}
	glGenBuffers(3, vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo[0]);
	glBufferData(GL_ARRAY_BUFFER, (size_t)nverts * 3 * sizeof(float), pos,
		     GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, vbo[1]);
	glBufferData(GL_ARRAY_BUFFER, (size_t)nverts * 2 * sizeof(float), uv,
		     GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, vbo[2]);
	glBufferData(GL_ARRAY_BUFFER, (size_t)nverts * sizeof(float), shade,
		     GL_STATIC_DRAW);
	free(pos);
	free(uv);
	free(shade);
	pos = uv = shade = NULL;

	{
		uint8_t *px = malloc(64 * 64 * 4);

		if (!px) {
			fprintf(stderr, "glcubes: no memory for the texture\n");
			goto out;
		}
		make_texture(px, 64);
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA,
			     GL_UNSIGNED_BYTE, px);
		free(px);
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glGenerateMipmap(GL_TEXTURE_2D);

	glGenTextures(1, &color);
	glBindTexture(GL_TEXTURE_2D, color);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
		     GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glGenRenderbuffers(1, &depth);
	glBindRenderbuffer(GL_RENDERBUFFER, depth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, width, height);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
			       color, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
				  GL_RENDERBUFFER, depth);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "glcubes: incomplete framebuffer\n");
		goto out;
	}
	glViewport(0, 0, width, height);
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	if (cull) {
		glEnable(GL_CULL_FACE);
		glCullFace(GL_BACK);
	}
	glClearColor(0.04f, 0.05f, 0.09f, 1.0f);

	/* The attribute pointers stay valid for the whole run. */
	glUseProgram(prog);
	glUniform1i(u_tex, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tex);
	glBindBuffer(GL_ARRAY_BUFFER, vbo[0]);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, (void *)0);
	glBindBuffer(GL_ARRAY_BUFFER, vbo[1]);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);
	glBindBuffer(GL_ARRAY_BUFFER, vbo[2]);
	glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 0, (void *)0);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);
	glFinish();
	t_setup = now();

	/* ---- one frame, the way the loop draws it ---- */
	{
		float proj[16], view[16], rx[16], ry[16], tmp[16], mvp[16];

		mat_perspective(proj, 55.0f, (float)width / height, 0.5f, 80.0f);
		mat_translate(view, 0, 0, -(float)grid * 1.6f - 1.0f);
		mat_rotate_x(rx, 0.35f);
		mat_rotate_y(ry, 0.55f);
		mat_mul(tmp, view, ry);
		mat_mul(tmp, tmp, rx);
		mat_mul(mvp, proj, tmp);
		glUniformMatrix4fv(u_mvp, 1, GL_FALSE, mvp);
	}
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLES, 0, nverts);
	glFinish();
	t_ready = now();
	if (glGetError() != GL_NO_ERROR) {
		fprintf(stderr, "glcubes: GL error after the first frame\n");
		goto out;
	}
	nonclear = coverage(width, height, &sampled);

	for (unsigned f = 0; f < warmup; f++) {
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLES, 0, nverts);
		glFinish();
	}

	printf("glcubes: %ux%ux%u cubes (%u vertices), %ux%u\n", grid, grid, grid,
	       nverts, width, height);
	printf("load: %.1f ms (context %.1f, assets %.1f, first render %.1f)\n",
	       (t_ready - t0) * 1000, (t_egl - t0) * 1000,
	       (t_setup - t_egl) * 1000, (t_ready - t_setup) * 1000);
	printf("coverage: %u of %u sampled pixels not the clear colour\n",
	       nonclear, sampled);

	if (frames) {
		t_start = now();
		for (unsigned f = 0; f < frames; f++) {
			double a = now();
			float proj[16], view[16], rx[16], ry[16], tmp[16], mvp[16];
			double dt;

			mat_perspective(proj, 55.0f, (float)width / height, 0.5f, 80.0f);
			mat_translate(view, 0, 0, -(float)grid * 1.6f - 1.0f);
			mat_rotate_x(rx, 0.35f + 0.20f * sinf(f * 0.013f));
			mat_rotate_y(ry, f * 0.017f);
			mat_mul(tmp, view, ry);
			mat_mul(tmp, tmp, rx);
			mat_mul(mvp, proj, tmp);
			glUniformMatrix4fv(u_mvp, 1, GL_FALSE, mvp);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
			glDrawArrays(GL_TRIANGLES, 0, nverts);
			glFinish();
			dt = (now() - a) * 1000;
			sum_ms += dt;
			if (dt < min_ms)
				min_ms = dt;
			if (dt > max_ms)
				max_ms = dt;
		}
		t_end = now();
		printf("frames: %u in %.3f s\n", frames, t_end - t_start);
		printf("fps: %.2f\n", frames / (t_end - t_start));
		printf("frame: avg %.2f ms, min %.2f, max %.2f\n",
		       sum_ms / frames, min_ms, max_ms);
		printf("glcubes_result cubes=%u vertices=%u width=%u height=%u "
		       "load_ms=%.1f setup_ms=%.1f first_ms=%.1f frames=%u "
		       "seconds=%.3f fps=%.2f avg_ms=%.2f min_ms=%.2f max_ms=%.2f\n",
		       grid * grid * grid, nverts, width, height,
		       (t_ready - t0) * 1000, (t_setup - t_egl) * 1000,
		       (t_ready - t_setup) * 1000, frames, t_end - t_start,
		       frames / (t_end - t_start), sum_ms / frames, min_ms, max_ms);
	} else {
		printf("glcubes_result cubes=%u vertices=%u width=%u height=%u "
		       "load_ms=%.1f setup_ms=%.1f first_ms=%.1f frames=0\n",
		       grid * grid * grid, nverts, width, height,
		       (t_ready - t0) * 1000, (t_setup - t_egl) * 1000,
		       (t_ready - t_setup) * 1000);
	}
	ret = 0;

out:
	free(pos);
	free(uv);
	free(shade);
	glDeleteBuffers(3, vbo);
	glDeleteTextures(1, &tex);
	glDeleteFramebuffers(1, &fbo);
	glDeleteRenderbuffers(1, &depth);
	glDeleteTextures(1, &color);
	glDeleteProgram(prog);
	if (ctx != EGL_NO_CONTEXT) {
		eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		eglDestroyContext(dpy, ctx);
	}
	eglTerminate(dpy);
	return ret;
}
