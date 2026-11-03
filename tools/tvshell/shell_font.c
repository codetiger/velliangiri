// SPDX-License-Identifier: GPL-2.0-only
/*
 * Velliangiri 1280x720 TV Shell - TrueType (.ttf) & Vector Fallback Font Engine
 *
 * Parses TrueType outlines (head, hhea, maxp, cmap, hmtx, loca, glyf) from
 * /usr/share/fonts/tvshell/{Literata,AtkinsonHyperlegible}-*.ttf and caches
 * anti-aliased glyph bitmaps for the shell's fixed type scale:
 *   16, 18, 20, 22, 24, 26, 28, 34, 44, 64 px
 * Includes a built-in geometric vector fallback so text remains legible even
 * if the external font files are absent.
 */

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tvshell.h"
#include "shell_assets.h"

#define SUB		16
#define SAMPLES		16

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
	uint32_t hmtx_off;
	uint32_t loca_off;
	uint32_t glyf_off;
};

struct glyph_entry {
	uint32_t codepoint;
	uint16_t size_px;
	uint8_t theme;
	uint8_t bold;
	uint8_t italic;
	int16_t advance;
	int16_t xoff;
	int16_t yoff;
	uint16_t w;
	uint16_t h;
	uint8_t *bitmap;
	struct glyph_entry *next;
};

#define GLYPH_HASH_BUCKETS	512

static struct ttf_font fonts[2][2]; /* [theme][bold] */
static struct glyph_entry *glyph_cache[GLYPH_HASH_BUCKETS];

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
	FILE *fp;
	long sz;
	uint16_t num_tables, i;

	memset(f, 0, sizeof(*f));
	fp = fopen(path, "rb");
	if (!fp)
		return -1;
	if (fseek(fp, 0, SEEK_END) != 0 || (sz = ftell(fp)) < 64 ||
	    fseek(fp, 0, SEEK_SET) != 0) {
		fclose(fp);
		return -1;
	}
	f->data = malloc((size_t)sz);
	if (!f->data || fread(f->data, 1, (size_t)sz, fp) != (size_t)sz) {
		free(f->data);
		f->data = NULL;
		fclose(fp);
		return -1;
	}
	fclose(fp);
	f->size = (size_t)sz;

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
	return 0;

bad:
	free(f->data);
	memset(f, 0, sizeof(*f));
	return -1;
}

