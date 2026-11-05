#!/usr/bin/env -S python3 -S
# SPDX-License-Identifier: GPL-2.0-only
"""
Generate Giri on Velliangiri OS (v1.2 TV Design System) C headers:
  - shell_tokens.h  (from tools/tvshell/design/tokens.json)
  - shell_assets.h  (from tools/tvshell/design/{logo/*.svg,symbols/*.svg})

Usage: gen-shell-assets.py <design-dir> <out-dir>
"""

import json
import math
import os
import re
import struct
import sys
import xml.etree.ElementTree as ET

SUB = 16
SAMPLES = 16
NS = "{http://www.w3.org/2000/svg}"


def parse_hex_color(s: str) -> int:
    s = s.strip()
    if not re.fullmatch(r"#[0-9a-fA-F]{6}", s):
        sys.exit(f"gen-shell-assets: invalid hex color '{s}'")
    return 0xFF000000 | int(s[1:], 16)


def parse_rgba_or_hex(s: str) -> tuple[int, int]:
    """Return (0xFFRRGGBB, alpha_0_to_255)."""
    s = s.strip()
    if s.startswith("#"):
        return parse_hex_color(s), 255
    m = re.fullmatch(
        r"rgba\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*([0-9.]+)\s*\)", s
    )
    if not m:
        sys.exit(f"gen-shell-assets: invalid color '{s}'")
    r, g, b = int(m.group(1)), int(m.group(2)), int(m.group(3))
    a = max(0, min(255, round(float(m.group(4)) * 255.0)))
    return (0xFF000000 | (r << 16) | (g << 8) | b), a


def srgb_to_linear(c: float) -> float:
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def linear_to_srgb(l: float) -> float:
    if l <= 0.0:
        return 0.0
    if l >= 1.0:
        return 1.0
    return l * 12.92 if l <= 0.0031308 else 1.055 * (l ** (1.0 / 2.4)) - 0.055


TO_LIN = [round(srgb_to_linear(k / 255.0) * 4095.0) for k in range(256)]
TO_SRGB = [round(linear_to_srgb(k / 4095.0) * 255.0) for k in range(4096)]
TO_LIN_F = [srgb_to_linear(k / 255.0) for k in range(256)]


def flatten_cubic(p0, p1, p2, p3, steps=48):
    out = []
    for i in range(1, steps + 1):
        t = i / steps
        u = 1.0 - t
        x = (
            (u**3) * p0[0]
            + 3 * u * u * t * p1[0]
            + 3 * u * t * t * p2[0]
            + (t**3) * p3[0]
        )
        y = (
            (u**3) * p0[1]
            + 3 * u * u * t * p1[1]
            + 3 * u * t * t * p2[1]
            + (t**3) * p3[1]
        )
        out.append((x, y))
    return out


def flatten_quad(p0, p1, p2, steps=48):
    out = []
    for i in range(1, steps + 1):
        t = i / steps
        u = 1.0 - t
        x = u * u * p0[0] + 2 * u * t * p1[0] + t * t * p2[0]
        y = u * u * p0[1] + 2 * u * t * p1[1] + t * t * p2[1]
        out.append((x, y))
    return out


def flatten_svg_arc(p0, rx, ry, x_axis_rot_deg, large_arc_flag, sweep_flag, p1, steps=20):
    """W3C SVG F.6.5 endpoint-to-center elliptical arc parameterization."""
    x1, y1 = p0
    x2, y2 = p1
    if abs(x1 - x2) < 1e-7 and abs(y1 - y2) < 1e-7:
        return []
    rx = abs(rx)
    ry = abs(ry)
    if rx < 1e-7 or ry < 1e-7:
        return [p1]

    phi = math.radians(x_axis_rot_deg % 360.0)
    cos_phi = math.cos(phi)
    sin_phi = math.sin(phi)

    dx2 = (x1 - x2) / 2.0
    dy2 = (y1 - y2) / 2.0
    x1p = cos_phi * dx2 + sin_phi * dy2
    y1p = -sin_phi * dx2 + cos_phi * dy2

    lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry)
    if lam > 1.0:
        sq = math.sqrt(lam)
        rx *= sq
        ry *= sq

    num = max(0.0, rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p)
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p
    rad = math.sqrt(num / den) if den > 1e-12 else 0.0
    if bool(large_arc_flag) == bool(sweep_flag):
        rad = -rad

    cxp = rad * (rx * y1p / ry)
    cyp = rad * (-ry * x1p / rx)

    cx = cos_phi * cxp - sin_phi * cyp + (x1 + x2) / 2.0
    cy = sin_phi * cxp + cos_phi * cyp + (y1 + y2) / 2.0

    def vec_angle(ux, uy, vx, vy):
        dot = ux * vx + uy * vy
        mag = math.hypot(ux, uy) * math.hypot(vx, vy)
        if mag < 1e-12:
            return 0.0
        c = max(-1.0, min(1.0, dot / mag))
        ang = math.acos(c)
        return -ang if (ux * vy - uy * vx) < 0.0 else ang

    vx1 = (x1p - cxp) / rx
    vy1 = (y1p - cyp) / ry
    vx2 = (-x1p - cxp) / rx
    vy2 = (-y1p - cyp) / ry

    theta1 = vec_angle(1.0, 0.0, vx1, vy1)
    dtheta = vec_angle(vx1, vy1, vx2, vy2)
    if not sweep_flag and dtheta > 0:
        dtheta -= 2.0 * math.pi
    elif sweep_flag and dtheta < 0:
        dtheta += 2.0 * math.pi

    out = []
    for i in range(1, steps + 1):
        t = theta1 + dtheta * (i / steps)
        ct = math.cos(t)
        st = math.sin(t)
        x = cos_phi * rx * ct - sin_phi * ry * st + cx
        y = sin_phi * rx * ct + cos_phi * ry * st + cy
        out.append((x, y))
    return out


