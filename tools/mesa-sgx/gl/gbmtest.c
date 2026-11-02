// SPDX-License-Identifier: GPL-2.0-only
/* gbmtest -- OpenGL ES 2.0 on a GBM surface, through kmsro, on the HDMI
 * display.
 *
 *   sgx-gl gbmtest [--finish] [--quads N] [frames] [/dev/dri/card1]
 *
 * The screen is the display device (s5l8947x); GBM opens it, EGL picks the
 * kmsro screen it pairs with the SGX render node, and the surface's buffers
 * are SGX render targets the display scans out.  Each frame is cleared and
 * gets a triangle and is then flipped onto the display with
 * DRM_MODE_PAGE_FLIP_EVENT.  The display's atomic commit waits on the render
 * node's fence through the dma-buf, so the client does not call glFinish
 * (--finish does, for comparison); --quads N makes a frame heavy enough for
 * the wait to show in the "frame 0" timing.  The console's framebuffer is put
 * back on exit.
 *
 * This is the EGL/GBM half of the route to OpenGL ES 2.0: no read-back and
 * no /dev/fb0 write in the loop, and no mode set of its own -- the mode the
 * console is already in is kept, so a failure leaves the display alone.
 *
 * Exit status 0 when every check passed.
 */
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define MAX_FRAMES 100000

static int flip_done;

static double
now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void
flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec,
	     void *data)
{
	(void)fd;
	(void)frame;
	(void)sec;
	(void)usec;
	flip_done = 1;
	*(int *)data = 1;
}

static GLuint
program(const char *vs_src, const char *fs_src)
{
	const char *src[2] = { vs_src, fs_src };
	GLenum type[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
	GLuint p = glCreateProgram();
	char log[1024];
	GLint ok;

	for (int i = 0; i < 2; i++) {
		GLuint s = glCreateShader(type[i]);

		glShaderSource(s, 1, &src[i], NULL);
		glCompileShader(s);
		glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
		if (!ok) {
			glGetShaderInfoLog(s, sizeof(log), NULL, log);
			fprintf(stderr, "gbmtest: shader: %s\n", log);
			exit(1);
		}
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "p");
	glBindAttribLocation(p, 1, "c");
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		glGetProgramInfoLog(p, sizeof(log), NULL, log);
		fprintf(stderr, "gbmtest: link: %s\n", log);
		exit(1);
	}
	return p;
}