static uint16_t ttf_find_glyph(const struct ttf_font *f, uint32_t cp)
{
	const uint8_t *cmap = f->data + f->cmap_off;
	uint16_t ntables = be16(cmap + 2);
	uint32_t fmt4_off = 0, fmt12_off = 0;
	uint16_t i;

	for (i = 0; i < ntables; i++) {
		const uint8_t *rec = cmap + 4 + i * 8;
		uint16_t pid = be16(rec);
		uint16_t eid = be16(rec + 2);
		uint32_t sub_off = f->cmap_off + be32(rec + 4);

		if (sub_off + 8 > f->size)
			continue;
		if ((pid == 3 && (eid == 1 || eid == 10)) || pid == 0) {
			uint16_t fmt = be16(f->data + sub_off);

			if (fmt == 12)
				fmt12_off = sub_off;
			else if (fmt == 4 && !fmt4_off)
				fmt4_off = sub_off;
		}
	}

	if (fmt12_off && fmt12_off + 16 <= f->size) {
		const uint8_t *sub = f->data + fmt12_off;
		uint32_t ngroups = be32(sub + 12);
		uint32_t g;

		for (g = 0; g < ngroups && fmt12_off + 16 + (g + 1) * 12 <= f->size; g++) {
			const uint8_t *grp = sub + 16 + g * 12;
			uint32_t start = be32(grp);
			uint32_t end = be32(grp + 4);
			uint32_t glyph_id = be32(grp + 8);

			if (cp >= start && cp <= end)
				return (uint16_t)(glyph_id + (cp - start));
		}
	}

	if (fmt4_off && cp <= 0xffffU && fmt4_off + 14 <= f->size) {
		const uint8_t *sub = f->data + fmt4_off;
		uint16_t seg_count = be16(sub + 6) / 2;
		const uint8_t *end_codes = sub + 14;
		const uint8_t *start_codes = end_codes + seg_count * 2 + 2;
		const uint8_t *id_deltas = start_codes + seg_count * 2;
		const uint8_t *id_range_offs = id_deltas + seg_count * 2;
		uint16_t s;

		for (s = 0; s < seg_count; s++) {
			uint16_t end_c = be16(end_codes + s * 2);
			uint16_t start_c;

			if (cp > end_c)
				continue;
			start_c = be16(start_codes + s * 2);
			if (cp < start_c)
				return 0;
			int16_t delta = sbe16(id_deltas + s * 2);
			uint16_t roff = be16(id_range_offs + s * 2);

			if (roff == 0)
				return (uint16_t)((cp + delta) & 0xffffU);
			const uint8_t *gptr = id_range_offs + s * 2 + roff +
					      (cp - start_c) * 2;
			if ((size_t)(gptr - f->data) + 2 > f->size)
				return 0;
			uint16_t glyph_id = be16(gptr);

			if (glyph_id == 0)
				return 0;
			return (uint16_t)((glyph_id + delta) & 0xffffU);
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
	int steps = 16, i;

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

		uint8_t *flags = malloc(npts);
		int16_t *raw_x = malloc((size_t)npts * sizeof(int16_t));
		int16_t *raw_y = malloc((size_t)npts * sizeof(int16_t));
		float *cxs = malloc(4096 * sizeof(float));
		float *cys = malloc(4096 * sizeof(float));

		if (!flags || !raw_x || !raw_y || !cxs || !cys)
			goto out_simple;

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
								    px, py, cxs, cys, &out_n, 4090);
						have_ctrl = false;
					} else if (out_n < 4090) {
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
								    mx, my, cxs, cys, &out_n, 4090);
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
						    start_x, start_y, cxs, cys, &out_n, 4090);
			}
			#undef TX
			#undef TY

			poly_add_contour(pl, cxs, cys, out_n);
			start_pt = end_pt + 1;
		}

out_simple:
		free(flags);
		free(raw_x);
		free(raw_y);
		free(cxs);
		free(cys);
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
					    struct poly_list *pl, int *adv_out)
{
	float h = (float)size_px;
	float cap = h * 0.70f;
	float xh = h * 0.48f;
	float sw = h * (bold ? 0.095f : 0.072f);
	if (sw < 0.9f)
		sw = 0.9f;
	float w = h * 0.52f;
	float pad = h * 0.06f;

