#!/usr/bin/env -S python3 -S
# SPDX-License-Identifier: GPL-2.0-only
"""
Generate TV Shell C headers (shell_tokens.h and shell_assets.h) from
tools/tvshell/design/{tokens.json,logo/*.svg,symbols/*.svg}.

Usage: gen-shell-assets.py <design-dir> <out-dir>
"""

import json
import math
import os
import re
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


def srgb_to_linear(c: float) -> float:
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def linear_to_srgb(c: float) -> float:
    return c * 12.92 if c <= 0.0031308 else 1.055 * (c ** (1.0 / 2.4)) - 0.055


TO_LIN = [round(srgb_to_linear(k / 255.0) * 65535.0) for k in range(256)]


def to_srgb_byte(lin: int) -> int:
    c = max(0.0, min(1.0, lin / 65535.0))
    return max(0, min(255, round(linear_to_srgb(c) * 255.0)))


def blend_linear(dst: int, src: int, alpha_256: int) -> int:
    if alpha_256 <= 0:
        return dst
    if alpha_256 >= 256:
        return src
    out = 0xFF000000
    for sh in (0, 8, 16):
        d = TO_LIN[(dst >> sh) & 0xFF]
        s = TO_LIN[(src >> sh) & 0xFF]
        lin = (d * (256 - alpha_256) + s * alpha_256) >> 8
        out |= to_srgb_byte(lin) << sh
    return out


def flatten_cubic(p0, p1, p2, p3, steps=16):
    out = []
    for i in range(1, steps + 1):
        t = i / steps
        u = 1.0 - t
        x = (u**3) * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + (t**3) * p3[0]
        y = (u**3) * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + (t**3) * p3[1]
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
    return sum(
        pts[k][0] * pts[k - 1][1] - pts[k - 1][0] * pts[k][1]
        for k in range(len(pts))
    ) / 2.0


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


def parse_svg_path(d: str):
    toks = re.findall(r"[A-Za-z]|-?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?", d)
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
        return [((x, y), (x + w, y), (x + w, y + h), (x, y + h))], True
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


def render_logo_svg(svg_path: str, out_w: int, out_h: int, bg_color: int):
    root = ET.parse(svg_path).getroot()
    vb = [float(v) for v in root.get("viewBox").split()]
    vx, vy, vw, vh = vb
    sx = out_w / vw
    sy = out_h / vh
    scale = min(sx, sy)
    ox = (out_w - vw * scale) / 2.0 - vx * scale
    oy = (out_h - vh * scale) / 2.0 - vy * scale

    def X(p):
        return (p[0] * scale + ox, p[1] * scale + oy)

    fb = [bg_color] * (out_w * out_h)
    for el in root.iter():
        tag = el.tag.replace(NS, "")
        if tag != "path":
            continue
        contours = parse_svg_path(el.get("d"))
        fill = el.get("fill", "none")
        if fill and fill != "none":
            c = parse_hex_color(fill)
            polys = [[X(p) for p in pts] for pts, _ in contours]
            cov = rasterize_polygons(polys, out_w, out_h)
            for y in range(out_h):
                for x in range(out_w):
                    a = cov[y][x]
                    if a:
                        fb[y * out_w + x] = blend_linear(fb[y * out_w + x], c, a)
        stroke = el.get("stroke", "none")
        if stroke and stroke != "none":
            c = parse_hex_color(stroke)
            sw = float(el.get("stroke-width", "1")) * scale / 2.0
            polys = []
            for pts, closed in contours:
                polys.extend(stroke_polyline([X(p) for p in pts], closed, sw))
            cov = rasterize_polygons(polys, out_w, out_h)
            for y in range(out_h):
                for x in range(out_w):
                    a = cov[y][x]
                    if a:
                        fb[y * out_w + x] = blend_linear(fb[y * out_w + x], c, a)
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
            contours = parse_svg_path(el.get("d"))
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


def render_dot_mask(size: int):
    scale = size / 24.0
    pts = arc_pts((12.0 * scale, 12.0 * scale), 6.0 * scale, 0.0, 2.0 * math.pi, 24)[:-1]
    cov = rasterize_polygons([orient(pts)], size, size)
    return [min(255, (cov[y][x] * 255 + 128) >> 8) for y in range(size) for x in range(size)]


