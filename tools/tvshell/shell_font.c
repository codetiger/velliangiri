// SPDX-License-Identifier: GPL-2.0-only
/*
 * Giri on Velliangiri OS — TrueType (.ttf) & Vector Fallback Typography Engine
 *
 * Parses TrueType outlines (head, hhea, maxp, cmap, hmtx, loca, glyf) from
 * /usr/share/fonts/tvshell/{PlusJakartaSans,JetBrainsMono}-*.ttf and caches
 * anti-aliased glyph bitmaps for the v1.2 design system type scale.
 * Includes built-in geometric vector glyphs for UI symbols (↵, —, ✓, ⬇, ·)
 * and ASCII fallback so text remains crisp even without external font files.
 */

#include <ctype.h>
#include <fcntl.h>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tvshell.h"
#include "shell_assets.h"

#define SUB		32
#define SAMPLES		4

struct ttf_font {
	uint8_t *data;
	size_t size;
	uint16_t units_per_em;
	int16_t ascent;
	int16_t descent;
	uint16_t num_glyphs;
	uint16_t num_hmetrics;
	int16_t loca_format;
	uint32_t cmap_off;
	uint32_t fmt4_off;
	uint32_t fmt12_off;
	uint32_t hmtx_off;
	uint32_t loca_off;
	uint32_t glyf_off;
};

struct glyph_entry {
	uint32_t codepoint;
	uint16_t size_px;
	uint8_t role;
	uint8_t phase;
	int16_t advance;
	int16_t advance_q6;
	int16_t xoff;
	int16_t yoff;
	uint16_t w;
	uint16_t h;
	uint8_t *bitmap;
	struct glyph_entry *next;
};

#define GLYPH_HASH_BUCKETS	1024

static struct ttf_font fonts[SHELL_FONT_ROLE_COUNT];
static struct glyph_entry *glyph_cache[GLYPH_HASH_BUCKETS];

static const char * const font_filenames[SHELL_FONT_ROLE_COUNT] = {
	[SHELL_FONT_SANS_MEDIUM]   = "PlusJakartaSans-Medium.ttf",
	[SHELL_FONT_SANS_SEMIBOLD] = "PlusJakartaSans-SemiBold.ttf",
	[SHELL_FONT_SANS_BOLD]     = "PlusJakartaSans-Bold.ttf",
	[SHELL_FONT_MONO_MEDIUM]   = "JetBrainsMono-Medium.ttf",
	[SHELL_FONT_MONO_SEMIBOLD] = "JetBrainsMono-SemiBold.ttf",
};

static inline uint16_t be16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static inline int16_t sbe16(const uint8_t *p)
{
	return (int16_t)be16(p);
}

