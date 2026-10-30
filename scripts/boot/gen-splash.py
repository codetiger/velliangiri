#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
Generate the pre-kernel boot splash as C polygon tables.

Reads the project logo (a small SVG subset: path with absolute M/L/H/V/C/Q/Z,
polygon, two-stop linearGradient fills, opacity and round-capped strokes),
flattens it into contours laid out for the 1280x720 scanout, and adds the
"Velliangiri" wordmark below it drawn with a built-in geometric stroke font.

The loader has no runtime division, so every edge carries a precomputed
16.16 dx/dy slope. Coordinates are in 1/16 pixel units.

Usage: gen-splash.py logo.svg > splash-data.h
"""

import math
import re
import sys
import xml.etree.ElementTree as ET

FB_W, FB_H = 1280, 720
SUB = 16                    # subpixel units per pixel
LOGO_H = 150                # rendered logo content height in pixels
GAP = 24                    # logo bottom to text cap line, pixels
CAP_H = 42                  # wordmark cap height, pixels
FLAT_TOL = 0.2              # curve flattening tolerance, pixels

BG = 0xfef3c7
TEXT_COLOR = 0x1e293b
WORDMARK = "Velliangiri"

NS = "{http://www.w3.org/2000/svg}"


def parse_color(s):
    s = s.strip()
    if not re.fullmatch(r"#[0-9a-fA-F]{6}", s):
        sys.exit(f"gen-splash: unsupported color '{s}'")
    return int(s[1:], 16)


def parse_pct(s):
    s = s.strip()
    return float(s[:-1]) / 100.0 if s.endswith("%") else float(s)


# ---------------------------------------------------------------- geometry

def flatten_cubic(p0, p1, p2, p3, scale):
    est = (math.dist(p0, p1) + math.dist(p1, p2) + math.dist(p2, p3)) * scale
    n = max(2, math.ceil(math.sqrt(est / FLAT_TOL) / 2))
    out = []
    for i in range(1, n + 1):
        t = i / n
        u = 1 - t
        out.append((u**3 * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t**3 * p3[0],
                    u**3 * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t**3 * p3[1]))
    return out


def flatten_quad(p0, p1, p2, scale):
    c1 = (p0[0] + 2 / 3 * (p1[0] - p0[0]), p0[1] + 2 / 3 * (p1[1] - p0[1]))
    c2 = (p2[0] + 2 / 3 * (p1[0] - p2[0]), p2[1] + 2 / 3 * (p1[1] - p2[1]))
    return flatten_cubic(p0, c1, c2, p2, scale)


def parse_path(d, scale):
    """Return [(points, closed), ...] in SVG user units."""
    toks = re.findall(r"[A-Za-z]|-?\d*\.?\d+(?:[eE][-+]?\d+)?", d)
    i = 0
    cmd = None
    cur = (0.0, 0.0)
    contours, poly = [], []

    def num():
        nonlocal i
        v = float(toks[i])
        i += 1
        return v

    while i < len(toks):
        if re.fullmatch(r"[A-Za-z]", toks[i]):
            cmd = toks[i]
            i += 1
        if cmd == "M":
            if poly:
                contours.append((poly, False))
            cur = (num(), num())
            poly = [cur]
            cmd = "L"
        elif cmd == "L":
            cur = (num(), num())
            poly.append(cur)
        elif cmd == "H":
            cur = (num(), cur[1])
            poly.append(cur)
        elif cmd == "V":
            cur = (cur[0], num())
            poly.append(cur)
        elif cmd == "C":
            p1, p2, p3 = (num(), num()), (num(), num()), (num(), num())
            poly += flatten_cubic(cur, p1, p2, p3, scale)
            cur = p3
        elif cmd == "Q":
            p1, p2 = (num(), num()), (num(), num())
            poly += flatten_quad(cur, p1, p2, scale)
            cur = p2
        elif cmd in "Zz":
            if poly:
                contours.append((poly, True))
                cur = poly[0]
                poly = []
            cmd = None
        else:
            sys.exit(f"gen-splash: unsupported path command '{cmd}'")
    if poly:
        contours.append((poly, False))
    return contours


def arc_pts(c, r, a0, a1, step_px):
    """Points along a circle arc from angle a0 to a1 (radians), inclusive."""
    n = max(2, math.ceil(abs(a1 - a0) * r / step_px))
    return [(c[0] + r * math.cos(a0 + (a1 - a0) * k / n),
             c[1] + r * math.sin(a0 + (a1 - a0) * k / n)) for k in range(n + 1)]


def capsule(p0, p1, r, step):
    """Round-capped thick segment as one closed contour."""
    a = math.atan2(p1[1] - p0[1], p1[0] - p0[0])
    pts = arc_pts(p1, r, a - math.pi / 2, a + math.pi / 2, step)
    pts += arc_pts(p0, r, a + math.pi / 2, a + 3 * math.pi / 2, step)
    return pts


def arc_band(c, rad, a0, a1, r, step):
    """Round-capped thick circular arc as one closed contour."""
    outer = arc_pts(c, rad + r, a0, a1, step)
    inner = arc_pts(c, rad - r, a1, a0, step)
    e1 = (c[0] + rad * math.cos(a1), c[1] + rad * math.sin(a1))
    e0 = (c[0] + rad * math.cos(a0), c[1] + rad * math.sin(a0))
    s = 1 if a1 > a0 else -1
    cap1 = arc_pts(e1, r, a1, a1 + s * math.pi, step)[1:-1]
    cap0 = arc_pts(e0, r, a0 + s * math.pi, a0 + s * 2 * math.pi, step)[1:-1]
    return outer + cap1 + inner + cap0


def signed_area(pts):
    return sum(pts[k][0] * pts[k - 1][1] - pts[k - 1][0] * pts[k][1]
               for k in range(len(pts))) / 2


def orient(pts):
    """Same winding for all contours so nonzero fill unions them."""
    return pts if signed_area(pts) > 0 else pts[::-1]


def stroke_polyline(pts, r, step):
    return [orient(capsule(pts[k], pts[k + 1], r, step)) for k in range(len(pts) - 1)]


# --------------------------------------------------------------- wordmark
#
# Geometric stroke font, y up, baseline 0, x-height 50, cap height 70.
# Each glyph: (advance, [primitives]); primitives are
#   ("l", [(x, y), ...])            polyline
#   ("a", cx, cy, r, deg0, deg1)     arc, counter-clockwise when deg1 > deg0
#   ("d", x, y)                     dot

FONT_CAP = 70.0
FONT_STROKE = 10.0
FONT_SPACING = 18.0

GLYPHS = {
    "V": (48, [("l", [(0, 70), (24, 0), (48, 70)])]),
    "e": (50, [("l", [(0, 25), (50, 25)]), ("a", 25, 25, 25, 0, 315)]),
    "l": (0, [("l", [(0, 0), (0, 74)])]),
    "i": (0, [("l", [(0, 0), (0, 50)]), ("d", 0, 66)]),
    "a": (50, [("a", 25, 25, 25, 0, 360), ("l", [(50, 0), (50, 50)])]),
    "n": (40, [("l", [(0, 0), (0, 50)]), ("a", 20, 30, 20, 180, 0), ("l", [(40, 30), (40, 0)])]),
    "g": (50, [("a", 25, 25, 25, 0, 360), ("l", [(50, 50), (50, -10)]),
               ("a", 25, -10, 25, 0, -150)]),
    "r": (30, [("l", [(0, 0), (0, 50)]), ("a", 24, 26, 24, 180, 75)]),
}


def wordmark_contours(text, top_px):
    s = CAP_H / FONT_CAP
    r = FONT_STROKE / 2 * s
    step = 1.0
    width = sum(GLYPHS[ch][0] for ch in text) + FONT_SPACING * (len(text) - 1)
    x0 = (FB_W - width * s) / 2
    base = top_px + CAP_H
    pen = 0.0
    out = []

    def P(x, y):
        return (x0 + (pen + x) * s, base - y * s)

    for ch in text:
        adv, prims = GLYPHS[ch]
        for p in prims:
            if p[0] == "l":
                out += stroke_polyline([P(*q) for q in p[1]], r, step)
            elif p[0] == "d":
                c = P(p[1], p[2])
                out.append(orient(arc_pts(c, r * 1.15, 0, 2 * math.pi, step)[:-1]))
            else:
                _, cx, cy, rad, d0, d1 = p
                # y flips to screen space, so angles negate
                c = P(cx, cy)
                out.append(orient(arc_band(c, rad * s, -math.radians(d0),
                                           -math.radians(d1), r, step)))
        pen += adv + FONT_SPACING
    return out


# ------------------------------------------------------------------- logo

def load_logo(path):
    root = ET.parse(path).getroot()
    grads = {}
    for g in root.iter(NS + "linearGradient"):
        stops = g.findall(NS + "stop")
        if len(stops) != 2:
            sys.exit("gen-splash: only two-stop gradients are supported")
        if g.get("gradientUnits", "objectBoundingBox") != "objectBoundingBox":
            sys.exit("gen-splash: only objectBoundingBox gradients are supported")
        grads[g.get("id")] = (
            tuple(parse_pct(g.get(k, d)) for k, d in
                  (("x1", "0%"), ("y1", "0%"), ("x2", "100%"), ("y2", "0%"))),
            [(parse_pct(st.get("offset", "0")), parse_color(st.get("stop-color")))
             for st in stops])

    items = []
    for el in root.iter():
        tag = el.tag.replace(NS, "")
        if tag == "path":
            contours = parse_path(el.get("d"), 1.0)
        elif tag == "polygon":
            v = [float(t) for t in re.split(r"[\s,]+", el.get("points").strip())]
            contours = [(list(zip(v[0::2], v[1::2])), True)]
        else:
            continue
        items.append((el, contours))
    return grads, items


def build(svg_path):
    grads, items = load_logo(svg_path)

    # first pass in SVG units to find the content box
    allpts = [p for _, cs in items for pts, _ in cs for p in pts]
    minx = min(p[0] for p in allpts)
    maxx = max(p[0] for p in allpts)
    miny = min(p[1] for p in allpts)
    maxy = max(p[1] for p in allpts)
    scale = LOGO_H / (maxy - miny)
    total_h = LOGO_H + GAP + CAP_H
    top = (FB_H - total_h) / 2
    ox = (FB_W - (maxx - minx) * scale) / 2 - minx * scale
    oy = top - miny * scale

    def X(p):
        return (p[0] * scale + ox, p[1] * scale + oy)

    shapes = []
    for el, _ in items:
        # re-flatten at final scale for tolerance
        if el.tag == NS + "path":
            contours = parse_path(el.get("d"), scale)
        else:
            v = [float(t) for t in re.split(r"[\s,]+", el.get("points").strip())]
            contours = [(list(zip(v[0::2], v[1::2])), True)]
        opacity = float(el.get("opacity", "1"))

        fill = el.get("fill", "#000000")
        if fill != "none":
            polys = [[X(p) for p in pts] for pts, _ in contours]
            shapes.append((polys, paint(fill, grads, contours), opacity))

        stroke = el.get("stroke")
        if stroke and stroke != "none":
            if el.get("stroke-linecap") != "round":
                sys.exit("gen-splash: only round-capped strokes are supported")
            r = float(el.get("stroke-width", "1")) * scale / 2
            polys = []
            for pts, closed in contours:
                line = [X(p) for p in pts] + ([X(pts[0])] if closed else [])
                polys += stroke_polyline(line, r, 0.5)
            shapes.append((polys, ("solid", parse_color(stroke)),
                           opacity * float(el.get("stroke-opacity", "1"))))

    def to_px_paint(pnt):
        if pnt[0] == "solid":
            return pnt
        _, (bx, by, bw, bh), (x1, y1, x2, y2), c0, c1 = pnt
        # t(u, v) over the normalized bbox, mapped back to screen pixels
        dx, dy = x2 - x1, y2 - y1
        dd = dx * dx + dy * dy
        sbx, sby = X((bx, by))
        sw, sh = bw * scale, bh * scale
        a = dx / dd / sw
        b = dy / dd / sh
        c = (-(sbx / sw) - x1) * dx / dd + (-(sby / sh) - y1) * dy / dd
        return ("grad", a, b, c, c0, c1)

    shapes = [(p, to_px_paint(pt), o) for p, pt, o in shapes]
    shapes.append((wordmark_contours(WORDMARK, top + LOGO_H + GAP),
                   ("solid", TEXT_COLOR), 1.0))
    return shapes


def paint(fill, grads, contours):
    m = re.fullmatch(r"url\(#([^)]+)\)", fill)
    if not m:
        return ("solid", parse_color(fill))
    (x1, y1, x2, y2), stops = grads[m.group(1)]
    pts = [p for c, _ in contours for p in c]
    bx, by = min(p[0] for p in pts), min(p[1] for p in pts)
    bw, bh = max(p[0] for p in pts) - bx, max(p[1] for p in pts) - by
    # fold stop offsets into the gradient vector
    (o0, c0), (o1, c1) = stops
    sx1, sy1 = x1 + (x2 - x1) * o0, y1 + (y2 - y1) * o0
    sx2, sy2 = x1 + (x2 - x1) * o1, y1 + (y2 - y1) * o1
    return ("grad", (bx, by, bw, bh), (sx1, sy1, sx2, sy2), c0, c1)


# ------------------------------------------------------------------ emit

def srgb_to_linear(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def linear_to_srgb(c):
    return c * 12.92 if c <= 0.0031308 else 1.055 * c ** (1 / 2.4) - 0.055


def emit(shapes, out):
    vtx, clen, recs = [], [], []
    for polys, pnt, opacity in shapes:
        first_c = len(clen)
        xs, ys = [], []
        for pts in polys:
            q = [(round(x * SUB), round(y * SUB)) for x, y in pts]
            q = [p for k, p in enumerate(q) if p != q[k - 1]]
            if len(q) < 3:
                continue
            for k, (x0, y0) in enumerate(q):
                x1, y1 = q[(k + 1) % len(q)]
                slope = 0 if y1 == y0 else round((x1 - x0) * 65536 / (y1 - y0))
                vtx.append((x0, y0, slope))
                xs.append(x0)
                ys.append(y0)
            clen.append(len(q))
        if pnt[0] == "solid":
            ga = gb = gc = 0
            c0 = c1 = pnt[1]
        else:
            _, a, b, c, c0, c1 = pnt
            # evaluated at pixel centres
            ga = round(a * 65536)
            gb = round(b * 65536)
            gc = round((c + (a + b) * 0.5) * 65536)
        recs.append((first_c, len(clen) - first_c,
                     min(xs) // SUB, min(ys) // SUB,
                     min(FB_W - 1, max(xs) // SUB), min(FB_H - 1, max(ys) // SUB),
                     round(opacity * 256), ga, gb, gc, c0, c1))

    w = out.write
    w("/* SPDX-License-Identifier: GPL-2.0-only */\n")
    w("/* Generated by scripts/boot/gen-splash.py - do not edit. */\n\n")
    w(f"#define SPLASH_BG\t0x{BG:06x}U\n")
    w(f"#define SPLASH_NSHAPES\t{len(recs)}\n\n")
    w("static const struct splash_vtx splash_vtx[] = {\n")
    for x, y, s in vtx:
        w(f"\t{{ {x}, {y}, {s} }},\n")
    w("};\n\n")
    w("static const uint16_t splash_contour_len[] = {\n")
    for k in range(0, len(clen), 12):
        w("\t" + " ".join(f"{n}," for n in clen[k:k + 12]) + "\n")
    w("};\n\n")
    w("static const struct splash_shape splash_shapes[SPLASH_NSHAPES] = {\n")
    for r in recs:
        w("\t{ %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, 0x%06xU, 0x%06xU },\n" % r)
    w("};\n\n")
    # anti-aliased edges are blended in linear light
    w("static const uint16_t splash_to_linear[256] = {\n")
    lin = [round(srgb_to_linear(k / 255) * 65535) for k in range(256)]
    for k in range(0, 256, 8):
        w("\t" + " ".join(f"{v}," for v in lin[k:k + 8]) + "\n")
    w("};\n\n")
    w("static const uint8_t splash_to_srgb[1024] = {\n")
    srgb = [round(linear_to_srgb(k / 1023) * 255) for k in range(1024)]
    for k in range(0, 1024, 16):
        w("\t" + " ".join(f"{v}," for v in srgb[k:k + 16]) + "\n")
    w("};\n")
    print(f"gen-splash: {len(recs)} shapes, {len(clen)} contours, {len(vtx)} vertices",
          file=sys.stderr)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip().splitlines()[-1])
    emit(build(sys.argv[1]), sys.stdout)