def write_tokens_header(tokens: dict, out_path: str):
    scr = tokens["screen"]
    tp = tokens["type"]
    sp = tokens["space"]
    lg = tokens["logo"]
    th = tokens["thumbnail"]
    lim = tokens["limits"]
    tm = tokens["timing"]
    paper = tokens["themes"]["paper"]
    signal = tokens["themes"]["signal"]
    pc = paper["color"]
    sc = signal["color"]

    with open(out_path, "w", encoding="utf-8") as f:
        w = f.write
        w("/* SPDX-License-Identifier: GPL-2.0-only */\n")
        w("/* Generated from tokens.json by scripts/gen-shell-assets.py - do not edit. */\n\n")
        w("#ifndef TVSHELL_TOKENS_H\n#define TVSHELL_TOKENS_H\n\n")
        w("#include <stdint.h>\n\n")
        w(f"#define SHELL_SCREEN_W\t\t\t{scr['width']}\n")
        w(f"#define SHELL_SCREEN_H\t\t\t{scr['height']}\n")
        w(f"#define SHELL_SAFE_X\t\t\t{scr['safeX']}\n")
        w(f"#define SHELL_SAFE_Y\t\t\t{scr['safeY']}\n")
        w(f"#define SHELL_CONTENT_LEFT\t\t{scr['contentLeft']}\n")
        w(f"#define SHELL_CONTENT_RIGHT\t\t{scr['contentRight']}\n")
        w(f"#define SHELL_CONTENT_TOP\t\t{scr['contentTop']}\n")
        w(f"#define SHELL_CONTENT_BOTTOM\t\t{scr['contentBottom']}\n")
        w(f"#define SHELL_CONTENT_MAX_W\t\t{scr['contentMaxWidth']}\n")
        w(f"#define SHELL_TEXT_MAX_W\t\t{scr['textMaxWidth']}\n\n")

        w(f"#define SHELL_FONT_DISPLAY\t\t{tp['display']}\n")
        w(f"#define SHELL_FONT_ASK_INPUT\t\t{tp['askInput']}\n")
        w(f"#define SHELL_FONT_ANSWER\t\t{tp['answer']}\n")
        w(f"#define SHELL_FONT_ITEM\t\t\t{tp['item']}\n")
        w(f"#define SHELL_FONT_STEP\t\t\t{tp['step']}\n")
        w(f"#define SHELL_FONT_CHOICE\t\t{tp['choice']}\n")
        w(f"#define SHELL_FONT_QUESTION\t\t{tp['question']}\n")
        w(f"#define SHELL_FONT_TILE_TITLE\t\t{tp['tileTitle']}\n")
        w(f"#define SHELL_FONT_ITEM_DETAIL\t\t{tp['itemDetail']}\n")
        w(f"#define SHELL_FONT_META\t\t\t{tp['meta']}\n")
        w(f"#define SHELL_FONT_TILE_DETAIL\t\t{tp['tileDetail']}\n")
        w(f"#define SHELL_FONT_BADGE\t\t{tp['badge']}\n\n")

        w(f"#define SHELL_SPACE_HEADER_GAP\t\t{sp['headerGap']}\n")
        w(f"#define SHELL_SPACE_RULE_ABOVE\t\t{sp['ruleAbove']}\n")
        w(f"#define SHELL_SPACE_RULE_BELOW\t\t{sp['ruleBelow']}\n")
        w(f"#define SHELL_SPACE_ANSWER_TO_SOURCES\t{sp['answerToSources']}\n")
        w(f"#define SHELL_SPACE_ANSWER_TO_ITEMS\t{sp['answerToItems']}\n")
        w(f"#define SHELL_SPACE_ANSWER_TO_TILES\t{sp['answerToTiles']}\n")
        w(f"#define SHELL_SPACE_CHOICE_GAP\t\t{sp['choiceGap']}\n")
        w(f"#define SHELL_SPACE_CHOICE_PAD_Y\t{sp['choicePadY']}\n")
        w(f"#define SHELL_SPACE_CHOICE_PAD_X\t{sp['choicePadX']}\n")
        w(f"#define SHELL_SPACE_ITEM_PAD_Y\t\t{sp['itemPadY']}\n")
        w(f"#define SHELL_SPACE_ITEM_PAD_X\t\t{sp['itemPadX']}\n")
        w(f"#define SHELL_SPACE_STEP_GAP\t\t{sp['stepGap']}\n")
        w(f"#define SHELL_SPACE_TILE_GAP\t\t{sp['tileGap']}\n")
        w(f"#define SHELL_SPACE_SYMBOL_GAP\t\t{sp['symbolGap']}\n\n")

        w(f"#define SHELL_LOGO_W\t\t\t{lg['width']}\n")
        w(f"#define SHELL_LOGO_H\t\t\t{lg['height']}\n")
        w(f"#define SHELL_LOGO_CLEAR\t\t{lg['clearSpace']}\n\n")

        w(f"#define SHELL_THUMB_W\t\t\t{th['width']}\n")
        w(f"#define SHELL_THUMB_H\t\t\t{th['height']}\n")
        w(f"#define SHELL_THUMB_PER_ROW\t\t{th['perRow']}\n")
        w(f"#define SHELL_THUMB_FOCUS_FRAME\t\t{th['focusFrame']}\n")
        w(f"#define SHELL_THUMB_FOCUS_OFFSET\t{th['focusOffset']}\n\n")

        w(f"#define SHELL_LIMIT_ANSWER_LINES\t{lim['answerLines']}\n")
        w(f"#define SHELL_LIMIT_ITEMS\t\t{lim['items']}\n")
        w(f"#define SHELL_LIMIT_TILES\t\t{lim['tiles']}\n")
        w(f"#define SHELL_LIMIT_LLM_CHOICES\t\t{lim['llmChoices']}\n")
        w(f"#define SHELL_LIMIT_CHOICE_CHARS\t{lim['choiceLabelChars']}\n")
        w(f"#define SHELL_LIMIT_QUESTION_LINES\t{lim['questionLines']}\n\n")

        w(f"#define SHELL_TIMING_LOGO_BLINK_MS\t{tm['logoBlinkMs']}\n")
        w(f"#define SHELL_TIMING_VIEWER_HIDE_MS\t{tm['viewerOverlayHideMs']}\n")
        w(f"#define SHELL_TIMING_MENU_HOLD_MS\t{tm['menuHoldMs']}\n\n")

        w("enum shell_theme_id {\n\tSHELL_THEME_PAPER = 0,\n\tSHELL_THEME_SIGNAL = 1,\n};\n\n")
        w("enum shell_symbol_id {\n")
        w("\tSHELL_SYM_NONE = 0,\n")
        w("\tSHELL_SYM_CHECK,\n")
        w("\tSHELL_SYM_PLAY,\n")
        w("\tSHELL_SYM_PAGE,\n")
        w("\tSHELL_SYM_FOLDER,\n")
        w("\tSHELL_SYM_GAME,\n")
        w("\tSHELL_SYM_KEYBOARD,\n")
        w("\tSHELL_SYM_STEP_DOT,\n")
        w("\tSHELL_SYM_COUNT\n")
        w("};\n\n")

        w("struct shell_theme {\n")
        w("\tconst char *name;\n")
        w("\tconst char *font_regular;\n")
        w("\tconst char *font_bold;\n")
        w("\tint radius;\n")
        w("\tint answer_lh_pct;\n")
        w("\tint has_rule;\n")
        w("\tint has_state_line;\n")
        w("\tint state_line_idle_h;\n")
        w("\tint state_line_work_h;\n")
        w("\tuint32_t ground;\n")
        w("\tuint32_t text;\n")
        w("\tuint32_t choice;\n")
        w("\tuint32_t item;\n")
        w("\tuint32_t secondary;\n")
        w("\tuint32_t rule;\n")
        w("\tuint32_t focus;\n")
        w("\tuint32_t on_focus;\n")
        w("\tuint32_t on_focus_secondary;\n")
        w("\tuint32_t badge;\n")
        w("\tuint32_t on_badge;\n")
        w("\tuint32_t accent;\n")
        w("\tuint32_t state_line_idle;\n")
        w("\tuint32_t state_line_track;\n")
        w("};\n\n")

        w("static const struct shell_theme shell_themes[2] = {\n")
        w("\t[SHELL_THEME_PAPER] = {\n")
        w('\t\t.name = "paper",\n')
        w('\t\t.font_regular = "/usr/share/fonts/tvshell/Literata-Regular.ttf",\n')
        w('\t\t.font_bold = "/usr/share/fonts/tvshell/Literata-SemiBold.ttf",\n')
        w(f"\t\t.radius = {paper['radius']},\n")
        w(f"\t\t.answer_lh_pct = {round(paper['answerLineHeight'] * 100)},\n")
        w("\t\t.has_rule = 1,\n")
        w("\t\t.has_state_line = 0,\n")
        w("\t\t.state_line_idle_h = 0,\n")
        w("\t\t.state_line_work_h = 0,\n")
        w(f"\t\t.ground = 0x{parse_hex_color(pc['ground']):08x}U,\n")
        w(f"\t\t.text = 0x{parse_hex_color(pc['text']):08x}U,\n")
        w(f"\t\t.choice = 0x{parse_hex_color(pc['choice']):08x}U,\n")
        w(f"\t\t.item = 0x{parse_hex_color(pc.get('item', pc['text'])):08x}U,\n")
        w(f"\t\t.secondary = 0x{parse_hex_color(pc['secondary']):08x}U,\n")
        w(f"\t\t.rule = 0x{parse_hex_color(pc['rule']):08x}U,\n")
        w(f"\t\t.focus = 0x{parse_hex_color(pc['focus']):08x}U,\n")
        w(f"\t\t.on_focus = 0x{parse_hex_color(pc['onFocus']):08x}U,\n")
        w(f"\t\t.on_focus_secondary = 0x{parse_hex_color(pc['onFocusSecondary']):08x}U,\n")
        w(f"\t\t.badge = 0x{parse_hex_color(pc['badge']):08x}U,\n")
        w(f"\t\t.on_badge = 0x{parse_hex_color(pc['onBadge']):08x}U,\n")
        w(f"\t\t.accent = 0x{parse_hex_color(pc['accent']):08x}U,\n")
        w(f"\t\t.state_line_idle = 0x{parse_hex_color(pc['rule']):08x}U,\n")
        w(f"\t\t.state_line_track = 0x{parse_hex_color(pc['rule']):08x}U,\n")
        w("\t},\n")
        w("\t[SHELL_THEME_SIGNAL] = {\n")
        w('\t\t.name = "signal",\n')
        w('\t\t.font_regular = "/usr/share/fonts/tvshell/AtkinsonHyperlegible-Regular.ttf",\n')
        w('\t\t.font_bold = "/usr/share/fonts/tvshell/AtkinsonHyperlegible-Bold.ttf",\n')
        w(f"\t\t.radius = {signal['radius']},\n")
        w(f"\t\t.answer_lh_pct = {round(signal['answerLineHeight'] * 100)},\n")
        w("\t\t.has_rule = 0,\n")
        w("\t\t.has_state_line = 1,\n")
        w(f"\t\t.state_line_idle_h = {signal['stateLine']['idleHeight']},\n")
        w(f"\t\t.state_line_work_h = {signal['stateLine']['workingHeight']},\n")
        w(f"\t\t.ground = 0x{parse_hex_color(sc['ground']):08x}U,\n")
        w(f"\t\t.text = 0x{parse_hex_color(sc['text']):08x}U,\n")
        w(f"\t\t.choice = 0x{parse_hex_color(sc['choice']):08x}U,\n")
        w(f"\t\t.item = 0x{parse_hex_color(sc.get('item', sc['text'])):08x}U,\n")
        w(f"\t\t.secondary = 0x{parse_hex_color(sc['secondary']):08x}U,\n")
        w(f"\t\t.rule = 0x{parse_hex_color(sc['rule']):08x}U,\n")
        w(f"\t\t.focus = 0x{parse_hex_color(sc['focus']):08x}U,\n")
        w(f"\t\t.on_focus = 0x{parse_hex_color(sc['onFocus']):08x}U,\n")
        w(f"\t\t.on_focus_secondary = 0x{parse_hex_color(sc['onFocusSecondary']):08x}U,\n")
        w(f"\t\t.badge = 0x{parse_hex_color(sc['badge']):08x}U,\n")
        w(f"\t\t.on_badge = 0x{parse_hex_color(sc['onBadge']):08x}U,\n")
        w(f"\t\t.accent = 0x{parse_hex_color(sc['accent']):08x}U,\n")
        w(f"\t\t.state_line_idle = 0x{parse_hex_color(sc['stateLineIdle']):08x}U,\n")
        w(f"\t\t.state_line_track = 0x{parse_hex_color(sc['stateLineTrack']):08x}U,\n")
        w("\t},\n")
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