def arc_pts(c, r, a0, a1, steps=16):
    return [
        (
            c[0] + r * math.cos(a0 + (a1 - a0) * k / steps),
            c[1] + r * math.sin(a0 + (a1 - a0) * k / steps),
        )
        for k in range(steps + 1)
    ]


def signed_area(pts):
    return (
        sum(
            pts[k][0] * pts[k - 1][1] - pts[k - 1][0] * pts[k][1]
            for k in range(len(pts))
        )
        / 2.0
    )


def orient(pts):
    if len(pts) < 3:
        return pts
    return pts if signed_area(pts) > 0 else pts[::-1]


def capsule(p0, p1, r):
    dx, dy = p1[0] - p0[0], p1[1] - p0[1]
    if abs(dx) < 1e-6 and abs(dy) < 1e-6:
        return orient(arc_pts(p0, r, 0.0, 2.0 * math.pi, 16)[:-1])
    a = math.atan2(dy, dx)
    pts = arc_pts(p1, r, a - math.pi / 2.0, a + math.pi / 2.0, 10)
    pts += arc_pts(p0, r, a + math.pi / 2.0, a + 3.0 * math.pi / 2.0, 10)
    return orient(pts)


def stroke_polyline(pts, closed, r):
    line = list(pts) + ([pts[0]] if closed and len(pts) > 1 else [])
    polys = []
    for k in range(len(line) - 1):
        polys.append(capsule(line[k], line[k + 1], r))
    return polys


def tokenize_svg_path(d: str):
    """Tokenize SVG path data, properly splitting single-character arc flags."""
    raw = re.findall(r"[A-Za-z]|[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?", d)
    return raw


def parse_svg_path(d: str):
    toks = tokenize_svg_path(d)
    i = 0
    cmd = None
    cur = (0.0, 0.0)
    start = (0.0, 0.0)
    contours = []
    poly = []

    def num():
        nonlocal i
        v = float(toks[i])
        i += 1
        return v

    def flag():
        nonlocal i
        tok = toks[i]
        if tok in ("0", "1"):
            i += 1
            return int(tok)
        if tok[0] in ("0", "1") and len(tok) > 1:
            f = int(tok[0])
            toks[i] = tok[1:]
            return f
        v = int(float(tok))
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
            start = cur
            poly = [cur]
            cmd = "L"
        elif cmd == "m":
            if poly:
                contours.append((poly, False))
            cur = (cur[0] + num(), cur[1] + num())
            start = cur
            poly = [cur]
            cmd = "l"
        elif cmd == "L":
            cur = (num(), num())
            poly.append(cur)
        elif cmd == "l":
            cur = (cur[0] + num(), cur[1] + num())
            poly.append(cur)
        elif cmd == "H":
            cur = (num(), cur[1])
            poly.append(cur)
        elif cmd == "h":
            cur = (cur[0] + num(), cur[1])
            poly.append(cur)
        elif cmd == "V":
            cur = (cur[0], num())
            poly.append(cur)
        elif cmd == "v":
            cur = (cur[0], cur[1] + num())
            poly.append(cur)
        elif cmd == "C":
            p1 = (num(), num())
            p2 = (num(), num())
            p3 = (num(), num())
            poly.extend(flatten_cubic(cur, p1, p2, p3))
            cur = p3
        elif cmd == "c":
            p1 = (cur[0] + num(), cur[1] + num())
            p2 = (cur[0] + num(), cur[1] + num())
            p3 = (cur[0] + num(), cur[1] + num())
            poly.extend(flatten_cubic(cur, p1, p2, p3))
            cur = p3
        elif cmd == "Q":
            p1 = (num(), num())
            p2 = (num(), num())
            poly.extend(flatten_quad(cur, p1, p2))
            cur = p2
        elif cmd == "q":
            p1 = (cur[0] + num(), cur[1] + num())
            p2 = (cur[0] + num(), cur[1] + num())
            poly.extend(flatten_quad(cur, p1, p2))
            cur = p2
        elif cmd == "A":
            rx = num()
            ry = num()
            rot = num()
            la = flag()
            sw = flag()
            p1 = (num(), num())
            poly.extend(flatten_svg_arc(cur, rx, ry, rot, la, sw, p1))
            cur = p1
        elif cmd == "a":
            rx = num()
            ry = num()
            rot = num()
            la = flag()
            sw = flag()
            p1 = (cur[0] + num(), cur[1] + num())
            poly.extend(flatten_svg_arc(cur, rx, ry, rot, la, sw, p1))
            cur = p1
        elif cmd in ("Z", "z"):
            if poly:
                contours.append((poly, True))
                cur = start
                poly = []
            cmd = None
        else:
            sys.exit(f"gen-shell-assets: unsupported SVG path command '{cmd}'")
    if poly:
        contours.append((poly, False))
    return contours


def rect_contour(x, y, w, h, rx):
    if rx <= 0:
        return [([(x, y), (x + w, y), (x + w, y + h), (x, y + h)], True)]
    rx = min(rx, w / 2.0, h / 2.0)
    pts = []
    pts.extend(arc_pts((x + w - rx, y + rx), rx, -math.pi / 2.0, 0.0, 8))
    pts.extend(arc_pts((x + w - rx, y + h - rx), rx, 0.0, math.pi / 2.0, 8))
    pts.extend(arc_pts((x + rx, y + h - rx), rx, math.pi / 2.0, math.pi, 8))
    pts.extend(arc_pts((x + rx, y + rx), rx, math.pi, 1.5 * math.pi, 8))
    return [(pts, True)]