static inline uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int ttf_load_file(struct ttf_font *f, const char *path)
{
	struct stat st;
	int fd;
	uint16_t num_tables, i;

	memset(f, 0, sizeof(*f));
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	if (fstat(fd, &st) < 0 || st.st_size < 64) {
		close(fd);
		return -1;
	}
	f->size = (size_t)st.st_size;
	f->data = mmap(NULL, f->size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (f->data == MAP_FAILED) {
		f->data = NULL;
		f->size = 0;
		return -1;
	}

	num_tables = be16(f->data + 4);
	if (12 + (size_t)num_tables * 16 > f->size)
		goto bad;

	uint32_t head_off = 0, hhea_off = 0, maxp_off = 0;

	for (i = 0; i < num_tables; i++) {
		const uint8_t *rec = f->data + 12 + i * 16;
		uint32_t off = be32(rec + 8);
		uint32_t len = be32(rec + 12);

		if ((size_t)off + len > f->size)
			continue;
		if (!memcmp(rec, "head", 4))
			head_off = off;
		else if (!memcmp(rec, "hhea", 4))
			hhea_off = off;
		else if (!memcmp(rec, "maxp", 4))
			maxp_off = off;
		else if (!memcmp(rec, "cmap", 4))
			f->cmap_off = off;
		else if (!memcmp(rec, "hmtx", 4))
			f->hmtx_off = off;
		else if (!memcmp(rec, "loca", 4))
			f->loca_off = off;
		else if (!memcmp(rec, "glyf", 4))
			f->glyf_off = off;
	}

	if (!head_off || !hhea_off || !maxp_off || !f->cmap_off ||
	    !f->hmtx_off || !f->loca_off || !f->glyf_off)
		goto bad;

	f->units_per_em = be16(f->data + head_off + 18);
	f->loca_format = sbe16(f->data + head_off + 50);
	f->ascent = sbe16(f->data + hhea_off + 4);
	f->descent = sbe16(f->data + hhea_off + 6);
	f->num_hmetrics = be16(f->data + hhea_off + 34);
	f->num_glyphs = be16(f->data + maxp_off + 4);
	if (!f->units_per_em || !f->num_glyphs)
		goto bad;

	{
		const uint8_t *cmap = f->data + f->cmap_off;
		uint16_t ntables = (f->cmap_off + 4 <= f->size) ? be16(cmap + 2) : 0;

		for (i = 0; i < ntables && f->cmap_off + 4 + (size_t)(i + 1) * 8 <= f->size; i++) {
			const uint8_t *rec = cmap + 4 + i * 8;
			uint16_t pid = be16(rec);
			uint16_t eid = be16(rec + 2);
			uint32_t sub_off = f->cmap_off + be32(rec + 4);

			if (sub_off + 8 > f->size)
				continue;
			if ((pid == 3 && (eid == 1 || eid == 10)) || pid == 0) {
				uint16_t fmt = be16(f->data + sub_off);

				if (fmt == 12)
					f->fmt12_off = sub_off;
				else if (fmt == 4 && !f->fmt4_off)
					f->fmt4_off = sub_off;
			}
		}
	}
	return 0;

bad:
	if (f->data)
		munmap(f->data, f->size);
	memset(f, 0, sizeof(*f));
	return -1;
}

static uint16_t ttf_find_glyph(const struct ttf_font *f, uint32_t cp)
{
	uint32_t fmt4_off = f->fmt4_off;
	uint32_t fmt12_off = f->fmt12_off;

	if (fmt12_off && fmt12_off + 16 <= f->size) {
		const uint8_t *sub = f->data + fmt12_off;
		uint32_t ngroups = be32(sub + 12);
		uint32_t lo = 0, hi = ngroups;

		if (fmt12_off + 16 + (size_t)ngroups * 12 <= f->size) {
			while (lo < hi) {
				uint32_t mid = lo + ((hi - lo) >> 1);
				const uint8_t *grp = sub + 16 + mid * 12;
				uint32_t end = be32(grp + 4);

				if (cp > end) {
					lo = mid + 1;
				} else {
					uint32_t start = be32(grp);

					if (cp >= start)
						return (uint16_t)(be32(grp + 8) + (cp - start));
					hi = mid;
				}
			}
		}
	}

	if (fmt4_off && cp <= 0xffffU && fmt4_off + 14 <= f->size) {
		const uint8_t *sub = f->data + fmt4_off;
		uint16_t seg_count = be16(sub + 6) / 2;
		const uint8_t *end_codes = sub + 14;
		const uint8_t *start_codes = end_codes + seg_count * 2 + 2;
		const uint8_t *id_deltas = start_codes + seg_count * 2;
		const uint8_t *id_range_offs = id_deltas + seg_count * 2;
		uint16_t lo = 0, hi = seg_count;

		if ((size_t)(id_range_offs + seg_count * 2 - f->data) <= f->size) {
			while (lo < hi) {
				uint16_t mid = (uint16_t)(lo + ((hi - lo) >> 1));
				uint16_t end_c = be16(end_codes + mid * 2);

				if (cp > end_c)
					lo = (uint16_t)(mid + 1);
				else
					hi = mid;
			}
			if (lo < seg_count) {
				uint16_t start_c = be16(start_codes + lo * 2);

				if (cp < start_c)
					return 0;
				int16_t delta = sbe16(id_deltas + lo * 2);
				uint16_t roff = be16(id_range_offs + lo * 2);

				if (roff == 0)
					return (uint16_t)((cp + delta) & 0xffffU);
				const uint8_t *gptr = id_range_offs + lo * 2 + roff +
						      (cp - start_c) * 2;
				if ((size_t)(gptr - f->data) + 2 > f->size)
					return 0;
				uint16_t glyph_id = be16(gptr);

				if (glyph_id == 0)
					return 0;
				return (uint16_t)((glyph_id + delta) & 0xffffU);
			}
		}
	}
	return 0;
}

static void ttf_get_hmetrics(const struct ttf_font *f, uint16_t glyph_id,
			     uint16_t *adv, int16_t *lsb)
{
	if (glyph_id < f->num_hmetrics) {
		const uint8_t *p = f->data + f->hmtx_off + (size_t)glyph_id * 4;

		*adv = be16(p);
		*lsb = sbe16(p + 2);
	} else {
		const uint8_t *p = f->data + f->hmtx_off +
				   (size_t)(f->num_hmetrics - 1) * 4;
		const uint8_t *lp = f->data + f->hmtx_off +
				    (size_t)f->num_hmetrics * 4 +
				    (size_t)(glyph_id - f->num_hmetrics) * 2;
		*adv = be16(p);
		*lsb = ((size_t)(lp - f->data) + 2 <= f->size) ? sbe16(lp) : 0;
	}
}

static uint32_t ttf_glyph_offset(const struct ttf_font *f, uint16_t glyph_id, uint32_t *len)
{
	uint32_t off0, off1;

	if (glyph_id >= f->num_glyphs) {
		*len = 0;
		return 0;
	}
	if (f->loca_format == 0) {
		const uint8_t *p = f->data + f->loca_off + (size_t)glyph_id * 2;

		off0 = (uint32_t)be16(p) * 2;
		off1 = (uint32_t)be16(p + 2) * 2;
	} else {
		const uint8_t *p = f->data + f->loca_off + (size_t)glyph_id * 4;

		off0 = be32(p);
		off1 = be32(p + 4);
	}
	*len = (off1 > off0) ? (off1 - off0) : 0;
	return f->glyf_off + off0;
}

/* Polygon contour builder for scanline rasterization */
struct poly_pt {
	int32_t x, y; /* in 1/SUB pixel units */
};

struct poly_list {
	struct poly_pt *pts;
	int *contour_len;
	int npts, cap_pts;
	int ncontours, cap_contours;
};

static void poly_add_contour(struct poly_list *pl, const float *xs, const float *ys, int n)
{
	int i, added = 0;

	if (n < 3)
		return;
	if (pl->ncontours >= pl->cap_contours) {
		pl->cap_contours = pl->cap_contours ? pl->cap_contours * 2 : 32;
		pl->contour_len = realloc(pl->contour_len,
					  (size_t)pl->cap_contours * sizeof(int));
	}
	if (pl->npts + n >= pl->cap_pts) {
		pl->cap_pts = (pl->npts + n) * 2 + 64;
		pl->pts = realloc(pl->pts,
				  (size_t)pl->cap_pts * sizeof(struct poly_pt));
	}
	for (i = 0; i < n; i++) {
		int32_t qx = (int32_t)lrintf(xs[i] * SUB);
		int32_t qy = (int32_t)lrintf(ys[i] * SUB);

		if (added > 0 &&
		    pl->pts[pl->npts - 1].x == qx &&
		    pl->pts[pl->npts - 1].y == qy)
			continue;
		pl->pts[pl->npts].x = qx;
		pl->pts[pl->npts].y = qy;
		pl->npts++;
		added++;
	}
	if (added >= 3)
		pl->contour_len[pl->ncontours++] = added;
	else
		pl->npts -= added;
}

static void flatten_quad_to_buf(float x0, float y0, float x1, float y1,
				float x2, float y2, float *xs, float *ys,
				int *count, int max_pts)
{
	int steps = 5, i;

	for (i = 1; i <= steps && *count < max_pts; i++) {
		float t = (float)i / (float)steps;
		float u = 1.0f - t;

		xs[*count] = u * u * x0 + 2.0f * u * t * x1 + t * t * x2;
		ys[*count] = u * u * y0 + 2.0f * u * t * y1 + t * t * y2;
		(*count)++;
	}
}

static void ttf_extract_glyph_polys(const struct ttf_font *f, uint16_t glyph_id,
				    float scale, float a, float b, float c, float d,
				    float tx, float ty, struct poly_list *pl, int depth)
{
	uint32_t glen = 0;
	uint32_t goff = ttf_glyph_offset(f, glyph_id, &glen);

	if (glen < 10 || goff + glen > f->size || depth > 4)
		return;

	const uint8_t *g = f->data + goff;
	int16_t ncontours = sbe16(g);

	if (ncontours > 0) {
		const uint8_t *ends = g + 10;
		uint16_t npts = be16(ends + (ncontours - 1) * 2) + 1;
		uint16_t insn_len = be16(ends + ncontours * 2);
		const uint8_t *p = ends + ncontours * 2 + 2 + insn_len;
		const uint8_t *end_g = g + glen;

		if (npts == 0 || npts > 2048 || p >= end_g)
			return;

		uint8_t flags[2048];
		int16_t raw_x[2048];
		int16_t raw_y[2048];
		float cxs[2048];
		float cys[2048];

		uint16_t idx = 0;

		while (idx < npts && p < end_g) {
			uint8_t fl = *p++;

			flags[idx++] = fl;
			if (fl & 0x08) {
				uint8_t rep = (p < end_g) ? *p++ : 0;

				while (rep-- > 0 && idx < npts)
					flags[idx++] = fl;
			}
		}

		int16_t cur = 0;

		for (idx = 0; idx < npts; idx++) {
			uint8_t fl = flags[idx];

			if (fl & 0x02) {
				int16_t dx = (p < end_g) ? *p++ : 0;

				cur += (fl & 0x10) ? dx : -dx;
			} else if (!(fl & 0x10)) {
				if (p + 1 < end_g) {
					cur += sbe16(p);
					p += 2;
				}
			}
			raw_x[idx] = cur;
		}

		cur = 0;
		for (idx = 0; idx < npts; idx++) {
			uint8_t fl = flags[idx];

			if (fl & 0x04) {
				int16_t dy = (p < end_g) ? *p++ : 0;

				cur += (fl & 0x20) ? dy : -dy;
			} else if (!(fl & 0x20)) {
				if (p + 1 < end_g) {
					cur += sbe16(p);
					p += 2;
				}
			}
			raw_y[idx] = cur;
		}

		uint16_t start_pt = 0;
		int c_idx;

		for (c_idx = 0; c_idx < ncontours; c_idx++) {
			uint16_t end_pt = be16(ends + c_idx * 2);

			if (end_pt >= npts || end_pt < start_pt)
				break;
			int clen = end_pt - start_pt + 1;
			int out_n = 0;

			/* Transform helper from font units to pixel coords (y down) */
			#define TX(rx, ry) (((a * (rx) + c * (ry)) + tx) * scale)
			#define TY(rx, ry) (-((b * (rx) + d * (ry)) + ty) * scale)

			/* Find first on-curve point or midpoint of two off-curve points */
			float start_x, start_y;
			int k0 = 0;
			bool first_off = !(flags[start_pt] & 1);

			if (!first_off) {
				start_x = TX(raw_x[start_pt], raw_y[start_pt]);
				start_y = TY(raw_x[start_pt], raw_y[start_pt]);
				k0 = 1;
			} else {
				uint16_t last = end_pt;

				if (flags[last] & 1) {
					start_x = TX(raw_x[last], raw_y[last]);
					start_y = TY(raw_x[last], raw_y[last]);
				} else {
					float lx = TX(raw_x[last], raw_y[last]);
					float ly = TY(raw_x[last], raw_y[last]);
					float fx = TX(raw_x[start_pt], raw_y[start_pt]);
					float fy = TY(raw_x[start_pt], raw_y[start_pt]);

					start_x = 0.5f * (lx + fx);
					start_y = 0.5f * (ly + fy);
				}
				k0 = 0;
			}

			cxs[out_n] = start_x;
			cys[out_n] = start_y;
			out_n++;

			float cur_x = start_x, cur_y = start_y;
			bool have_ctrl = false;
			float ctrl_x = 0, ctrl_y = 0;
			int k;

			for (k = k0; k < clen; k++) {
				uint16_t pi = start_pt + k;
				float px = TX(raw_x[pi], raw_y[pi]);
				float py = TY(raw_x[pi], raw_y[pi]);
				bool on = (flags[pi] & 1) != 0;

				if (on) {
					if (have_ctrl) {
						flatten_quad_to_buf(cur_x, cur_y, ctrl_x, ctrl_y,
								    px, py, cxs, cys, &out_n, 2040);
						have_ctrl = false;
					} else if (out_n < 2040) {
						cxs[out_n] = px;
						cys[out_n] = py;
						out_n++;
					}
					cur_x = px;
					cur_y = py;
				} else {
					if (have_ctrl) {
						float mx = 0.5f * (ctrl_x + px);
						float my = 0.5f * (ctrl_y + py);

						flatten_quad_to_buf(cur_x, cur_y, ctrl_x, ctrl_y,
								    mx, my, cxs, cys, &out_n, 2040);
						cur_x = mx;
						cur_y = my;
					}
					ctrl_x = px;
					ctrl_y = py;
					have_ctrl = true;
				}
			}
			if (have_ctrl) {
				flatten_quad_to_buf(cur_x, cur_y, ctrl_x, ctrl_y,
						    start_x, start_y, cxs, cys, &out_n, 2040);
			}
			#undef TX
			#undef TY

			poly_add_contour(pl, cxs, cys, out_n);
			start_pt = end_pt + 1;
		}
	} else if (ncontours < 0) {
		/* Compound glyph */
		const uint8_t *p = g + 10;
		const uint8_t *end_g = g + glen;
		uint16_t comp_flags;

		do {
			if (p + 4 > end_g)
				break;
			comp_flags = be16(p);
			uint16_t sub_gid = be16(p + 2);
			p += 4;

			float dx = 0, dy = 0;

			if (comp_flags & 0x0002) { /* ARGS_ARE_XY_VALUES */
				if (comp_flags & 0x0001) { /* ARG_1_AND_2_ARE_WORDS */
					dx = sbe16(p);
					dy = sbe16(p + 2);
					p += 4;
				} else {
					dx = (int8_t)p[0];
					dy = (int8_t)p[1];
					p += 2;
				}
			} else {
				p += (comp_flags & 0x0001) ? 4 : 2;
			}

			float ma = 1.0f, mb = 0.0f, mc = 0.0f, md = 1.0f;

			if (comp_flags & 0x0008) { /* WE_HAVE_A_SCALE */
				ma = md = sbe16(p) / 16384.0f;
				p += 2;
			} else if (comp_flags & 0x0040) { /* WE_HAVE_AN_X_AND_Y_SCALE */
				ma = sbe16(p) / 16384.0f;
				md = sbe16(p + 2) / 16384.0f;
				p += 4;
			} else if (comp_flags & 0x0080) { /* WE_HAVE_A_TWO_BY_TWO */
				ma = sbe16(p) / 16384.0f;
				mb = sbe16(p + 2) / 16384.0f;
				mc = sbe16(p + 4) / 16384.0f;
				md = sbe16(p + 6) / 16384.0f;
				p += 8;
			}

			ttf_extract_glyph_polys(f, sub_gid, scale,
						a * ma + c * mb, b * ma + d * mb,
						a * mc + c * md, b * mc + d * md,
						tx + a * dx + c * dy,
						ty + b * dx + d * dy,
						pl, depth + 1);
		} while (comp_flags & 0x0020); /* MORE_COMPONENTS */
	}
}

/* Add a thick round-capped segment to poly_list for fallback glyphs */
static void poly_add_segment(struct poly_list *pl, float x0, float y0,
			     float x1, float y1, float r)
{
	float xs[24], ys[24];
	float dx = x1 - x0, dy = y1 - y0;
	float base_a = (fabsf(dx) < 1e-4f && fabsf(dy) < 1e-4f) ? 0.0f : atan2f(dy, dx);
	int i, n = 0;

	for (i = 0; i <= 8; i++) {
		float a = base_a - (float)M_PI * 0.5f + (float)M_PI * i / 8.0f;

		xs[n] = x1 + r * cosf(a);
		ys[n] = y1 + r * sinf(a);
		n++;
	}
	for (i = 0; i <= 8; i++) {
		float a = base_a + (float)M_PI * 0.5f + (float)M_PI * i / 8.0f;

		xs[n] = x0 + r * cosf(a);
		ys[n] = y0 + r * sinf(a);
		n++;
	}
	poly_add_contour(pl, xs, ys, n);
}

/*
 * Built-in vector glyphs for arrows, play triangle, middle dot, ellipsis,
 * degree sign, and ASCII fallback when .ttf files are not present or a font
 * omits an arrow/symbol glyph.
 */
static bool build_special_or_fallback_glyph(uint32_t cp, int size_px, bool bold,
					    float tx_px, struct poly_list *pl, int *adv_q6_out)
{
	float h = (float)size_px;
	float cap = h * 0.70f;
	float xh = h * 0.48f;
	float sw = h * (bold ? 0.088f : 0.068f);
	if (sw < 0.85f)
		sw = 0.85f;
	float w = h * 0.52f;
	float pad = h * 0.06f + tx_px;

	if (cp == 0x21B5) { /* ↵ (return keycap symbol, centered on cap midline) */
		float rx = pad + w * 0.84f;
		float lx = pad + w * 0.12f;
		float top_y = -cap * 0.82f;
		float mid_y = -cap * 0.36f;

		poly_add_segment(pl, rx, top_y, rx, mid_y, sw * 0.88f);
		poly_add_segment(pl, rx, mid_y, lx, mid_y, sw * 0.88f);
		poly_add_segment(pl, lx + w * 0.28f, mid_y - cap * 0.22f, lx, mid_y, sw * 0.88f);
		poly_add_segment(pl, lx + w * 0.28f, mid_y + cap * 0.22f, lx, mid_y, sw * 0.88f);
		*adv_q6_out = (int)lrintf((w + h * 0.12f) * 64.0f);
		return true;
	}
	if (cp == 0x2014 || cp == 0x2013) { /* — / – (em / en dash) */
		float cy = -cap * 0.48f;
		float dw = (cp == 0x2014) ? (h * 0.72f) : (h * 0.46f);

		poly_add_segment(pl, pad, cy, pad + dw, cy, sw * 0.90f);
		*adv_q6_out = (int)lrintf((dw + h * 0.12f) * 64.0f);
		return true;
	}
	if (cp == 0x2713) { /* ✓ (checkmark, centered on cap midline) */
		float x0 = pad + w * 0.06f, y0 = -cap * 0.44f;
		float x1 = pad + w * 0.36f, y1 = -cap * 0.16f;
		float x2 = pad + w * 0.90f, y2 = -cap * 0.84f;

		poly_add_segment(pl, x0, y0, x1, y1, sw * 0.92f);
		poly_add_segment(pl, x1, y1, x2, y2, sw * 0.92f);
		*adv_q6_out = (int)lrintf((w + h * 0.12f) * 64.0f);
		return true;
	}
	if (cp == 0x2B07 || cp == 0x2193) { /* ⬇ / ↓ (download / down arrow) */
		float cx = pad + w * 0.42f;

		poly_add_segment(pl, cx, -cap * 0.86f, cx, -cap * 0.14f, sw * 0.92f);
		poly_add_segment(pl, cx - w * 0.30f, -cap * 0.42f, cx, -cap * 0.14f, sw * 0.92f);
		poly_add_segment(pl, cx + w * 0.30f, -cap * 0.42f, cx, -cap * 0.14f, sw * 0.92f);
		*adv_q6_out = (int)lrintf((w * 0.84f + h * 0.12f) * 64.0f);
		return true;
	}
	if (cp == 0x2190) { /* ← */
		float cy = -cap * 0.50f;
		poly_add_segment(pl, pad, cy, pad + w, cy, sw);
		poly_add_segment(pl, pad + w * 0.42f, cy - cap * 0.28f, pad, cy, sw);
		poly_add_segment(pl, pad + w * 0.42f, cy + cap * 0.28f, pad, cy, sw);
		*adv_q6_out = (int)lrintf((w + h * 0.15f) * 64.0f);
		return true;
	}
	if (cp == 0x2192) { /* → */
		float cy = -cap * 0.50f;
		poly_add_segment(pl, pad, cy, pad + w, cy, sw);
		poly_add_segment(pl, pad + w * 0.58f, cy - cap * 0.28f, pad + w, cy, sw);
		poly_add_segment(pl, pad + w * 0.58f, cy + cap * 0.28f, pad + w, cy, sw);
		*adv_q6_out = (int)lrintf((w + h * 0.15f) * 64.0f);
		return true;
	}
	if (cp == 0x2191) { /* ↑ */
		float cx = pad + w * 0.4f;
		poly_add_segment(pl, cx, -cap * 0.88f, cx, -cap * 0.12f, sw);
		poly_add_segment(pl, cx - w * 0.3f, -cap * 0.58f, cx, -cap * 0.88f, sw);
		poly_add_segment(pl, cx + w * 0.3f, -cap * 0.58f, cx, -cap * 0.88f, sw);
		*adv_q6_out = (int)lrintf((w * 0.8f + h * 0.12f) * 64.0f);
		return true;
	}
	if (cp == 0x25B6) { /* ▶ */
		float xs[3] = { pad, pad + w * 0.85f, pad };
		float ys[3] = { -cap * 0.86f, -cap * 0.50f, -cap * 0.14f };
		poly_add_contour(pl, xs, ys, 3);
		*adv_q6_out = (int)lrintf((w + h * 0.12f) * 64.0f);
		return true;
	}
	if (cp == 0x00B7) { /* · */
		poly_add_segment(pl, pad + sw * 1.5f, -cap * 0.48f,
				 pad + sw * 1.5f, -cap * 0.48f, sw * 1.35f);
		*adv_q6_out = (int)lrintf((h * 0.12f + sw * 3.0f) * 64.0f);
		return true;
	}
	if (cp == 0x2026) { /* … */
		float sp = h * 0.22f;
		poly_add_segment(pl, pad + sw, -sw, pad + sw, -sw, sw * 1.15f);
		poly_add_segment(pl, pad + sw + sp, -sw, pad + sw + sp, -sw, sw * 1.15f);
		poly_add_segment(pl, pad + sw + sp * 2.0f, -sw, pad + sw + sp * 2.0f, -sw, sw * 1.15f);
		*adv_q6_out = (int)lrintf((h * 0.12f + sp * 2.5f) * 64.0f);
		return true;
	}
	if (cp == 0x00B0) { /* ° */
		float cx = pad + h * 0.16f, cy = -cap * 0.80f, r = h * 0.12f;
		int i;
		for (i = 0; i < 16; i++) {
			float a0 = 2.0f * (float)M_PI * i / 16.0f;
			float a1 = 2.0f * (float)M_PI * (i + 1) / 16.0f;
			poly_add_segment(pl, cx + r * cosf(a0), cy + r * sinf(a0),
					 cx + r * cosf(a1), cy + r * sinf(a1), sw * 0.72f);
		}
		*adv_q6_out = (int)lrintf((h * 0.44f) * 64.0f);
		return true;
	}
	if (cp == 0x00D7) { /* × */
		float cy = -cap * 0.48f, s = xh * 0.34f;
		poly_add_segment(pl, pad, cy - s, pad + s * 2.0f, cy + s, sw * 0.90f);
		poly_add_segment(pl, pad, cy + s, pad + s * 2.0f, cy - s, sw * 0.90f);
		*adv_q6_out = (int)lrintf((h * 0.12f + s * 2.2f) * 64.0f);
		return true;
	}
	return false;
}

static struct glyph_entry *rasterize_glyph(enum shell_font_role role, int size_px,
					   uint32_t cp, uint8_t phase)
{
	const struct ttf_font *f = &fonts[role];
	struct poly_list pl = { 0 };
	struct glyph_entry *ge;
	int adv_q6 = 0;
	bool bold = (role == SHELL_FONT_SANS_BOLD || role == SHELL_FONT_SANS_SEMIBOLD ||
		     role == SHELL_FONT_MONO_SEMIBOLD);
	bool used_special = false;
	float sub_x_px = (float)(phase & 3) * 0.25f;

	if (!f->data && fonts[SHELL_FONT_SANS_MEDIUM].data)
		f = &fonts[SHELL_FONT_SANS_MEDIUM];

	if (cp == ' ') {
		ge = calloc(1, sizeof(*ge));
		if (!ge)
			return NULL;
		ge->codepoint = cp;
		ge->size_px = (uint16_t)size_px;
		ge->role = (uint8_t)role;
		ge->phase = phase & 3;
		ge->advance_q6 = (int16_t)(size_px * 64 * 28 / 100);
		if (f->data) {
			uint16_t glyph_id = ttf_find_glyph(f, ' ');
			uint16_t adv = 0;
			int16_t lsb = 0;

			ttf_get_hmetrics(f, glyph_id, &adv, &lsb);
			if (adv)
				ge->advance_q6 = (int16_t)lrintf((float)adv * size_px * 64.0f / f->units_per_em);
		}
		ge->advance = (int16_t)((ge->advance_q6 + 32) >> 6);
		return ge;
	}

	/* Always use built-in crisp geometric glyphs for UI symbols ↵, ✓, ⬇ */
	if (cp == 0x21B5 || cp == 0x2713 || cp == 0x2B07) {
		used_special = build_special_or_fallback_glyph(cp, size_px, bold,
							       sub_x_px, &pl, &adv_q6);
	}

	uint16_t glyph_id = (!used_special && f->data) ? ttf_find_glyph(f, cp) : 0;

	if (glyph_id != 0 && f->data) {
		float scale = (float)size_px / (float)f->units_per_em;
		uint16_t adv = 0;
		int16_t lsb = 0;

		ttf_get_hmetrics(f, glyph_id, &adv, &lsb);
		adv_q6 = (int)lrintf((float)adv * scale * 64.0f);
		ttf_extract_glyph_polys(f, glyph_id, scale, 1.0f, 0.0f, 0.0f, 1.0f,
					sub_x_px / scale, 0.0f, &pl, 0);
	} else if (!used_special) {
		used_special = build_special_or_fallback_glyph(cp, size_px, bold,
							       sub_x_px, &pl, &adv_q6);
		if (!used_special && f->data && cp > 127) {
			uint32_t ascii_cp = (cp == 0x2018 || cp == 0x2019) ? '\'' :
					    (cp == 0x201C || cp == 0x201D) ? '"' :
					    (cp == 0x2013 || cp == 0x2014) ? '-' : '?';

			glyph_id = ttf_find_glyph(f, ascii_cp);
			if (glyph_id) {
				float scale = (float)size_px / (float)f->units_per_em;
				uint16_t adv = 0;
				int16_t lsb = 0;

				ttf_get_hmetrics(f, glyph_id, &adv, &lsb);
				adv_q6 = (int)lrintf((float)adv * scale * 64.0f);
				ttf_extract_glyph_polys(f, glyph_id, scale, 1.0f, 0.0f, 0.0f, 1.0f,
							sub_x_px / scale, 0.0f, &pl, 0);
			}
		}
	}

	ge = calloc(1, sizeof(*ge));
	if (!ge) {
		free(pl.pts);
		free(pl.contour_len);
		return NULL;
	}
	ge->codepoint = cp;
	ge->size_px = (uint16_t)size_px;
	ge->role = (uint8_t)role;
	ge->phase = phase & 3;
	ge->advance_q6 = (int16_t)(adv_q6 > 0 ? adv_q6 : (size_px * 32));
	ge->advance = (int16_t)((ge->advance_q6 + 32) >> 6);

	if (pl.ncontours == 0 || pl.npts == 0) {
		free(pl.pts);
		free(pl.contour_len);
		return ge;
	}

	int32_t min_x = pl.pts[0].x, max_x = pl.pts[0].x;
	int32_t min_y = pl.pts[0].y, max_y = pl.pts[0].y;
	int i;

	for (i = 1; i < pl.npts; i++) {
		if (pl.pts[i].x < min_x)
			min_x = pl.pts[i].x;
		if (pl.pts[i].x > max_x)
			max_x = pl.pts[i].x;
		if (pl.pts[i].y < min_y)
			min_y = pl.pts[i].y;
		if (pl.pts[i].y > max_y)
			max_y = pl.pts[i].y;
	}

	int px0 = (min_x >= 0) ? (min_x / SUB) : ((min_x - SUB + 1) / SUB);
	int py0 = (min_y >= 0) ? (min_y / SUB) : ((min_y - SUB + 1) / SUB);
	int px1 = (max_x >= 0) ? (max_x / SUB + 1) : (max_x / SUB);
	int py1 = (max_y >= 0) ? (max_y / SUB + 1) : (max_y / SUB);
	int gw = px1 - px0 + 1;
	int gh = py1 - py0 + 1;

	if (gw <= 0 || gh <= 0 || gw > 256 || gh > 256) {
		free(pl.pts);
		free(pl.contour_len);
		return ge;
	}

	ge->xoff = (int16_t)px0;
	ge->yoff = (int16_t)py0;
	ge->w = (uint16_t)gw;
	ge->h = (uint16_t)gh;
	ge->bitmap = calloc((size_t)gw * gh, 1);
	if (!ge->bitmap) {
		free(pl.pts);
		free(pl.contour_len);
		return ge;
	}

	for (i = 0; i < pl.npts; i++) {
		pl.pts[i].x -= px0 * SUB;
		pl.pts[i].y -= py0 * SUB;
	}

	uint16_t row_cov[256];
	struct {
		int32_t x;
		int dir;
	} cr[128];
	int y, s, c;

	for (y = 0; y < gh; y++) {
		memset(row_cov, 0, (size_t)gw * sizeof(uint16_t));
		for (s = 0; s < SAMPLES; s++) {
			int32_t sy = y * SUB + s * (SUB / SAMPLES) + (SUB / SAMPLES) / 2;
			int ncr = 0, pt_base = 0;

			for (c = 0; c < pl.ncontours; c++) {
				int clen = pl.contour_len[c];
				int k;

				for (k = 0; k < clen; k++) {
					struct poly_pt a = pl.pts[pt_base + k];
					struct poly_pt b = pl.pts[pt_base + ((k + 1 == clen) ? 0 : (k + 1))];
					int32_t ylo = a.y < b.y ? a.y : b.y;
					int32_t yhi = a.y < b.y ? b.y : a.y;

					if (sy < ylo || sy >= yhi || ncr >= 128)
						continue;
					int32_t cx = a.x + ((sy - a.y) * (b.x - a.x)) / (b.y - a.y);
					int dir = (b.y > a.y) ? 1 : -1;
					int m = ncr++;

					while (m > 0 && cr[m - 1].x > cx) {
						cr[m] = cr[m - 1];
						m--;
					}
					cr[m].x = cx;
					cr[m].dir = dir;
				}
				pt_base += clen;
			}

			int wind = 0, k;

			for (k = 0; k + 1 < ncr; k++) {
				wind += cr[k].dir;
				if (wind != 0) {
					int32_t xa = cr[k].x < 0 ? 0 : cr[k].x;
					int32_t xb = cr[k + 1].x > gw * SUB ? gw * SUB : cr[k + 1].x;

					if (xa >= xb)
						continue;
					int pa = xa >> 5;
					int pb = xb >> 5;

					if (pa == pb) {
						row_cov[pa] += (uint16_t)(xb - xa);
					} else {
						row_cov[pa] += (uint16_t)(SUB - (xa & (SUB - 1)));
						int p;
						for (p = pa + 1; p < pb; p++)
							row_cov[p] += SUB;
						if (pb < gw)
							row_cov[pb] += (uint16_t)(xb & (SUB - 1));
					}
				}
			}
		}
		for (i = 0; i < gw; i++) {
			uint32_t v = ((uint32_t)row_cov[i] * 255U + 64U) >> 7;

			if (v > 255U)
				v = 255U;
			/* Bake contrast-preserving gamma boost once at rasterization time */
			v += (v * (255U - v)) >> 9;
			if (v > 255U)
				v = 255U;
			ge->bitmap[y * gw + i] = (uint8_t)v;
		}
	}

	free(pl.pts);
	free(pl.contour_len);
	return ge;
}

static struct glyph_entry *get_glyph_phased(enum shell_font_role role, int size_px,
					    uint32_t cp, uint8_t phase)
{
	phase &= 3;
	uint32_t h = ((cp * 2654435761U) ^ ((uint32_t)size_px << 8) ^
		      ((uint32_t)role << 3) ^ phase) & (GLYPH_HASH_BUCKETS - 1);
	struct glyph_entry *e = glyph_cache[h];

	while (e) {
		if (e->codepoint == cp && e->size_px == size_px &&
		    e->role == (uint8_t)role && e->phase == phase)
			return e;
		e = e->next;
	}
	e = rasterize_glyph(role, size_px, cp, phase);
	if (e) {
		e->next = glyph_cache[h];
		glyph_cache[h] = e;
	}
	return e;
}

static inline struct glyph_entry *get_glyph(enum shell_font_role role, int size_px, uint32_t cp)
{
	return get_glyph_phased(role, size_px, cp, 0);
}

static uint32_t utf8_next(const char **p)
{
	const uint8_t *s = (const uint8_t *)*p;
	uint32_t cp;

	if (!*s)
		return 0;
	if (s[0] < 0x80) {
		*p += 1;
		return s[0];
	}
	if ((s[0] & 0xe0) == 0xc0 && (s[1] & 0xc0) == 0x80) {
		cp = ((uint32_t)(s[0] & 0x1f) << 6) | (s[1] & 0x3f);
		*p += 2;
		return cp;
	}
	if ((s[0] & 0xf0) == 0xe0 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80) {
		cp = ((uint32_t)(s[0] & 0x0f) << 12) |
		     ((uint32_t)(s[1] & 0x3f) << 6) | (s[2] & 0x3f);
		*p += 3;
		return cp;
	}
	if ((s[0] & 0xf8) == 0xf0 && (s[1] & 0xc0) == 0x80 &&
	    (s[2] & 0xc0) == 0x80 && (s[3] & 0xc0) == 0x80) {
		cp = ((uint32_t)(s[0] & 0x07) << 18) |
		     ((uint32_t)(s[1] & 0x3f) << 12) |
		     ((uint32_t)(s[2] & 0x3f) << 6) | (s[3] & 0x3f);
		*p += 4;
		return cp;
	}
	*p += 1;
	return '?';
}

int shell_font_init(void)
{
	const char *env_dir = getenv("TVSHELL_FONT_DIR");
	int r;

	for (r = 0; r < SHELL_FONT_ROLE_COUNT; r++) {
		char path[256];
		const char *fname = font_filenames[r];

		if (env_dir && env_dir[0]) {
			snprintf(path, sizeof(path), "%s/%s", env_dir, fname);
			if (ttf_load_file(&fonts[r], path) == 0)
				continue;
		}
		snprintf(path, sizeof(path), "/usr/share/fonts/tvshell/%s", fname);
		if (ttf_load_file(&fonts[r], path) == 0)
			continue;
		snprintf(path, sizeof(path), "build/dl/fonts/%s", fname);
		(void)ttf_load_file(&fonts[r], path);
	}

	/* Pre-warm all UI sizes so interactive navigation and modals never miss cache */
	static const struct {
		enum shell_font_role role;
		int size_px;
	} warmup[] = {
		{ SHELL_FONT_SANS_BOLD,     24 },
		{ SHELL_FONT_SANS_BOLD,     22 },
		{ SHELL_FONT_SANS_BOLD,     21 },
		{ SHELL_FONT_SANS_BOLD,     18 },
		{ SHELL_FONT_SANS_BOLD,     17 },
		{ SHELL_FONT_SANS_BOLD,     15 },
		{ SHELL_FONT_SANS_SEMIBOLD, 20 },
		{ SHELL_FONT_SANS_SEMIBOLD, 19 },
		{ SHELL_FONT_SANS_SEMIBOLD, 14 },
		{ SHELL_FONT_SANS_MEDIUM,   18 },
		{ SHELL_FONT_SANS_MEDIUM,   15 },
		{ SHELL_FONT_SANS_MEDIUM,   13 },
		{ SHELL_FONT_SANS_MEDIUM,   11 },
		{ SHELL_FONT_MONO_SEMIBOLD, 12 },
		{ SHELL_FONT_MONO_SEMIBOLD, 11 },
		{ SHELL_FONT_MONO_MEDIUM,   13 },
		{ SHELL_FONT_MONO_MEDIUM,   11 },
	};
	static const uint32_t special_cps[] = {
		0x21B5, 0x2014, 0x2013, 0x00B7, 0x00D7, 0x2026
	};
	size_t wi, si;
	uint32_t cp;

	for (wi = 0; wi < sizeof(warmup) / sizeof(warmup[0]); wi++) {
		for (cp = 32; cp <= 122; cp++)
			(void)get_glyph(warmup[wi].role, warmup[wi].size_px, cp);
		for (si = 0; si < sizeof(special_cps) / sizeof(special_cps[0]); si++)
			(void)get_glyph(warmup[wi].role, warmup[wi].size_px, special_cps[si]);
	}
	return 0;
}

void shell_font_free(void)
{
	int i, r;

	for (i = 0; i < GLYPH_HASH_BUCKETS; i++) {
		struct glyph_entry *e = glyph_cache[i];

		while (e) {
			struct glyph_entry *next = e->next;

			free(e->bitmap);
			free(e);
			e = next;
		}
		glyph_cache[i] = NULL;
	}
	for (r = 0; r < SHELL_FONT_ROLE_COUNT; r++) {
		if (fonts[r].data)
			munmap(fonts[r].data, fonts[r].size);
	}
	memset(fonts, 0, sizeof(fonts));
}

int shell_font_cap_height(enum shell_font_role role, int size_px)
{
	struct glyph_entry *ge = get_glyph_phased(role, size_px, 'H', 0);

	if (ge && ge->yoff < 0)
		return -ge->yoff;
	return (size_px * 70 + 50) / 100;
}

int shell_font_measure_role(enum shell_font_role role, int size_px, const char *utf8)
{
	const char *p = utf8;
	int w_q6 = 0;
	uint32_t cp;

	if (!p)
		return 0;
	while ((cp = utf8_next(&p)) != 0) {
		struct glyph_entry *ge = get_glyph_phased(role, size_px, cp, 0);

		if (ge)
			w_q6 += ge->advance_q6;
	}
	return (w_q6 + 32) >> 6;
}

static void blit_glyph(const struct glyph_entry *ge, int pen_x, int baseline_y,
		       uint32_t fg)
{
	uint32_t *fb = shell_display_backbuffer();
	int py, px;
	uint32_t fg_opaque, fg_rb, fg_g;
	int x0, gw;

	if (!ge || !ge->bitmap || !fb)
		return;

	fg_opaque = fg | 0xff000000U;
	fg_rb = fg & 0x00ff00ffU;
	fg_g  = fg & 0x0000ff00U;
	x0 = pen_x + ge->xoff;
	gw = (int)ge->w;

#ifdef __ARM_NEON
	uint8x8_t fg_b_v = vdup_n_u8((uint8_t)(fg & 0xffU));
	uint8x8_t fg_g_v = vdup_n_u8((uint8_t)((fg >> 8) & 0xffU));
	uint8x8_t fg_r_v = vdup_n_u8((uint8_t)((fg >> 16) & 0xffU));
	uint8x8_t ff_v   = vdup_n_u8(0xffU);
	bool inside_x    = (x0 >= 0 && x0 + gw <= SHELL_SCREEN_W);
#endif

	for (py = 0; py < (int)ge->h; py++) {
		int rel_y = ge->yoff + py;
		int sy = baseline_y + rel_y;

		if (sy < 0 || sy >= SHELL_SCREEN_H)
			continue;
		uint32_t *row = fb + sy * SHELL_SCREEN_W;
		const uint8_t *grow = ge->bitmap + py * gw;

		px = 0;
#ifdef __ARM_NEON
		if (inside_x) {
			for (; px + 8 <= gw; px += 8) {
				uint8x8_t a8 = vld1_u8(grow + px);

				if (vget_lane_u64(vreinterpret_u64_u8(a8), 0) == 0)
					continue;
				uint8x8_t inv_a = vmvn_u8(a8);
				uint8x8x4_t d = vld4_u8((const uint8_t *)(row + x0 + px));

				d.val[0] = vrshrn_n_u16(vmlal_u8(vmull_u8(fg_b_v, a8), d.val[0], inv_a), 8);
				d.val[1] = vrshrn_n_u16(vmlal_u8(vmull_u8(fg_g_v, a8), d.val[1], inv_a), 8);
				d.val[2] = vrshrn_n_u16(vmlal_u8(vmull_u8(fg_r_v, a8), d.val[2], inv_a), 8);
				d.val[3] = ff_v;
				vst4_u8((uint8_t *)(row + x0 + px), d);
			}
		}
#endif
		for (; px < gw; px++) {
			uint32_t a255 = grow[px];
			int sx;

			if (!a255)
				continue;
			sx = x0 + px;
			if (sx < 0 || sx >= SHELL_SCREEN_W)
				continue;
			if (a255 == 255) {
				row[sx] = fg_opaque;
			} else {
				uint32_t a = a255 + (a255 >> 7);
				uint32_t inv_a = 256U - a;
				uint32_t dst = row[sx];
				uint32_t rb = (((dst & 0x00ff00ffU) * inv_a + fg_rb * a) >> 8) & 0x00ff00ffU;
				uint32_t g  = (((dst & 0x0000ff00U) * inv_a + fg_g * a) >> 8) & 0x0000ff00U;

				row[sx] = 0xff000000U | rb | g;
			}
		}
	}
}

static int draw_role_at_baseline_ex(enum shell_font_role role, int size_px,
				    int x, int baseline_y, uint32_t fg,
				    const char *utf8, int max_width,
				    bool emit_damage)
{
	const char *p = utf8;
	int pen_x_q6 = x << 6;
	int start_x_q6 = pen_x_q6;
	int max_w_q6 = max_width > 0 ? (max_width << 6) : 0;
	uint32_t cp;

	if (!utf8 || !utf8[0])
		return 0;

	while ((cp = utf8_next(&p)) != 0) {
		struct glyph_entry *ge = get_glyph_phased(role, size_px, cp, 0);

		if (!ge)
			continue;
		if (max_w_q6 > 0 && (pen_x_q6 - start_x_q6) + ge->advance_q6 > max_w_q6)
			break;
		blit_glyph(ge, (pen_x_q6 + 32) >> 6, baseline_y, fg);
		pen_x_q6 += ge->advance_q6;
	}

	int drawn_w = (pen_x_q6 - start_x_q6 + 32) >> 6;
	if (emit_damage)
		shell_display_damage(x - 4, baseline_y - size_px - 4, drawn_w + 16, size_px + 16);
	return drawn_w;
}

static int draw_role_at_baseline(enum shell_font_role role, int size_px,
				 int x, int baseline_y, uint32_t fg,
				 const char *utf8, int max_width)
{
	return draw_role_at_baseline_ex(role, size_px, x, baseline_y, fg, utf8, max_width, true);
}

/*
 * Draw single-line text with zero ellipsis truncation (stops cleanly at word
 * or character boundary if max_width > 0). Centers cap-height vertically inside
 * the [y, y + size_px] line box so top and bottom margins are equal.
 */
int shell_font_draw_role(enum shell_font_role role, int size_px,
			 int x, int y, uint32_t fg, const char *utf8, int max_width)
{
	int cap_h = shell_font_cap_height(role, size_px);
	int baseline_y = y + (size_px + cap_h) / 2;

	return draw_role_at_baseline(role, size_px, x, baseline_y, fg, utf8, max_width);
}

/*
 * Draw single-line text vertically centered inside [box_y, box_y + box_h]
 * using the exact measured cap-height of the font so top and bottom margins
 * above capitals and below the baseline are identical to the pixel.
 */
int shell_font_draw_vcentered(enum shell_font_role role, int size_px,
			      int x, int box_y, int box_h,
			      uint32_t fg, const char *utf8, int max_width)
{
	int cap_h = shell_font_cap_height(role, size_px);
	int baseline_y = box_y + (box_h + cap_h) / 2;

	return draw_role_at_baseline(role, size_px, x, baseline_y, fg, utf8, max_width);
}

/*
 * Draw single-line text both horizontally and vertically centered inside
 * [box_x, box_y, box_w, box_h].
 */
int shell_font_draw_box_centered(enum shell_font_role role, int size_px,
				 int box_x, int box_y, int box_w, int box_h,
				 uint32_t fg, const char *utf8)
{
	int tw = shell_font_measure_role(role, size_px, utf8);
	int tx = box_x + (box_w - tw) / 2;

	if (tx < box_x)
		tx = box_x;
	return shell_font_draw_vcentered(role, size_px, tx, box_y, box_h,
					 fg, utf8, box_w);
}

/*
 * Draw multi-line wrapped text with zero ellipsis truncation (O(N) incremental
 * Q6 width accumulation).
 */
int shell_font_draw_wrapped_role(enum shell_font_role role, int size_px,
				 int x, int y, int line_height, int max_width,
				 int max_lines, uint32_t fg, const char *utf8)
{
	char line[384];
	const char *p = utf8;
	int lines_drawn = 0;
	int max_w_q6 = max_width << 6;
	struct glyph_entry *space_ge;
	int space_q6;

	if (!utf8 || !utf8[0])
		return 0;

	space_ge = get_glyph_phased(role, size_px, ' ', 0);
	space_q6 = space_ge ? space_ge->advance_q6 : (size_px * 18);

	while (*p && lines_drawn < max_lines) {
		while (*p == ' ')
			p++;
		if (!*p)
			break;

		const char *scan = p;
		const char *last_fit = p;
		int line_w_q6 = 0;

		while (*scan && *scan != '\n') {
			const char *word_end = scan;
			const char *wp = scan;
			int word_w_q6 = 0;

			while (*word_end && *word_end != ' ' && *word_end != '\n')
				word_end++;

			while (wp < word_end) {
				uint32_t cp = utf8_next(&wp);
				struct glyph_entry *ge = get_glyph_phased(role, size_px, cp, 0);

				if (ge)
					word_w_q6 += ge->advance_q6;
			}

			size_t candidate_len = (size_t)(word_end - p);
			if (candidate_len >= sizeof(line))
				break;

			int next_w_q6 = line_w_q6 + word_w_q6;
			if ((next_w_q6 + 32) <= max_w_q6 || last_fit == p) {
				last_fit = word_end;
				line_w_q6 = next_w_q6;
				if ((next_w_q6 + 32) > max_w_q6)
					break;
			} else {
				break;
			}

			scan = word_end;
			while (*scan == ' ') {
				line_w_q6 += space_q6;
				scan++;
			}
		}

		size_t final_len = (size_t)(last_fit - p);

		if (final_len >= sizeof(line))
			final_len = sizeof(line) - 1;
		memcpy(line, p, final_len);
		line[final_len] = '\0';

		shell_font_draw_role(role, size_px, x, y + lines_drawn * line_height,
				     fg, line, max_width);
		lines_drawn++;

		p = last_fit;
		if (*p == '\n')
			p++;
	}

	return lines_drawn * line_height;
}

/*
 * Render Slot 2 Editorial Summary Body (18px Plus Jakarta Sans Medium, max 3 lines)
 * with sentence-level highlight pill (.summary-sentence.speaking) when Play/Pause
 * read-aloud is active.
 */
int shell_font_draw_summary_body(int x, int y, int line_height, int max_width,
				 int max_lines, uint32_t fg, uint32_t highlight_color,
				 uint8_t highlight_alpha, const char *utf8,
				 int active_sentence_idx, int *total_sentences_out)
{
	enum shell_font_role role = SHELL_FONT_SANS_MEDIUM;
	int size_px = 18;
	const char *p = utf8;
	int cur_line = 0;
	int pen_x = x;
	int cur_sentence = 0;
	int space_w;
	struct {
		char text[96];
		int wx;
		int wy;
		int ww;
		int line;
		int sentence;
	} words[80];
	int word_count = 0;
	int line_hl_x0[8];
	int line_hl_x1[8];
	int i;

	if (!utf8 || !utf8[0]) {
		if (total_sentences_out)
			*total_sentences_out = 0;
		return 0;
	}

	for (i = 0; i < 8; i++) {
		line_hl_x0[i] = 100000;
		line_hl_x1[i] = -1;
	}

	space_w = shell_font_measure_role(role, size_px, " ");
	if (space_w <= 0)
		space_w = 5;

	while (*p) {
		while (*p == ' ' || *p == '\n')
			p++;
		if (!*p)
			break;

		const char *w_end = p;

		while (*w_end && *w_end != ' ' && *w_end != '\n')
			w_end++;

		char word[96];
		size_t wlen = (size_t)(w_end - p);

		if (wlen >= sizeof(word))
			wlen = sizeof(word) - 1;
		memcpy(word, p, wlen);
		word[wlen] = '\0';

		int ww = shell_font_measure_role(role, size_px, word);

		if (pen_x > x && (pen_x - x) + ww > max_width) {
			cur_line++;
			pen_x = x;
		}
		if (cur_line < max_lines && cur_line < 8 && word_count < 80) {
			int wy = y + cur_line * line_height;

			if (cur_sentence == active_sentence_idx && highlight_alpha > 0) {
				if (pen_x < line_hl_x0[cur_line])
					line_hl_x0[cur_line] = pen_x;
				if (pen_x + ww > line_hl_x1[cur_line])
					line_hl_x1[cur_line] = pen_x + ww;
			}
			snprintf(words[word_count].text, sizeof(words[word_count].text), "%s", word);
			words[word_count].wx = pen_x;
			words[word_count].wy = wy;
			words[word_count].ww = ww;
			words[word_count].line = cur_line;
			words[word_count].sentence = cur_sentence;
			word_count++;
			pen_x += ww + space_w;
		}

		/* Check if this word ends a sentence */
		if (wlen > 0 && (word[wlen - 1] == '.' ||
				 word[wlen - 1] == '!' ||
				 word[wlen - 1] == '?')) {
			const char *next = w_end;

			while (*next == ' ')
				next++;
			if (*next)
				cur_sentence++;
		}

		p = w_end;
	}

	/* First draw continuous highlight pill per line for the active sentence */
	if (active_sentence_idx >= 0 && highlight_alpha > 0) {
		for (i = 0; i < max_lines && i < 8; i++) {
			if (line_hl_x1[i] > line_hl_x0[i]) {
				int wy = y + i * line_height;

				shell_draw_rounded_rect_alpha(line_hl_x0[i] - 4, wy - 1,
							      (line_hl_x1[i] - line_hl_x0[i]) + 8,
							      size_px + 5, 4,
							      highlight_color, highlight_alpha);
			}
		}
	}

	/* Then draw all words cleanly on top and emit a single coalesced damage rect */
	{
		int cap_h = shell_font_cap_height(role, size_px);

		for (i = 0; i < word_count; i++) {
			int baseline_y = words[i].wy + (size_px + cap_h) / 2;

			draw_role_at_baseline_ex(role, size_px, words[i].wx,
						 baseline_y, fg,
						 words[i].text, 0, false);
		}
		if (word_count > 0) {
			int lines_used = cur_line < max_lines ? (cur_line + 1) : max_lines;

			shell_display_damage(x - 6, y - 4, max_width + 20,
					     lines_used * line_height + 12);
		}
	}

	if (total_sentences_out)
		*total_sentences_out = cur_sentence + 1;
	return (cur_line + 1) * line_height;
}

/*
 * Fit a phrase cleanly within max_chars with zero '...' ellipsis truncation,
 * matching fit_clean_phrase() in giri_server.py.
 */
void shell_font_fit_clean_phrase(const char *src, char *dst, size_t dst_sz, int max_chars)
{
	char buf[512];
	size_t i = 0, len;

	if (!dst || dst_sz == 0)
		return;
	dst[0] = '\0';
	if (!src)
		return;

	while (*src == ' ')
		src++;
	while (*src && i + 1 < sizeof(buf)) {
		if (*src == '.' && src[1] == '.' && src[2] == '.') {
			src += 3;
			continue;
		}
		if ((uint8_t)src[0] == 0xe2 && (uint8_t)src[1] == 0x80 &&
		    (uint8_t)src[2] == 0xa6) {
			src += 3;
			continue;
		}
		buf[i++] = *src++;
	}
	buf[i] = '\0';
	len = strlen(buf);
	while (len > 0 && (buf[len - 1] == ' ' || buf[len - 1] == '-' ||
			   buf[len - 1] == ',' || buf[len - 1] == ';' ||
			   buf[len - 1] == ':'))
		buf[--len] = '\0';

	if ((int)len > max_chars && max_chars > 0) {
		int cut = max_chars;

		while (cut > max_chars / 2 && buf[cut] != ' ')
			cut--;
		if (cut <= max_chars / 2)
			cut = max_chars;
		buf[cut] = '\0';
		len = (size_t)cut;
		while (len > 0 && (buf[len - 1] == ' ' || buf[len - 1] == '-' ||
				   buf[len - 1] == ',' || buf[len - 1] == ';' ||
				   buf[len - 1] == ':'))
			buf[--len] = '\0';
	}

	if (len >= dst_sz)
		len = dst_sz - 1;
	memcpy(dst, buf, len);
	dst[len] = '\0';
}

/*
 * Fit a summary paragraph within max_chars (<= 250) on sentence/word boundaries
 * with zero '...' ellipsis truncation, matching fit_short_summary() in giri_server.py.
 */
void shell_font_fit_short_summary(const char *src, char *dst, size_t dst_sz, int max_chars)
{
	shell_font_fit_clean_phrase(src, dst, dst_sz, max_chars);
	size_t len = strlen(dst);

	if (len > 0 && dst[len - 1] != '.' && dst[len - 1] != '!' &&
	    dst[len - 1] != '?' && len + 1 < dst_sz && (int)len < max_chars) {
		dst[len] = '.';
		dst[len + 1] = '\0';
	}
}
