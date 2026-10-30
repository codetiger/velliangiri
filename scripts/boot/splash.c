// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X pre-kernel boot splash rasterizer
 *
 * Fills each shape with nonzero winding, anti-aliased with 16 sample rows
 * per pixel and 1/16 pixel horizontal coverage, and blends it over the
 * framebuffer with a solid or two-stop linear gradient paint. Partially
 * covered pixels are blended in linear light so edges keep their weight.
 */

#include <stdint.h>
#include "splash.h"
#include "splash-data.h"

#define SUB		16
#define SAMPLES		16
#define MAX_CROSS	64
#define MAX_W		1280

struct crossing {
	int32_t x;
	int32_t dir;
};

static uint16_t cov[MAX_W];

static uint32_t paint_at(const struct splash_shape *s, int x, int y)
{
	int32_t t;
	uint32_t c = 0;
	int sh;

	if (s->c0 == s->c1)
		return s->c0;
	t = s->ga * x + s->gb * y + s->gc;
	if (t < 0)
		t = 0;
	if (t > 65536)
		t = 65536;
	for (sh = 0; sh < 24; sh += 8) {
		int32_t a = (s->c0 >> sh) & 0xff;
		int32_t b = (s->c1 >> sh) & 0xff;

		c |= (uint32_t)(a + (((b - a) * t) >> 16)) << sh;
	}
	return c;
}

static uint32_t blend(uint32_t dst, uint32_t src, uint32_t a)
{
	uint32_t c = 0;
	int sh;

	if (a >= 256)
		return src;
	for (sh = 0; sh < 24; sh += 8) {
		uint32_t d = splash_to_linear[(dst >> sh) & 0xff];
		uint32_t s = splash_to_linear[(src >> sh) & 0xff];

		c |= (uint32_t)splash_to_srgb[(d * (256 - a) + s * a) >> 14] << sh;
	}
	return c;
}

static void add_span(int32_t xa, int32_t xb, int lo, int hi)
{
	int pa, pb, p;

	if (xa < lo * SUB)
		xa = lo * SUB;
	if (xb > (hi + 1) * SUB)
		xb = (hi + 1) * SUB;
	if (xa >= xb)
		return;
	pa = xa / SUB;
	pb = xb / SUB;
	if (pa == pb) {
		cov[pa] += xb - xa;
		return;
	}
	cov[pa] += SUB - (xa & (SUB - 1));
	for (p = pa + 1; p < pb; p++)
		cov[p] += SUB;
	if (pb <= hi)
		cov[pb] += xb & (SUB - 1);
}

static void scan_row(const struct splash_shape *s, const struct splash_vtx *v,
		     int32_t sy)
{
	struct crossing cr[MAX_CROSS];
	int n = 0, c, k, i, wind = 0;

	for (c = 0; c < s->ncontours; c++) {
		int len = splash_contour_len[s->first_contour + c];

		for (k = 0; k < len; k++) {
			const struct splash_vtx *a = &v[k];
			const struct splash_vtx *b = &v[k + 1 < len ? k + 1 : 0];
			int32_t ylo = a->y < b->y ? a->y : b->y;
			int32_t yhi = a->y < b->y ? b->y : a->y;

			if (sy < ylo || sy >= yhi || n == MAX_CROSS)
				continue;
			cr[n].x = a->x + (int32_t)(((int64_t)(sy - a->y) * a->slope) >> 16);
			cr[n].dir = b->y > a->y ? 1 : -1;
			for (i = n++; i > 0 && cr[i - 1].x > cr[i].x; i--) {
				struct crossing t = cr[i];

				cr[i] = cr[i - 1];
				cr[i - 1] = t;
			}
		}
		v += len;
	}

	for (i = 0; i + 1 < n; i++) {
		wind += cr[i].dir;
		if (wind)
			add_span(cr[i].x, cr[i + 1].x, s->x0, s->x1);
	}
}

static void draw_shape(volatile uint32_t *fb, int stride, const struct splash_shape *s)
{
	const struct splash_vtx *v = splash_vtx;
	int x, y, k;

	for (k = 0; k < s->first_contour; k++)
		v += splash_contour_len[k];

	for (y = s->y0; y <= s->y1; y++) {
		volatile uint32_t *row = fb + y * stride;

		for (k = 0; k < SAMPLES; k++)
			scan_row(s, v, y * SUB + k * (SUB / SAMPLES) + SUB / SAMPLES / 2);
		for (x = s->x0; x <= s->x1; x++) {
			uint32_t a = cov[x];

			/* clear as we go; an explicit loop could become memset() */
			cov[x] = 0;
			if (!a)
				continue;
			/* cov peaks at SUB * SAMPLES = 256 */
			a = (a * s->opacity) >> 8;
			row[x] = blend(row[x], paint_at(s, x, y), a);
		}
	}
}

void splash_draw(volatile uint32_t *fb, int width, int height, int stride)
{
	int x, y, i;

	for (y = 0; y < height; y++)
		for (x = 0; x < width; x++)
			fb[y * stride + x] = SPLASH_BG;
	for (i = 0; i < SPLASH_NSHAPES; i++)
		draw_shape(fb, stride, &splash_shapes[i]);
}