def rasterize_polygons(polys, width, height):
    """Return width*height coverage array in 0..256 using 16x16 supersampling."""
    edges_by_poly = []
    all_pts = []
    for pts in polys:
        q = [(round(x * SUB), round(y * SUB)) for x, y in pts]
        q = [p for k, p in enumerate(q) if p != q[k - 1]]
        if len(q) >= 3:
            edges_by_poly.append(q)
            all_pts.extend(q)
    cov_img = [[0] * width for _ in range(height)]
    if not edges_by_poly:
        return cov_img

    y0 = max(0, min(p[1] for p in all_pts) // SUB)
    y1 = min(height - 1, max(p[1] for p in all_pts) // SUB)
    x0 = max(0, min(p[0] for p in all_pts) // SUB)
    x1 = min(width - 1, max(p[0] for p in all_pts) // SUB)

    for y in range(y0, y1 + 1):
        row_cov = [0] * width
        for s in range(SAMPLES):
            sy = y * SUB + s * (SUB // SAMPLES) + (SUB // SAMPLES) // 2
            crossings = []
            for q in edges_by_poly:
                n = len(q)
                for k in range(n):
                    ax, ay = q[k]
                    bx, by = q[(k + 1) % n]
                    ylo, yhi = (ay, by) if ay < by else (by, ay)
                    if sy < ylo or sy >= yhi:
                        continue
                    cx = ax + ((sy - ay) * (bx - ax)) // (by - ay)
                    direction = 1 if by > ay else -1
                    crossings.append((cx, direction))
            crossings.sort(key=lambda item: item[0])
            wind = 0
            for k in range(len(crossings) - 1):
                wind += crossings[k][1]
                if wind != 0:
                    xa = max(x0 * SUB, crossings[k][0])
                    xb = min((x1 + 1) * SUB, crossings[k + 1][0])
                    if xa >= xb:
                        continue
                    pa = xa // SUB
                    pb = xb // SUB
                    if pa == pb:
                        row_cov[pa] += xb - xa
                    else:
                        row_cov[pa] += SUB - (xa & (SUB - 1))
                        for p in range(pa + 1, pb):
                            row_cov[p] += SUB
                        if pb <= x1:
                            row_cov[pb] += xb & (SUB - 1)
        for x in range(x0, x1 + 1):
            cov_img[y][x] = min(256, row_cov[x])
    return cov_img


def parse_pct(s: str, default: float) -> float:
    if not s:
        return default
    s = s.strip()
    return float(s[:-1]) / 100.0 if s.endswith("%") else float(s)


def parse_transform(transform_str: str):
    """
    Parse SVG transform string supporting:
      translate(tx, ty) scale(s) translate(-tx, -ty)
    Returns a point transform function T(pt) -> (x, y) and scalar scale factor.
    """
    if not transform_str:
        return (lambda p: p), 1.0
    ops = re.findall(r"([a-zA-Z]+)\s*\(([^)]*)\)", transform_str)
    # Build 2D affine matrix [a, c, e; b, d, f]
    a, b, c, d, e, f = 1.0, 0.0, 0.0, 1.0, 0.0, 0.0

    def mat_mul(m1, m2):
        a1, b1, c1, d1, e1, f1 = m1
        a2, b2, c2, d2, e2, f2 = m2
        return (
            a1 * a2 + c1 * b2,
            b1 * a2 + d1 * b2,
            a1 * c2 + c1 * d2,
            b1 * c2 + d1 * d2,
            a1 * e2 + c1 * f2 + e1,
            b1 * e2 + d1 * f2 + f1,
        )

    for name, args_s in ops:
        vals = [float(v) for v in re.findall(r"[-+]?(?:\d+\.?\d*|\.\d+)", args_s)]
        if name == "translate":
            tx = vals[0] if vals else 0.0
            ty = vals[1] if len(vals) > 1 else 0.0
            a, b, c, d, e, f = mat_mul((a, b, c, d, e, f), (1.0, 0.0, 0.0, 1.0, tx, ty))
        elif name == "scale":
            sx = vals[0] if vals else 1.0
            sy = vals[1] if len(vals) > 1 else sx
            a, b, c, d, e, f = mat_mul((a, b, c, d, e, f), (sx, 0.0, 0.0, sy, 0.0, 0.0))

    sc = math.hypot(a, b)
    return (lambda p: (a * p[0] + c * p[1] + e, b * p[0] + d * p[1] + f)), sc


def render_logo_svg(svg_path: str, out_w: int, out_h: int, bg_color: int):
    """
    Render the logo SVG onto a 16x16 subpixel grid ((out_w*16) x (out_h*16))
    with exact SVG objectBoundingBox gradient interpolation and <g opacity transform>
    support. Zero double-dithering distortion so dark slate ridges and bright
    amber/glacier highlights retain 100% color fidelity on TV scanout.
    """
    root = ET.parse(svg_path).getroot()
    vb = [float(v) for v in root.get("viewBox").split()]
    vx, vy, vw, vh = vb
    sx = out_w / vw
    sy = out_h / vh
    scale = min(sx, sy)
    ox = (out_w - vw * scale) / 2.0 - vx * scale
    oy = (out_h - vh * scale) / 2.0 - vy * scale

    grads = {}
    for el in root.iter():
        if el.tag.replace(NS, "") == "linearGradient":
            grad_id = el.get("id")
            gx1 = parse_pct(el.get("x1"), 0.0)
            gy1 = parse_pct(el.get("y1"), 0.0)
            gx2 = parse_pct(el.get("x2"), 1.0)
            gy2 = parse_pct(el.get("y2"), 0.0)
            stops = []
            for st in el:
                if st.tag.replace(NS, "") == "stop":
                    stops.append(
                        (
                            parse_pct(st.get("offset"), 0.0),
                            parse_hex_color(st.get("stop-color", "#000000")),
                        )
                    )
            if grad_id and len(stops) >= 2:
                o0, c0 = stops[0]
                o1, c1 = stops[-1]
                sx1 = gx1 + (gx2 - gx1) * o0
                sy1 = gy1 + (gy2 - gy1) * o0
                sx2 = gx1 + (gx2 - gx1) * o1
                sy2 = gy1 + (gy2 - gy1) * o1
                grads[grad_id] = (sx1, sy1, sx2, sy2, c0, c1)

    sub_w = out_w * SUB
    sub_h = out_h * SUB
    bg_r = float((bg_color >> 16) & 0xFF)
    bg_g = float((bg_color >> 8) & 0xFF)
    bg_b = float(bg_color & 0xFF)
    sub_r = [bg_r] * (sub_w * sub_h)
    sub_g = [bg_g] * (sub_w * sub_h)
    sub_b = [bg_b] * (sub_w * sub_h)

    def resolve_paint(spec: str):
        if not spec or spec == "none":
            return None
        spec = spec.strip()
        if spec.startswith("url(#") and spec.endswith(")"):
            return grads.get(spec[5:-1])
        return parse_hex_color(spec)

    def paint_polys_subpixel(polys, paint_spec, alpha_f, bbox_override=None):
        edges = []
        all_pts = []
        for pts in polys:
            q = [(round(x * SUB), round(y * SUB)) for x, y in orient(pts)]
            q = [p for k, p in enumerate(q) if p != q[k - 1]]
            if len(q) >= 3:
                edges.append(q)
                all_pts.extend(q)
        if not edges:
            return

        if bbox_override is not None:
            bx0, by0, bw, bh = bbox_override
        else:
            min_sx = min(p[0] for p in all_pts)
            max_sx = max(p[0] for p in all_pts)
            min_sy = min(p[1] for p in all_pts)
            max_sy = max(p[1] for p in all_pts)
            bx0 = min_sx / float(SUB)
            by0 = min_sy / float(SUB)
            bw = max(1e-6, (max_sx - min_sx) / float(SUB))
            bh = max(1e-6, (max_sy - min_sy) / float(SUB))

        is_grad = isinstance(paint_spec, tuple)
        if is_grad:
            gx1, gy1, gx2, gy2, c0, c1 = paint_spec
            gdx = gx2 - gx1
            gdy = gy2 - gy1
            gdenom = gdx * gdx + gdy * gdy
            c0_r = float((c0 >> 16) & 0xFF)
            c0_g = float((c0 >> 8) & 0xFF)
            c0_b = float(c0 & 0xFF)
            c1_r = float((c1 >> 16) & 0xFF)
            c1_g = float((c1 >> 8) & 0xFF)
            c1_b = float(c1 & 0xFF)
        else:
            c_r = float((paint_spec >> 16) & 0xFF)
            c_g = float((paint_spec >> 8) & 0xFF)
            c_b = float(paint_spec & 0xFF)

        alpha = max(0.0, min(1.0, alpha_f))
        if alpha <= 1e-5:
            return
        inv_a = 1.0 - alpha

        min_sy = min(p[1] for p in all_pts)
        max_sy = max(p[1] for p in all_pts)
        sy0 = max(0, min_sy)
        sy1 = min(sub_h - 1, max_sy)

        for sy in range(sy0, sy1 + 1):
            crossings = []
            for q in edges:
                n = len(q)
                for k in range(n):
                    ax, ay = q[k]
                    bx, by = q[(k + 1) % n]
                    ylo, yhi = (ay, by) if ay < by else (by, ay)
                    if sy < ylo or sy >= yhi:
                        continue
                    cx = ax + ((sy - ay) * (bx - ax)) // (by - ay)
                    direction = 1 if by > ay else -1
                    crossings.append((cx, direction))
            if len(crossings) < 2:
                continue
            crossings.sort(key=lambda item: item[0])
            wind = 0
            fy = (sy + 0.5) / float(SUB)
            v = (fy - by0) / bh
            row_base = sy * sub_w
            for k in range(len(crossings) - 1):
                wind += crossings[k][1]
                if wind != 0:
                    xa = max(0, crossings[k][0])
                    xb = min(sub_w, crossings[k + 1][0])
                    for sx_i in range(xa, xb):
                        idx = row_base + sx_i
                        if is_grad:
                            fx = (sx_i + 0.5) / float(SUB)
                            u = (fx - bx0) / bw
                            if gdenom > 1e-9:
                                t = ((u - gx1) * gdx + (v - gy1) * gdy) / gdenom
                            else:
                                t = 0.0
                            t = max(0.0, min(1.0, t))
                            sr = c0_r + (c1_r - c0_r) * t
                            sg = c0_g + (c1_g - c0_g) * t
                            sb = c0_b + (c1_b - c0_b) * t
                        else:
                            sr, sg, sb = c_r, c_g, c_b

                        if alpha >= 0.9999:
                            sub_r[idx] = sr
                            sub_g[idx] = sg
                            sub_b[idx] = sb
                        else:
                            sub_r[idx] = sub_r[idx] * inv_a + sr * alpha
                            sub_g[idx] = sub_g[idx] * inv_a + sg * alpha
                            sub_b[idx] = sub_b[idx] * inv_a + sb * alpha

    def walk_node(node, parent_tf, parent_sc, parent_op):
        tag = node.tag.replace(NS, "")
        if tag == "defs":
            return

        tf_fn, tf_sc = parse_transform(node.get("transform", ""))
        cur_op = parent_op * float(node.get("opacity", "1"))
        cur_sc = parent_sc * tf_sc

        def combined_tf(pt):
            return parent_tf(tf_fn(pt))

        if tag in ("svg", "g"):
            for child in node:
                walk_node(child, combined_tf, cur_sc, cur_op)
            return

        if tag == "path":
            contours = parse_svg_path(node.get("d", ""))
        elif tag == "polygon":
            nums = [
                float(v)
                for v in re.findall(r"[-+]?(?:\d+\.?\d*|\.\d+)", node.get("points", ""))
            ]
            pts = [(nums[k], nums[k + 1]) for k in range(0, len(nums) - 1, 2)]
            contours = [(pts, True)] if len(pts) >= 3 else []
        else:
            return

        if not contours:
            return

        # Compute un-stroked bounding box in screen space for objectBoundingBox gradients
        screen_contours = [
            ([ (combined_tf(p)[0] * scale + ox, combined_tf(p)[1] * scale + oy) for p in pts ], closed)
            for pts, closed in contours
        ]
        flat_pts = [p for pts, _ in screen_contours for p in pts]
        bx0 = min(p[0] for p in flat_pts)
        by0 = min(p[1] for p in flat_pts)
        bw = max(1e-6, max(p[0] for p in flat_pts) - bx0)
        bh = max(1e-6, max(p[1] for p in flat_pts) - by0)
        bbox = (bx0, by0, bw, bh)

        fill_paint = resolve_paint(node.get("fill", "none"))
        if fill_paint is not None:
            fop = cur_op * float(node.get("fill-opacity", "1"))
            polys = [pts for pts, _ in screen_contours]
            paint_polys_subpixel(polys, fill_paint, fop, bbox)

        stroke_paint = resolve_paint(node.get("stroke", "none"))
        if stroke_paint is not None:
            sop = cur_op * float(node.get("stroke-opacity", "1"))
            sw = float(node.get("stroke-width", "1")) * cur_sc * scale / 2.0
            polys = []
            for pts, closed in screen_contours:
                polys.extend(stroke_polyline(pts, closed, sw))
            paint_polys_subpixel(polys, stroke_paint, sop, bbox)

    walk_node(root, lambda p: p, 1.0, 1.0)

    fb = [bg_color] * (out_w * out_h)
    samples_total = float(SUB * SUB)
    for y in range(out_h):
        for x in range(out_w):
            acc_r = 0.0
            acc_g = 0.0
            acc_b = 0.0
            for sy in range(y * SUB, (y + 1) * SUB):
                base = sy * sub_w + x * SUB
                for sx_i in range(base, base + SUB):
                    acc_r += srgb_to_linear(sub_r[sx_i] / 255.0)
                    acc_g += srgb_to_linear(sub_g[sx_i] / 255.0)
                    acc_b += srgb_to_linear(sub_b[sx_i] / 255.0)
            r8 = max(0, min(255, round(linear_to_srgb(acc_r / samples_total) * 255.0)))
            g8 = max(0, min(255, round(linear_to_srgb(acc_g / samples_total) * 255.0)))
            b8 = max(0, min(255, round(linear_to_srgb(acc_b / samples_total) * 255.0)))
            fb[y * out_w + x] = 0xFF000000 | (r8 << 16) | (g8 << 8) | b8
    return fb


def render_symbol_svg(svg_path: str, size: int):
    root = ET.parse(svg_path).getroot()
    vb = [float(v) for v in root.get("viewBox", "0 0 24 24").split()]
    vx, vy, vw, vh = vb
    scale = size / vw
    ox = -vx * scale
    oy = -vy * scale

    def X(p):
        return (p[0] * scale + ox, p[1] * scale + oy)

    default_sw = float(root.get("stroke-width", "2"))
    default_fill = root.get("fill", "none")
    default_stroke = root.get("stroke", "currentColor")

    polys = []
    for el in root.iter():
        tag = el.tag.replace(NS, "")
        if tag == "path":
            contours = parse_svg_path(el.get("d", ""))
        elif tag == "rect":
            rx = float(el.get("rx", "0"))
            contours = rect_contour(
                float(el.get("x", "0")),
                float(el.get("y", "0")),
                float(el.get("width", "0")),
                float(el.get("height", "0")),
                rx,
            )
        elif tag == "circle":
            cx = float(el.get("cx", "0"))
            cy = float(el.get("cy", "0"))
            r = float(el.get("r", "0"))
            contours = [(arc_pts((cx, cy), r, 0.0, 2.0 * math.pi, 20)[:-1], True)]
        else:
            continue

        fill = el.get("fill", default_fill)
        stroke = el.get("stroke", default_stroke)
        sw = float(el.get("stroke-width", str(default_sw))) * scale / 2.0

        if fill and fill != "none":
            for pts, _ in contours:
                polys.append(orient([X(p) for p in pts]))
        if stroke and stroke != "none":
            for pts, closed in contours:
                polys.extend(stroke_polyline([X(p) for p in pts], closed, sw))

    cov = rasterize_polygons(polys, size, size)
    mask = []
    for y in range(size):
        for x in range(size):
            mask.append(min(255, (cov[y][x] * 255 + 128) >> 8))
    return mask


def write_tokens_header(tokens: dict, out_path: str):
    tgt = tokens["target"]
    res = tgt["resolution"]
    safe = tgt["safeArea"]
    usable = tgt["usableCanvas"]
    lay = tokens["layout"]
    slots = lay["slots"]
    mc = lay["mediaCards"]
    fb = lay["footerBar"]
    bdr = lay["borders"]
    lg = tokens["logo"]
    lim = tokens["characterBudgets"]
    paper = tokens["themes"]["paper"]
    signal = tokens["themes"]["signal"]

    with open(out_path, "w", encoding="utf-8") as f:
        w = f.write
        w("/* SPDX-License-Identifier: GPL-2.0-only */\n")
        w("/* Generated from tokens.json v1.2 by scripts/gen-shell-assets.py - do not edit. */\n\n")
        w("#ifndef TVSHELL_TOKENS_H\n#define TVSHELL_TOKENS_H\n\n")
        w("#include <stdint.h>\n\n")

        w(f"#define SHELL_SCREEN_W\t\t\t{res['width']}\n")
        w(f"#define SHELL_SCREEN_H\t\t\t{res['height']}\n")
        w(f"#define SHELL_SAFE_TOP\t\t\t{safe['top']}\n")
        w(f"#define SHELL_SAFE_BOTTOM\t\t{safe['bottom']}\n")
        w(f"#define SHELL_SAFE_LEFT\t\t\t{safe['left']}\n")
        w(f"#define SHELL_SAFE_RIGHT\t\t{safe['right']}\n")
        w(f"#define SHELL_USABLE_W\t\t\t{usable['width']}\n")
        w(f"#define SHELL_USABLE_H\t\t\t{usable['height']}\n\n")

        w(f"#define SHELL_SLOT_GAP\t\t\t{slots['verticalGap']}\n")
        w(f"#define SHELL_HEADER_H\t\t\t{slots['headerHeight']}\n")
        w(f"#define SHELL_SUMMARY_H\t\t\t{slots['summaryBoxHeight']}\n")
        w(f"#define SHELL_MEDIA_GRID_H\t\t{slots['mediaCardsRowHeight']}\n")
        w(f"#define SHELL_MEDIA_GRID_GAP\t\t{mc['gap']}\n")
        w(f"#define SHELL_FOOTER_H\t\t\t{slots['footerBarHeight']}\n")
        w(f"#define SHELL_FOOTER_GAP\t\t{fb['gap']}\n\n")

        w("#define SHELL_SLOT1_Y\t\t\tSHELL_SAFE_TOP\n")
        w("#define SHELL_SLOT2_Y\t\t\t(SHELL_SLOT1_Y + SHELL_HEADER_H + SHELL_SLOT_GAP)\n")
        w("#define SHELL_SLOT3_Y\t\t\t(SHELL_SLOT2_Y + SHELL_SUMMARY_H + SHELL_SLOT_GAP)\n")
        w("#define SHELL_SLOT4_Y\t\t\t(SHELL_SLOT3_Y + SHELL_MEDIA_GRID_H + SHELL_SLOT_GAP)\n\n")

        w(f"#define SHELL_LOGO_W\t\t\t{lg['headerSize']['width']}\n")
        w(f"#define SHELL_LOGO_H\t\t\t{lg['headerSize']['height']}\n")
        w(f"#define SHELL_LOGO_FRAMES\t\t{len(lg['animationFrames'])}\n")
        w(f"#define SHELL_LOGO_FRAME_MS\t\t{lg['frameIntervalMs']}\n\n")

        w(f"#define SHELL_CARD_COUNT\t\t{mc['columns']}\n")
        w(f"#define SHELL_CARD_W\t\t\t{int(mc['cardWidth'])}\n")
        w(f"#define SHELL_CARD_H\t\t\t{mc['cardHeight']}\n")
        w(f"#define SHELL_CARD_RADIUS\t\t{mc['radius']}\n")
        w(f"#define SHELL_CARD_BORDER\t\t{bdr['defaultWidthPx']}\n")
        w(f"#define SHELL_CARD_FOCUS_BORDER\t\t{bdr['focusedWidthPx']}\n\n")

        w("#define SHELL_FOOTER_COUNT\t\t4\n")
        w(f"#define SHELL_FOOTER_RADIUS\t\t{fb['radius']}\n")
        w(f"#define SHELL_FOOTER_BORDER\t\t{bdr['defaultWidthPx']}\n")
        w(f"#define SHELL_FOOTER_FOCUS_BORDER\t{bdr['focusedWidthPx']}\n")
        w("#define SHELL_FOOTER_ICON_BOX\t\t30\n\n")

        w(f"#define SHELL_LIMIT_PROMPT_TITLE\t{lim['headerPromptTitle']}\n")
        w(f"#define SHELL_LIMIT_SUMMARY_TITLE\t{lim['summaryTitle']}\n")
        w(f"#define SHELL_LIMIT_SUMMARY_TEXT\t{lim['summaryText']}\n")
        w("#define SHELL_LIMIT_SUMMARY_LINES\t3\n")
        w(f"#define SHELL_LIMIT_SUMMARY_SOURCE\t{lim['summarySourceCaption']}\n")
        w(f"#define SHELL_LIMIT_CARD_TITLE\t\t{lim['mediaCardTitle']}\n")
        w(f"#define SHELL_LIMIT_CARD_SUB\t\t{lim['mediaCardSubtitle']}\n")
        w(f"#define SHELL_LIMIT_CARD_BADGE\t\t{lim['mediaCardBadge']}\n")
        w(f"#define SHELL_LIMIT_CARD_META\t\t{lim['mediaCardMetaPill']}\n")
        w(f"#define SHELL_LIMIT_FOOTER_LABEL\t{lim['footerLinkLabel']}\n\n")

        w("enum shell_theme_id {\n\tSHELL_THEME_PAPER = 0,\n\tSHELL_THEME_SIGNAL = 1,\n};\n\n")

        w("enum shell_symbol_id {\n")
        w("\tSHELL_SYM_NONE = 0,\n")
        w("\tSHELL_SYM_CHECK,\n")
        w("\tSHELL_SYM_FOLDER,\n")
        w("\tSHELL_SYM_GAME,\n")
        w("\tSHELL_SYM_GLOBE,\n")
        w("\tSHELL_SYM_IMAGE,\n")
        w("\tSHELL_SYM_KEYBOARD,\n")
        w("\tSHELL_SYM_PAGE,\n")
        w("\tSHELL_SYM_PLAY,\n")
        w("\tSHELL_SYM_SETTINGS,\n")
        w("\tSHELL_SYM_SPARK,\n")
        w("\tSHELL_SYM_STREAM,\n")
        w("\tSHELL_SYM_COUNT\n")
        w("};\n\n")

        w("struct shell_theme {\n")
        w("\tconst char *name;\n")
        w("\tuint32_t bg;\n")
        w("\tuint32_t surface;\n")
        w("\tuint32_t surface_hi;\n")
        w("\tuint32_t ink;\n")
        w("\tuint32_t muted;\n")
        w("\tuint32_t border;\n")
        w("\tuint32_t accent;\n")
        w("\tuint32_t focus_ring;\n")
        w("\tuint32_t card_bg;\n")
        w("\tuint32_t btn_bg;\n")
        w("\tuint32_t btn_focus_bg;\n")
        w("};\n\n")

        def emit_theme(enum_name, tname, c):
            w(f"\t[{enum_name}] = {{\n")
            w(f'\t\t.name = "{tname}",\n')
            w(f"\t\t.bg = 0x{parse_hex_color(c['bg']):08x}U,\n")
            w(f"\t\t.surface = 0x{parse_hex_color(c['surface']):08x}U,\n")
            w(f"\t\t.surface_hi = 0x{parse_hex_color(c['surfaceHi']):08x}U,\n")
            w(f"\t\t.ink = 0x{parse_hex_color(c['ink']):08x}U,\n")
            w(f"\t\t.muted = 0x{parse_hex_color(c['muted']):08x}U,\n")
            w(f"\t\t.border = 0x{parse_hex_color(c['border']):08x}U,\n")
            w(f"\t\t.accent = 0x{parse_hex_color(c['accent']):08x}U,\n")
            w(f"\t\t.focus_ring = 0x{parse_hex_color(c['focusRing']):08x}U,\n")
            w(f"\t\t.card_bg = 0x{parse_hex_color(c['cardBg']):08x}U,\n")
            w(f"\t\t.btn_bg = 0x{parse_hex_color(c['btnBg']):08x}U,\n")
            w(f"\t\t.btn_focus_bg = 0x{parse_hex_color(c['btnFocusBg']):08x}U,\n")
            w("\t},\n")

        w("static const struct shell_theme shell_themes[2] = {\n")
        emit_theme("SHELL_THEME_PAPER", "paper", paper)
        emit_theme("SHELL_THEME_SIGNAL", "signal", signal)
        w("};\n\n")
        w("#endif /* TVSHELL_TOKENS_H */\n")


def write_u32_array(w, name: str, arr):
    w(f"static const uint32_t {name}[{len(arr)}] = {{\n")
    for k in range(0, len(arr), 8):
        chunk = ", ".join(f"0x{v:08x}U" for v in arr[k : k + 8])
        w(f"\t{chunk},\n")
    w("};\n\n")


def write_u8_array(w, name: str, arr):
    w(f"static const uint8_t {name}[{len(arr)}] = {{\n")
    for k in range(0, len(arr), 16):
        chunk = ", ".join(f"{v:3d}" for v in arr[k : k + 16])
        w(f"\t{chunk},\n")
    w("};\n\n")


def extract_doom_titlepic():
    """Extract 256-entry XRGB8888 PLAYPAL + 320x200 8-bit indexed TITLEPIC from doom1.wad if present."""
    candidates = [
        "build/dl/doom/doom1.wad",
        "research/doom-wads/Doom1.WAD",
        "build/dl/doom/freedoom1.wad",
    ]
    for path in candidates:
        if not os.path.isfile(path):
            continue
        try:
            with open(path, "rb") as f:
                data = f.read()
            if len(data) < 12:
                continue
            ident, numlumps, infotableofs = struct.unpack("<4sII", data[:12])
            if ident not in (b"IWAD", b"PWAD"):
                continue
            lumps = {}
            for i in range(numlumps):
                off = infotableofs + i * 16
                if off + 16 > len(data):
                    break
                pos, size, name = struct.unpack("<II8s", data[off : off + 16])
                name = name.split(b"\x00", 1)[0].decode("ascii", errors="ignore")
                if name in ("PLAYPAL", "TITLEPIC"):
                    lumps[name] = (pos, size)
            if "PLAYPAL" not in lumps or "TITLEPIC" not in lumps:
                continue
            pal_pos, _ = lumps["PLAYPAL"]
            pal = data[pal_pos : pal_pos + 768]
            t_pos, t_size = lumps["TITLEPIC"]
            pic = data[t_pos : t_pos + t_size]
            w, h, _, _ = struct.unpack("<HHhh", pic[:8])
            if w != 320 or h != 200:
                continue
            pal_u32 = [
                0xFF000000 | (pal[c * 3] << 16) | (pal[c * 3 + 1] << 8) | pal[c * 3 + 2]
                for c in range(256)
            ]
            col_ofs = struct.unpack(f"<{w}I", pic[8 : 8 + 4 * w])
            idx_u8 = [0] * (w * h)
            for x in range(w):
                p = col_ofs[x]
                while p < len(pic):
                    topdelta = pic[p]
                    if topdelta == 0xFF:
                        break
                    length = pic[p + 1]
                    for k in range(length):
                        y = topdelta + k
                        if 0 <= y < h:
                            idx_u8[y * w + x] = pic[p + 3 + k]
                    p += length + 4
            return pal_u32, idx_u8
        except Exception:
            continue
    return None


def write_assets_header(design_dir: str, tokens: dict, out_path: str):
    paper_bg = parse_hex_color(tokens["themes"]["paper"]["bg"])
    signal_bg = parse_hex_color(tokens["themes"]["signal"]["bg"])
    lw = tokens["logo"]["headerSize"]["width"]
    lh = tokens["logo"]["headerSize"]["height"]

    paper_files = [
        ("logo_paper_frame_0", "logo/logo-paper.svg"),
        ("logo_paper_frame_1", "logo/logo-paper-frame-1.svg"),
        ("logo_paper_frame_2", "logo/logo-paper-frame-2.svg"),
        ("logo_paper_frame_3", "logo/logo-paper-frame-3.svg"),
        ("logo_paper_frame_4", "logo/logo-paper-working.svg"),
    ]
    signal_files = [
        ("logo_signal_frame_0", "logo/logo-signal.svg"),
        ("logo_signal_frame_1", "logo/logo-signal-frame-1.svg"),
        ("logo_signal_frame_2", "logo/logo-signal-frame-2.svg"),
        ("logo_signal_frame_3", "logo/logo-signal-frame-3.svg"),
        ("logo_signal_frame_4", "logo/logo-signal-working.svg"),
    ]

    sym_names = [
        ("check", "symbols/check.svg"),
        ("folder", "symbols/folder.svg"),
        ("game", "symbols/game.svg"),
        ("globe", "symbols/globe.svg"),
        ("image", "symbols/image.svg"),
        ("keyboard", "symbols/keyboard.svg"),
        ("page", "symbols/page.svg"),
        ("play", "symbols/play.svg"),
        ("settings", "symbols/settings.svg"),
        ("spark", "symbols/spark.svg"),
        ("stream", "symbols/stream.svg"),
    ]
    sizes = [16, 18, 20]
    doom_poster = extract_doom_titlepic()

    with open(out_path, "w", encoding="utf-8") as f:
        w = f.write
        w("/* SPDX-License-Identifier: GPL-2.0-only */\n")
        w("/* Generated by scripts/gen-shell-assets.py - do not edit. */\n\n")
        w("#ifndef TVSHELL_ASSETS_H\n#define TVSHELL_ASSETS_H\n\n")
        w("#include <stdint.h>\n")
        w('#include "shell_tokens.h"\n\n')

        for name, rel in paper_files:
            arr = render_logo_svg(os.path.join(design_dir, rel), lw, lh, paper_bg)
            write_u32_array(w, name, arr)

        for name, rel in signal_files:
            arr = render_logo_svg(os.path.join(design_dir, rel), lw, lh, signal_bg)
            write_u32_array(w, name, arr)

        w("static const uint32_t * const logo_paper_frames[SHELL_LOGO_FRAMES] = {\n")
        for name, _ in paper_files:
            w(f"\t{name},\n")
        w("};\n\n")

        w("static const uint32_t * const logo_signal_frames[SHELL_LOGO_FRAMES] = {\n")
        for name, _ in signal_files:
            w(f"\t{name},\n")
        w("};\n\n")

        for sz in sizes:
            for sname, rel in sym_names:
                mask = render_symbol_svg(os.path.join(design_dir, rel), sz)
                write_u8_array(w, f"sym_{sname}_{sz}", mask)

        w("static inline const uint8_t *shell_get_symbol_mask(enum shell_symbol_id id, int size)\n{\n")
        w("\tswitch (size) {\n")
        for sz in sizes:
            w(f"\tcase {sz}:\n\t\tswitch (id) {{\n")
            for sname, _ in sym_names:
                enum_id = f"SHELL_SYM_{sname.upper()}"
                w(f"\t\tcase {enum_id}: return sym_{sname}_{sz};\n")
            w("\t\tdefault: return 0;\n\t\t}\n")
        w("\tdefault: return 0;\n\t}\n}\n\n")

        w("static const uint16_t shell_srgb_to_linear[256] = {\n")
        for k in range(0, 256, 8):
            w("\t" + ", ".join(str(v) for v in TO_LIN[k : k + 8]) + ",\n")
        w("};\n\n")

        w("static const uint8_t shell_lin_to_srgb[4096] = {\n")
        for k in range(0, 4096, 16):
            w("\t" + ", ".join(f"{v:3d}" for v in TO_SRGB[k : k + 16]) + ",\n")
        w("};\n\n")

        if doom_poster is not None:
            pal_u32, idx_u8 = doom_poster
            w("#define HAVE_DOOM_POSTER_ASSET 1\n")
            w("#define HAVE_DOOM_POSTER_PALETTE 1\n")
            write_u32_array(w, "doom_poster_palette", pal_u32)
            write_u8_array(w, "doom_poster_idx_320x200", idx_u8)

        w("#endif /* TVSHELL_ASSETS_H */\n")


def main():
    if len(sys.argv) != 3:
        sys.exit("Usage: gen-shell-assets.py <design-dir> <out-dir>")
    design_dir, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(design_dir, "tokens.json"), "r", encoding="utf-8") as f:
        tokens = json.load(f)
    write_tokens_header(tokens, os.path.join(out_dir, "shell_tokens.h"))
    write_assets_header(design_dir, tokens, os.path.join(out_dir, "shell_assets.h"))
    print(f"[tvshell] Generated v1.2 shell_tokens.h and shell_assets.h in {out_dir}")


if __name__ == "__main__":
    main()
