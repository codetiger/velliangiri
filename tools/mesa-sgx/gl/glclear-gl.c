// SPDX-License-Identifier: GPL-2.0-only
/* glclear-gl -- the desktop-GL half of the driver, checked the same way as
 * glclear: OpenGL (not OpenGL ES) through surfaceless EGL, a framebuffer
 * object, glClear, and the pixels read back.
 *
 *   sgx-gl glclear-gl [N]
 *
 * Only the desktop build (MESA_BUILD=desktop) has OpenGL in its Mesa, so
 * this program is built and installed with it.  It exists because GLX needs
 * an X server, which this image does not have: the same Gallium screen and
 * the same driver are exercised through EGL's OpenGL API instead.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 768
#define H 1024

static int
fail(const char *what)
{
	fprintf(stderr, "glclear-gl: %s (EGL 0x%x, GL 0x%x)\n", what,
		eglGetError(), glGetError());
	return 1;
}

int
main(int argc, char **argv)
{
	static const float colours[][4] = {
		{ 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 }, { 1, 1, 1, 1 },
		{ 0.2f, 0.4f, 0.6f, 1 }, { 0, 0, 0, 0 },
	};
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLDisplay dpy;
	EGLContext ctx;
	GLuint tex, fbo;
	uint8_t *px;
	int n = 1, bad_frames = 0, i;

	if (argc > 1)
		n = atoi(argv[1]) > 0 ? atoi(argv[1]) : 1;

	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
		eglGetProcAddress("eglGetPlatformDisplayEXT");
	if (!get_display)
		return fail("no eglGetPlatformDisplayEXT");
	dpy = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
	if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL))
		return fail("no surfaceless EGL display");
	if (!eglBindAPI(EGL_OPENGL_API))
		return fail("no desktop OpenGL");
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT ||
	    !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fail("no OpenGL 2 context");
	printf("GL_RENDERER %s\nGL_VERSION  %s\nGL_VENDOR   %s\n",
	       glGetString(GL_RENDERER), glGetString(GL_VERSION),
	       glGetString(GL_VENDOR));

	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA,
		     GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			       GL_TEXTURE_2D, tex, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		return fail("the framebuffer object is not complete");

	px = malloc(W * H * 4);
	if (!px)
		return fail("out of memory");
	for (i = 0; i < n; i++) {
		const float *c = colours[i % (sizeof(colours) / sizeof(colours[0]))];
		uint8_t want[4];
		int bad = 0, k;

		glClearColor(c[0], c[1], c[2], c[3]);
		glClear(GL_COLOR_BUFFER_BIT);
		glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
		for (k = 0; k < 4; k++)
			want[k] = (uint8_t)(c[k] * 255.0f + 0.5f);
		for (k = 0; k < W * H; k++) {
			int ch;

			for (ch = 0; ch < 4; ch++) {
				int d = px[k * 4 + ch] - want[ch];

				if (d < -1 || d > 1)
					break;
			}
			if (ch < 4 && bad++ < 3)
				fprintf(stderr, "  frame %d pixel %d,%d: %02x %02x %02x %02x, want %02x %02x %02x %02x\n",
					i, k % W, k / W, px[k * 4], px[k * 4 + 1],
					px[k * 4 + 2], px[k * 4 + 3], want[0],
					want[1], want[2], want[3]);
		}
		if (bad) {
			fprintf(stderr, "  frame %d: %d of %d pixels wrong\n", i,
				bad, W * H);
			bad_frames++;
		}
	}
	printf("%d desktop-GL clear%s: %d wrong\n", n, n == 1 ? "" : "s",
	       bad_frames);

	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	free(px);
	return bad_frames ? 1 : 0;
}