	if (cp == 0x2190) { /* ← */
		float cy = -cap * 0.5f;
		poly_add_segment(pl, pad, cy, pad + w, cy, sw);
		poly_add_segment(pl, pad + w * 0.42f, cy - cap * 0.30f, pad, cy, sw);
		poly_add_segment(pl, pad + w * 0.42f, cy + cap * 0.30f, pad, cy, sw);
		*adv_out = (int)lrintf(w + pad * 2.5f);
		return true;
	}
	if (cp == 0x2192) { /* → */
		float cy = -cap * 0.5f;
		poly_add_segment(pl, pad, cy, pad + w, cy, sw);
		poly_add_segment(pl, pad + w * 0.58f, cy - cap * 0.30f, pad + w, cy, sw);
		poly_add_segment(pl, pad + w * 0.58f, cy + cap * 0.30f, pad + w, cy, sw);
		*adv_out = (int)lrintf(w + pad * 2.5f);
		return true;
	}
	if (cp == 0x2191) { /* ↑ */
		float cx = pad + w * 0.4f;
		poly_add_segment(pl, cx, -cap, cx, 0.0f, sw);
		poly_add_segment(pl, cx - w * 0.3f, -cap * 0.62f, cx, -cap, sw);
		poly_add_segment(pl, cx + w * 0.3f, -cap * 0.62f, cx, -cap, sw);
		*adv_out = (int)lrintf(w * 0.8f + pad * 2.0f);
		return true;
	}
	if (cp == 0x2193) { /* ↓ */
		float cx = pad + w * 0.4f;
		poly_add_segment(pl, cx, -cap, cx, 0.0f, sw);
		poly_add_segment(pl, cx - w * 0.3f, -cap * 0.38f, cx, 0.0f, sw);
		poly_add_segment(pl, cx + w * 0.3f, -cap * 0.38f, cx, 0.0f, sw);
		*adv_out = (int)lrintf(w * 0.8f + pad * 2.0f);
		return true;
	}
	if (cp == 0x25B6) { /* ▶ */
		float xs[3] = { pad, pad + w * 0.85f, pad };
		float ys[3] = { -cap * 0.88f, -cap * 0.48f, -cap * 0.08f };
		poly_add_contour(pl, xs, ys, 3);
		*adv_out = (int)lrintf(w + pad * 2.0f);
		return true;
	}
	if (cp == 0x00B7) { /* · */
		poly_add_segment(pl, pad + sw * 1.5f, -xh * 0.65f,
				 pad + sw * 1.5f, -xh * 0.65f, sw * 1.4f);
		*adv_out = (int)lrintf(pad * 2.0f + sw * 3.0f);
		return true;
	}
	if (cp == 0x2026) { /* … */
		float sp = h * 0.22f;
		poly_add_segment(pl, pad + sw, -sw, pad + sw, -sw, sw * 1.15f);
		poly_add_segment(pl, pad + sw + sp, -sw, pad + sw + sp, -sw, sw * 1.15f);
		poly_add_segment(pl, pad + sw + sp * 2.0f, -sw, pad + sw + sp * 2.0f, -sw, sw * 1.15f);
		*adv_out = (int)lrintf(pad * 2.0f + sp * 2.5f);
		return true;
	}
	if (cp == 0x00B0) { /* ° */
		float cx = pad + h * 0.16f, cy = -cap * 0.80f, r = h * 0.12f;
		int i;
		for (i = 0; i < 12; i++) {
			float a0 = 2.0f * (float)M_PI * i / 12.0f;
			float a1 = 2.0f * (float)M_PI * (i + 1) / 12.0f;
			poly_add_segment(pl, cx + r * cosf(a0), cy + r * sinf(a0),
					 cx + r * cosf(a1), cy + r * sinf(a1), sw * 0.75f);
		}
		*adv_out = (int)lrintf(pad * 2.0f + h * 0.34f);
		return true;
	}
	if (cp == 0x00D7) { /* × */
		float cy = -xh * 0.6f, s = xh * 0.38f;
		poly_add_segment(pl, pad, cy - s, pad + s * 2.0f, cy + s, sw);
		poly_add_segment(pl, pad, cy + s, pad + s * 2.0f, cy - s, sw);
		*adv_out = (int)lrintf(pad * 2.0f + s * 2.2f);
		return true;
	}
	return false;
}

