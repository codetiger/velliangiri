/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * S5L8947X pre-kernel boot splash
 *
 * Shape tables are generated from logo.svg by scripts/boot/gen-splash.py.
 * Vertex coordinates are in 1/16 pixel units; each vertex also carries the
 * 16.16 dx/dy slope of the edge to the next vertex of its contour, so the
 * rasterizer never divides.
 */

#ifndef VELLIANGIRI_SPLASH_H
#define VELLIANGIRI_SPLASH_H

#include <stdint.h>

struct splash_vtx {
	int16_t x, y;
	int32_t slope;
};

struct splash_shape {
	uint16_t first_contour, ncontours;
	int16_t x0, y0, x1, y1;		/* pixel bounding box, inclusive */
	uint16_t opacity;		/* 0..256 */
	int32_t ga, gb, gc;		/* gradient t = ga * x + gb * y + gc, 16.16 */
	uint32_t c0, c1;		/* 0xRRGGBB at t = 0 and t = 1 */
};

void splash_draw(volatile uint32_t *fb, int width, int height, int stride);

#endif /* VELLIANGIRI_SPLASH_H */