int
main(int argc, char **argv)
{
	static const char *vs_src =
		"attribute vec2 p;\n"
		"attribute vec3 c;\n"
		"varying vec3 v;\n"
		"void main() { v = c; gl_Position = vec4(p, 0.0, 1.0); }\n";
	static const char *fs_src =
		"precision mediump float;\n"
		"varying vec3 v;\n"
		"void main() { gl_FragColor = vec4(v, 1.0); }\n";
	static const float verts[] = {
		/* x, y, r, g, b -- a triangle pointing up, one colour a corner */
		-0.8f, -0.8f, 1.0f, 0.0f, 0.0f,
		 0.8f, -0.8f, 0.0f, 1.0f, 0.0f,
		 0.0f,  0.8f, 0.0f, 0.0f, 1.0f,
	};
	static const float quad[] = {
		/* a full-screen strip, for --quads N: N times the fill */
		-1.0f, -1.0f, 0.2f, 0.4f, 0.8f,
		 1.0f, -1.0f, 0.2f, 0.4f, 0.8f,
		-1.0f,  1.0f, 0.2f, 0.4f, 0.8f,
		 1.0f,  1.0f, 0.2f, 0.4f, 0.8f,
	};
	static drmEventContext ev = {
		.version = 2,
		.page_flip_handler = flip_handler,
	};
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform;
	EGLint cfg_attr[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_NONE,
	};
	EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	drmModeRes *res = NULL;
	drmModeConnector *conn = NULL;
	drmModeCrtc *old_crtc = NULL;
	drmModeModeInfo mode;
	uint32_t conn_id = 0, crtc_id = 0;
	struct gbm_device *gbm = NULL;
	struct gbm_surface *gsurf = NULL;
	struct gbm_bo *bo = NULL, *prev = NULL;
	uint32_t prev_fb = 0, fb = 0;
	EGLDisplay dpy = EGL_NO_DISPLAY;
	EGLConfig cfg;
	EGLContext ctx = EGL_NO_CONTEXT;
	EGLSurface esurf = EGL_NO_SURFACE;
	GLuint prog, vbo, qvbo;
	const char *card = "/dev/dri/card1";
	int fd = -1, frames = 120, n, got, ret = 1, i;
	/* (the display waits on the render node's fence through the dma-buf:
	 * glFinish is not needed, and --finish is the comparison) */
	int no_finish = 1, quads = 0, have_frames = 0;

	setvbuf(stdout, NULL, _IONBF, 0);

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--no-finish")) {
			no_finish = 1;
		} else if (!strcmp(argv[i], "--finish")) {
			no_finish = 0;
		} else if (!strcmp(argv[i], "--quads") && i + 1 < argc) {
			quads = atoi(argv[++i]);
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "usage: gbmtest [--finish] [--quads N] [frames] [card]\n");
			return 2;
		} else if (!have_frames) {
			frames = atoi(argv[i]) > 0 ? atoi(argv[i]) : 120;
			have_frames = 1;
		} else {
			card = argv[i];
		}
	}
	if (frames > MAX_FRAMES)
		frames = MAX_FRAMES;

	fd = open(card, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "gbmtest: %s: %s\n", card, strerror(errno));
		return 1;
	}
	/* the first open is master; a page flip needs it */
	if (drmSetMaster(fd) < 0 && errno != EACCES)
		fprintf(stderr, "gbmtest: drmSetMaster: %s\n", strerror(errno));

	res = drmModeGetResources(fd);
	if (!res) {
		fprintf(stderr, "gbmtest: %s is not a KMS device\n", card);
		goto out;
	}
	for (i = 0; i < res->count_connectors; i++) {
		conn = drmModeGetConnector(fd, res->connectors[i]);
		if (conn && conn->connection == DRM_MODE_CONNECTED &&
		    conn->count_modes > 0) {
			conn_id = conn->connector_id;
			break;
		}
		if (conn)
			drmModeFreeConnector(conn);
		conn = NULL;
	}
	if (!conn) {
		fprintf(stderr, "gbmtest: no connected connector\n");
		goto out;
	}
	if (!res->count_crtcs) {
		fprintf(stderr, "gbmtest: no CRTC\n");
		goto out;
	}
	crtc_id = res->crtcs[0];
	old_crtc = drmModeGetCrtc(fd, crtc_id);
	if (!old_crtc || !old_crtc->mode_valid) {
		fprintf(stderr, "gbmtest: the console has no mode; set one first\n");
		goto out;
	}
	mode = old_crtc->mode;

	gbm = gbm_create_device(fd);
	if (!gbm) {
		fprintf(stderr, "gbmtest: gbm_create_device: %s\n", strerror(errno));
		goto out;
	}
	gsurf = gbm_surface_create(gbm, mode.hdisplay, mode.vdisplay,
				   GBM_FORMAT_XRGB8888,
				   GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	if (!gsurf) {
		fprintf(stderr, "gbmtest: gbm_surface_create: %s\n", strerror(errno));
		goto out;
	}

	get_platform = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
		eglGetProcAddress("eglGetPlatformDisplayEXT");
	if (!get_platform) {
		fprintf(stderr, "gbmtest: no eglGetPlatformDisplayEXT\n");
		goto out;
	}
	dpy = get_platform(EGL_PLATFORM_GBM_KHR, gbm, NULL);
	if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) {
		fprintf(stderr, "gbmtest: no EGL display on the GBM device (0x%x)\n",
			eglGetError());
		dpy = EGL_NO_DISPLAY;
		goto out;
	}
	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		fprintf(stderr, "gbmtest: no OpenGL ES\n");
		goto out;
	}
	if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &got) || got != 1) {
		fprintf(stderr, "gbmtest: no EGL config (%d)\n", got);
		goto out;
	}
	esurf = eglCreateWindowSurface(dpy, cfg, gsurf, NULL);
	if (esurf == EGL_NO_SURFACE) {
		fprintf(stderr, "gbmtest: eglCreateWindowSurface (0x%x)\n", eglGetError());
		goto out;
	}
	ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
	if (ctx == EGL_NO_CONTEXT ||
	    !eglMakeCurrent(dpy, esurf, esurf, ctx)) {
		fprintf(stderr, "gbmtest: no GLES 2 context (0x%x)\n", eglGetError());
		goto out;
	}

	printf("GL_RENDERER %s\nGL_VERSION  %s\nEGL %dx%d on %s\n",
	       glGetString(GL_RENDERER), glGetString(GL_VERSION),
	       mode.hdisplay, mode.vdisplay, card);

	prog = program(vs_src, fs_src);
	glUseProgram(prog);
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
	glGenBuffers(1, &qvbo);
	glBindBuffer(GL_ARRAY_BUFFER, qvbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
			      (void *)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
			      (void *)(2 * sizeof(float)));
	glViewport(0, 0, mode.hdisplay, mode.vdisplay);

	for (i = 0; i < frames; i++) {
		int32_t px[4];
		float f = (i % 60) / 60.0f;
		double t0, t1, t_finish = 0, t_swap, t_flip, t_event;

		/* a colour that moves, then the triangle over it: at frame 0
		 * the clear is orange, so a blue pixel proves the draw */
		glClearColor(1.0f - f, 0.2f, f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		if (quads) {
			glBindBuffer(GL_ARRAY_BUFFER, qvbo);
			glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
					      5 * sizeof(float), (void *)0);
			glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE,
					      5 * sizeof(float),
					      (void *)(2 * sizeof(float)));
			for (int q = 0; q < quads; q++)
				glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		} else {
			glBindBuffer(GL_ARRAY_BUFFER, vbo);
			glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
					      5 * sizeof(float), (void *)0);
			glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE,
					      5 * sizeof(float),
					      (void *)(2 * sizeof(float)));
			glDrawArrays(GL_TRIANGLES, 0, 3);
		}

		t0 = now();
		if (!no_finish)
			glFinish();
		t1 = now();
		t_finish = t1 - t0;

		if (i == 0 && !quads && !no_finish) {
			/* near the triangle's top vertex, which is blue */
			int r, g, b;

			glReadPixels(mode.hdisplay / 2, mode.vdisplay * 85 / 100,
				     1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
			r = px[0] & 0xff;
			g = (px[0] >> 8) & 0xff;
			b = (px[0] >> 16) & 0xff;
			if (r > 40 || g > 40 || b < 200) {
				fprintf(stderr, "gbmtest: the triangle pixel is %02x%02x%02x, want blue\n",
					r, g, b);
				goto out;
			}
			printf("render into the scanout buffer: %02x%02x%02x near the triangle's top\n",
			       r, g, b);
		}

		t0 = now();
		if (!eglSwapBuffers(dpy, esurf)) {
			fprintf(stderr, "gbmtest: eglSwapBuffers (0x%x)\n", eglGetError());
			goto out;
		}
		t1 = now();
		t_swap = t1 - t0;
		bo = gbm_surface_lock_front_buffer(gsurf);
		if (!bo) {
			fprintf(stderr, "gbmtest: lock_front_buffer: %s\n", strerror(errno));
			goto out;
		}
		if (drmModeAddFB(fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo),
				 24, 32, gbm_bo_get_stride(bo),
				 gbm_bo_get_handle(bo).u32, &fb)) {
			fprintf(stderr, "gbmtest: drmModeAddFB: %s\n", strerror(errno));
			goto out;
		}
		flip_done = 0;
		t0 = now();
		if (drmModePageFlip(fd, crtc_id, fb, DRM_MODE_PAGE_FLIP_EVENT,
				    &flip_done)) {
			fprintf(stderr, "gbmtest: drmModePageFlip: %s\n", strerror(errno));
			drmModeRmFB(fd, fb);
			fb = 0;
			goto out;
		}
		t1 = now();
		t_flip = t1 - t0;
		while (!flip_done) {
			fd_set fds;
			struct timeval tv = { 2, 0 };

			FD_ZERO(&fds);
			FD_SET(fd, &fds);
			n = select(fd + 1, &fds, NULL, NULL, &tv);
			if (n <= 0) {
				fprintf(stderr, "gbmtest: the flip did not complete\n");
				goto out;
			}
			drmHandleEvent(fd, &ev);
		}
		t1 = now();
		t_event = t1 - t0;
		if (i == 0)
			printf("frame 0 (%s, %d quads): glFinish %.1f ms, swap %.1f ms, "
			       "flip ioctl %.1f ms, flip done %.1f ms\n",
			       no_finish ? "no glFinish" : "glFinish", quads,
			       t_finish * 1e3, t_swap * 1e3, t_flip * 1e3,
			       t_event * 1e3);
		if (i == 0)
			printf("first frame flipped onto the display\n");
		if (prev) {
			if (prev_fb)
				drmModeRmFB(fd, prev_fb);
			gbm_surface_release_buffer(gsurf, prev);
		}
		prev = bo;
		prev_fb = fb;
		bo = NULL;
		fb = 0;
	}
	printf("%d frames on the display\n", frames);
	ret = 0;

out:
	if (ctx != EGL_NO_CONTEXT)
		eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if (esurf != EGL_NO_SURFACE)
		eglDestroySurface(dpy, esurf);
	if (ctx != EGL_NO_CONTEXT)
		eglDestroyContext(dpy, ctx);
	if (dpy != EGL_NO_DISPLAY)
		eglTerminate(dpy);
	/* the console's buffer back on the pipe before the last flip's buffer
	 * is released (the display still points at it) */
	if (old_crtc) {
		if (old_crtc->mode_valid)
			drmModeSetCrtc(fd, crtc_id, old_crtc->buffer_id,
				       0, 0, &conn_id, 1, &old_crtc->mode);
		drmModeFreeCrtc(old_crtc);
	}
	if (prev) {
		if (prev_fb)
			drmModeRmFB(fd, prev_fb);
		gbm_surface_release_buffer(gsurf, prev);
	}
	if (gsurf)
		gbm_surface_destroy(gsurf);
	if (gbm)
		gbm_device_destroy(gbm);
	if (conn)
		drmModeFreeConnector(conn);
	if (res)
		drmModeFreeResources(res);
	if (fd >= 0) {
		drmDropMaster(fd);
		close(fd);
	}
	return ret;
}