def write_assets_header(design_dir: str, tokens: dict, out_path: str):
    paper_bg = parse_hex_color(tokens["themes"]["paper"]["color"]["ground"])
    signal_bg = parse_hex_color(tokens["themes"]["signal"]["color"]["ground"])
    lw = tokens["logo"]["width"]
    lh = tokens["logo"]["height"]

    logos = {
        "logo_paper_ready": render_logo_svg(
            os.path.join(design_dir, "logo/logo-paper.svg"), lw, lh, paper_bg
        ),
        "logo_paper_working": render_logo_svg(
            os.path.join(design_dir, "logo/logo-paper-working.svg"), lw, lh, paper_bg
        ),
        "logo_signal_ready": render_logo_svg(
            os.path.join(design_dir, "logo/logo-signal.svg"), lw, lh, signal_bg
        ),
        "logo_signal_working": render_logo_svg(
            os.path.join(design_dir, "logo/logo-signal-working.svg"), lw, lh, signal_bg
        ),
    }

    sym_names = [
        ("check", "symbols/check.svg"),
        ("play", "symbols/play.svg"),
        ("page", "symbols/page.svg"),
        ("folder", "symbols/folder.svg"),
        ("game", "symbols/game.svg"),
        ("keyboard", "symbols/keyboard.svg"),
    ]
    sizes = [12, 20, 22, 24, 26, 28]

    with open(out_path, "w", encoding="utf-8") as f:
        w = f.write
        w("/* SPDX-License-Identifier: GPL-2.0-only */\n")
        w("/* Generated by scripts/gen-shell-assets.py - do not edit. */\n\n")
        w("#ifndef TVSHELL_ASSETS_H\n#define TVSHELL_ASSETS_H\n\n")
        w("#include <stdint.h>\n")
        w('#include "shell_tokens.h"\n\n')

        for name, arr in logos.items():
            write_u32_array(w, name, arr)

        for sz in sizes:
            for sname, rel in sym_names:
                mask = render_symbol_svg(os.path.join(design_dir, rel), sz)
                write_u8_array(w, f"sym_{sname}_{sz}", mask)
            dot_mask = render_dot_mask(sz)
            write_u8_array(w, f"sym_step_dot_{sz}", dot_mask)

        w("static inline const uint8_t *shell_get_symbol_mask(enum shell_symbol_id id, int size)\n{\n")
        w("\tswitch (size) {\n")
        for sz in sizes:
            w(f"\tcase {sz}:\n\t\tswitch (id) {{\n")
            for sname, _ in sym_names:
                enum_id = f"SHELL_SYM_{sname.upper()}"
                w(f"\t\tcase {enum_id}: return sym_{sname}_{sz};\n")
            w(f"\t\tcase SHELL_SYM_STEP_DOT: return sym_step_dot_{sz};\n")
            w("\t\tdefault: return 0;\n\t\t}\n")
        w("\tdefault: return 0;\n\t}\n}\n\n")
        w("static const uint16_t shell_srgb_to_linear[256] = {\n")
        for k in range(0, 256, 8):
            w("\t" + ", ".join(str(v) for v in TO_LIN[k : k + 8]) + ",\n")
        w("};\n\n")
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
    print(f"[tvshell] Generated shell_tokens.h and shell_assets.h in {out_dir}")


if __name__ == "__main__":
    main()