static struct glyph_entry *rasterize_glyph(enum shell_theme_id theme, int size_px,
					   bool bold, bool italic, uint32_t cp)
{
	const struct ttf_font *f = &fonts[theme][bold ? 1 : 0];
	struct poly_list pl = { 0 };
	struct glyph_entry *ge;
	int adv_px = 0;
	bool used_special = false;
	float shear_x = italic ? 0.18f : 0.0f;

	if (!f->data && fonts[theme][0].data)
		f = &fonts[theme][0];

	if (cp == ' ') {
		ge = calloc(1, sizeof(*ge));
		if (!ge)
			return NULL;
		ge->codepoint = cp;
		ge->size_px = (uint16_t)size_px;
		ge->theme = (uint8_t)theme;
		ge->bold = bold ? 1 : 0;
		ge->italic = italic ? 1 : 0;
		ge->advance = (int16_t)(size_px * 28 / 100);
		if (f->data) {
			uint16_t glyph_id = ttf_find_glyph(f, ' ');
			uint16_t adv = 0;
			int16_t lsb = 0;
			ttf_get_hmetrics(f, glyph_id, &adv, &lsb);
			if (adv)
				ge->advance = (int16_t)lrintf((float)adv * size_px / f->units_per_em);
		}
		return ge;
	}

	uint16_t glyph_id = f->data ? ttf_find_glyph(f, cp) : 0;

	if (glyph_id != 0 && f->data) {
		float scale = (float)size_px / (float)f->units_per_em;
		uint16_t adv = 0;
		int16_t lsb = 0;

		ttf_get_hmetrics(f, glyph_id, &adv, &lsb);
		adv_px = (int)lrintf((float)adv * scale);
		ttf_extract_glyph_polys(f, glyph_id, scale, 1.0f, 0.0f, shear_x, 1.0f,
					0.0f, 0.0f, &pl, 0);
	} else {
		used_special = build_special_or_fallback_glyph(cp, size_px, bold, &pl, &adv_px);
		if (!used_special && f->data && cp > 127) {
			/* Map curly quotes/dashes to ASCII if missing in font */
			uint32_t ascii_cp = (cp == 0x2018 || cp == 0x2019) ? '\'' :
					    (cp == 0x201C || cp == 0x201D) ? '"' :
					    (cp == 0x2013 || cp == 0x2014) ? '-' : '?';
			glyph_id = ttf_find_glyph(f, ascii_cp);
			if (glyph_id) {
				float scale = (float)size_px / (float)f->units_per_em;
				uint16_t adv = 0;
				int16_t lsb = 0;
				ttf_get_hmetrics(f, glyph_id, &adv, &lsb);
				adv_px = (int)lrintf((float)adv * scale);
				ttf_extract_glyph_polys(f, glyph_id, scale, 1.0f, 0.0f, shear_x, 1.0f,
							0.0f, 0.0f, &pl, 0);
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
	ge->theme = (uint8_t)theme;
	ge->bold = bold ? 1 : 0;
	ge->italic = italic ? 1 : 0;
	ge->advance = (int16_t)(adv_px > 0 ? adv_px : (size_px / 2));

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

	/* Shift polygon points relative to (px0, py0) */
	for (i = 0; i < pl.npts; i++) {
		pl.pts[i].x -= px0 * SUB;
		pl.pts[i].y -= py0 * SUB;
	}

	uint16_t *row_cov = calloc((size_t)gw, sizeof(uint16_t));
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
					struct poly_pt b = pl.pts[pt_base + ((k + 1) % clen)];
					int32_t ylo = a.y < b.y ? a.y : b.y;
					int32_t yhi = a.y < b.y ? b.y : a.y;

					if (sy < ylo || sy >= yhi || ncr >= 128)
						continue;
					int32_t cx = a.x + (sy - a.y) * (b.x - a.x) / (b.y - a.y);
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
					int pa = xa / SUB;
					int pb = xb / SUB;

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
			uint32_t v = (row_cov[i] * 255 + (SUB * SAMPLES / 2)) / (SUB * SAMPLES);

			if (v > 255)
				v = 255;
			if (v > 0 && v < 255) {
				/*
				 * Stem-weight contrast curve (gamma ~1.3): preserves smooth
				 * subpixel anti-aliasing on thin strokes straddling pixel columns.
				 */
				v = (uint32_t)lrintf(powf((float)v / 255.0f, 0.78f) * 255.0f);
				if (v > 255)
					v = 255;
			}
			ge->bitmap[y * gw + i] = (uint8_t)v;
		}
	}

	free(row_cov);
	free(pl.pts);
	free(pl.contour_len);
	return ge;
}

static struct glyph_entry *get_glyph(enum shell_theme_id theme, int size_px,
				     bool bold, bool italic, uint32_t cp)
{
	uint32_t h = ((cp * 2654435761U) ^ ((uint32_t)size_px << 8) ^
		      ((uint32_t)theme << 2) ^ (bold ? 1U : 0U) ^ (italic ? 8U : 0U)) &
		     (GLYPH_HASH_BUCKETS - 1);
	struct glyph_entry *e = glyph_cache[h];

	while (e) {
		if (e->codepoint == cp && e->size_px == size_px &&
		    e->theme == theme && e->bold == (bold ? 1 : 0) &&
		    e->italic == (italic ? 1 : 0))
			return e;
		e = e->next;
	}
	e = rasterize_glyph(theme, size_px, bold, italic, cp);
	if (e) {
		e->next = glyph_cache[h];
		glyph_cache[h] = e;
	}
	return e;
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
	int t;

	for (t = 0; t < 2; t++) {
		char path_reg[256], path_bold[256];

		if (env_dir && env_dir[0]) {
			const char *b_reg = strrchr(shell_themes[t].font_regular, '/');
			const char *b_bold = strrchr(shell_themes[t].font_bold, '/');

			snprintf(path_reg, sizeof(path_reg), "%s/%s", env_dir,
				 b_reg ? b_reg + 1 : shell_themes[t].font_regular);
			snprintf(path_bold, sizeof(path_bold), "%s/%s", env_dir,
				 b_bold ? b_bold + 1 : shell_themes[t].font_bold);
		} else {
			snprintf(path_reg, sizeof(path_reg), "%s", shell_themes[t].font_regular);
			snprintf(path_bold, sizeof(path_bold), "%s", shell_themes[t].font_bold);
		}
		if (ttf_load_file(&fonts[t][0], path_reg) < 0 && (!env_dir || !env_dir[0])) {
			const char *b_reg = strrchr(shell_themes[t].font_regular, '/');

			snprintf(path_reg, sizeof(path_reg), "build/dl/fonts/%s",
				 b_reg ? b_reg + 1 : shell_themes[t].font_regular);
			(void)ttf_load_file(&fonts[t][0], path_reg);
		}
		if (ttf_load_file(&fonts[t][1], path_bold) < 0 && (!env_dir || !env_dir[0])) {
			const char *b_bold = strrchr(shell_themes[t].font_bold, '/');

			snprintf(path_bold, sizeof(path_bold), "build/dl/fonts/%s",
				 b_bold ? b_bold + 1 : shell_themes[t].font_bold);
			(void)ttf_load_file(&fonts[t][1], path_bold);
		}
	}

	/* Pre-warm ASCII glyphs for the 10 unique sizes in tokens.json */
	static const int sizes[] = { 16, 18, 20, 22, 24, 26, 28, 34, 44, 64 };
	size_t si;
	uint32_t cp;

	for (t = 0; t < 2; t++) {
		for (si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
			for (cp = 32; cp <= 126; cp++)
				(void)get_glyph((enum shell_theme_id)t, sizes[si], false, false, cp);
		}
	}
	return 0;
}

void shell_font_free(void)
{
	int i, t, b;

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
	for (t = 0; t < 2; t++)
		for (b = 0; b < 2; b++)
			free(fonts[t][b].data);
	memset(fonts, 0, sizeof(fonts));
}

int shell_font_measure(enum shell_theme_id theme, int size_px, bool bold, const char *utf8)
{
	const char *p = utf8;
	int w = 0;
	uint32_t cp;

	if (!p)
		return 0;
	while ((cp = utf8_next(&p)) != 0) {
		struct glyph_entry *ge = get_glyph(theme, size_px, bold, false, cp);

		if (ge)
			w += ge->advance;
	}
	return w;
}

static void blit_glyph(const struct glyph_entry *ge, int pen_x, int baseline_y,
		       uint32_t fg)
{
	uint32_t *fb = shell_display_backbuffer();
	int py, px;

	if (!ge || !ge->bitmap || !fb)
		return;

	for (py = 0; py < (int)ge->h; py++) {
		int rel_y = ge->yoff + py;
		int sy = baseline_y + rel_y;

		if (sy < 0 || sy >= SHELL_SCREEN_H)
			continue;
		uint32_t *row = fb + sy * SHELL_SCREEN_W;

		for (px = 0; px < (int)ge->w; px++) {
			uint8_t a255 = ge->bitmap[py * ge->w + px];
			int sx;

			if (!a255)
				continue;
			sx = pen_x + ge->xoff + px;
			if (sx < 0 || sx >= SHELL_SCREEN_W)
				continue;
			if (a255 == 255) {
				row[sx] = fg | 0xff000000U;
			} else {
				uint32_t a = a255;
				uint32_t inv_a = 255U - a;
				uint32_t dst = row[sx];
				uint32_t r = (((dst >> 16) & 0xffU) * inv_a + ((fg >> 16) & 0xffU) * a + 127U) / 255U;
				uint32_t g = (((dst >> 8) & 0xffU) * inv_a + ((fg >> 8) & 0xffU) * a + 127U) / 255U;
				uint32_t b = ((dst & 0xffU) * inv_a + (fg & 0xffU) * a + 127U) / 255U;

				row[sx] = 0xff000000U | (r << 16) | (g << 8) | b;
			}
		}
	}
}

int shell_font_draw(enum shell_theme_id theme, int size_px, bool bold, bool italic,
		    int x, int y, uint32_t fg, const char *utf8, int max_width)
{
	const char *p = utf8;
	int baseline_y = y + (size_px * 78) / 100;
	int pen_x = x;
	int ell_w = 0;
	bool need_ellipsis = false;

	if (!utf8 || !utf8[0])
		return 0;

	if (max_width > 0 && shell_font_measure(theme, size_px, bold, utf8) > max_width) {
		struct glyph_entry *ell = get_glyph(theme, size_px, bold, italic, 0x2026);

		ell_w = ell ? ell->advance : size_px / 2;
		need_ellipsis = true;
	}

	uint32_t cp;

	while ((cp = utf8_next(&p)) != 0) {
		struct glyph_entry *ge = get_glyph(theme, size_px, bold, italic, cp);

		if (!ge)
			continue;
		if (need_ellipsis && (pen_x - x) + ge->advance + ell_w > max_width) {
			struct glyph_entry *ell = get_glyph(theme, size_px, bold, italic, 0x2026);

			if (ell) {
				blit_glyph(ell, pen_x, baseline_y, fg);
				pen_x += ell->advance;
			}
			break;
		}
		blit_glyph(ge, pen_x, baseline_y, fg);
		pen_x += ge->advance;
	}

	shell_display_damage(x - 4, y - 4, (pen_x - x) + 16, size_px + 12);
	return pen_x - x;
}

int shell_font_draw_wrapped(enum shell_theme_id theme, int size_px, bool bold,
			    int x, int y, int line_height, int max_width, int max_lines,
			    uint32_t fg, const char *utf8)
{
	char line[256];
	const char *p = utf8;
	int lines_drawn = 0;

	if (!utf8 || !utf8[0])
		return 0;

	while (*p && lines_drawn < max_lines) {
		while (*p == ' ')
			p++;
		if (!*p)
			break;

		/* Find how many words fit on this line */
		const char *scan = p;
		const char *last_fit = p;
		line[0] = '\0';

		while (*scan && *scan != '\n') {
			const char *word_end = scan;

			while (*word_end && *word_end != ' ' && *word_end != '\n')
				word_end++;

			size_t candidate_len = (size_t)(word_end - p);
			if (candidate_len >= sizeof(line))
				break;

			memcpy(line, p, candidate_len);
			line[candidate_len] = '\0';

			if (shell_font_measure(theme, size_px, bold, line) <= max_width ||
			    last_fit == p) {
				last_fit = word_end;
				if (shell_font_measure(theme, size_px, bold, line) > max_width)
					break;
			} else {
				break;
			}

			scan = word_end;
			while (*scan == ' ')
				scan++;
		}

		size_t final_len = (size_t)(last_fit - p);
		if (final_len >= sizeof(line))
			final_len = sizeof(line) - 1;
		memcpy(line, p, final_len);
		line[final_len] = '\0';

		/* If this is the last allowed line and more text remains, truncate with ellipsis */
		const char *rem = last_fit;
		while (*rem == ' ' || *rem == '\n')
			rem++;
		if (lines_drawn + 1 == max_lines && *rem != '\0') {
			size_t l = strlen(line);
			if (l + 4 < sizeof(line)) {
				memcpy(line + l, "\xe2\x80\xa6", 4);
			}
		}

		shell_font_draw(theme, size_px, bold, false,
				x, y + lines_drawn * line_height, fg, line, max_width);
		lines_drawn++;

		p = last_fit;
		if (*p == '\n')
			p++;
	}

	return lines_drawn * line_height;
}
