#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
Patch doomgeneric's software renderer (r_*) to render the 3D viewport natively
at high resolution (960x600 4:3 or 1066x600 true-FOV 16:9 widescreen, 3x the
original 320x200 resolution) into I_HiresBuffer while keeping 2D menus, status
bar, intermission screens, and HUD patches in 320x200 coordinate space with
per-pixel overlay compositing.
"""

import os
import shutil
import sys


def patch_file(dg_dir: str, filename: str, transform) -> None:
    path = os.path.join(dg_dir, filename)
    orig = path + ".orig"
    if not os.path.exists(orig):
        shutil.copy2(path, orig)
    with open(orig, "r", encoding="latin-1") as f:
        src = f.read()
    out = transform(src)
    if out == src:
        raise RuntimeError(f"{filename}: transform produced no changes")
    cur = None
    if os.path.exists(path):
        with open(path, "r", encoding="latin-1") as f:
            cur = f.read()
    if cur != out:
        with open(path, "w", encoding="latin-1") as f:
            f.write(out)
        print(f"  [patched] {filename}")


def must_replace(text: str, old: str, new: str, label: str = "") -> str:
    if old not in text:
        raise RuntimeError(f"must_replace failed ({label}): target not found:\n{old}")
    return text.replace(old, new, 1)


def patch_r_defs_h(s: str) -> str:
    s = must_replace(
        s,
        "#define MAXDRAWSEGS\t\t256",
        "#define MAXDRAWSEGS\t\t1024\n\n"
        "#define HIRES_SCALE\t\t3\n"
        "#define R_MAXWIDTH\t\t1280\n"
        "#define R_MAXHEIGHT\t\t720\n"
        "#define HIRES_HEIGHT\t\t(SCREENHEIGHT * HIRES_SCALE)\n\n"
        "extern int\t\tI_HiresWidth;\n"
        "extern int\t\tI_HiresViewActive;\n"
        "extern int\t\tI_RecordingUiOverlay;\n"
        "extern byte\t\tI_HiresBuffer[R_MAXWIDTH * R_MAXHEIGHT];\n"
        "extern byte\t\tI_UiOverlayMask[SCREENWIDTH * SCREENHEIGHT];",
        "r_defs.h MAXDRAWSEGS",
    )
    s = must_replace(
        s,
        "  byte\t\tpad1;\n"
        "  // Here lies the rub for all\n"
        "  //  dynamic resize/change of resolution.\n"
        "  byte\t\ttop[SCREENWIDTH];\n"
        "  byte\t\tpad2;\n"
        "  byte\t\tpad3;\n"
        "  // See above.\n"
        "  byte\t\tbottom[SCREENWIDTH];\n"
        "  byte\t\tpad4;",
        "  unsigned short\tpad1;\n"
        "  unsigned short\ttop[R_MAXWIDTH];\n"
        "  unsigned short\tpad2;\n"
        "  unsigned short\tpad3;\n"
        "  unsigned short\tbottom[R_MAXWIDTH];\n"
        "  unsigned short\tpad4;",
        "r_defs.h visplane_t",
    )
    return s


def patch_r_state_h(s: str) -> str:
    return must_replace(
        s,
        "extern angle_t\t\txtoviewangle[SCREENWIDTH+1];",
        "extern angle_t\t\txtoviewangle[R_MAXWIDTH+1];",
        "r_state.h xtoviewangle",
    )


def patch_r_main_h(s: str) -> str:
    return must_replace(
        s,
        "#define MAXLIGHTSCALE\t\t48",
        "#define MAXLIGHTSCALE\t\t(48 * HIRES_SCALE)",
        "r_main.h MAXLIGHTSCALE",
    )


def patch_r_plane_h(s: str) -> str:
    return must_replace(
        s,
        "extern short\t\tfloorclip[SCREENWIDTH];\n"
        "extern short\t\tceilingclip[SCREENWIDTH];\n"
        "\n"
        "extern fixed_t\t\tyslope[SCREENHEIGHT];\n"
        "extern fixed_t\t\tdistscale[SCREENWIDTH];",
        "extern short\t\tfloorclip[R_MAXWIDTH];\n"
        "extern short\t\tceilingclip[R_MAXWIDTH];\n"
        "\n"
        "extern fixed_t\t\tyslope[R_MAXHEIGHT];\n"
        "extern fixed_t\t\tdistscale[R_MAXWIDTH];",
        "r_plane.h arrays",
    )


def patch_r_plane_c(s: str) -> str:
    s = must_replace(
        s,
        "#define MAXVISPLANES\t128\n"
        "visplane_t\t\tvisplanes[MAXVISPLANES];\n"
        "visplane_t*\t\tlastvisplane;\n"
        "visplane_t*\t\tfloorplane;\n"
        "visplane_t*\t\tceilingplane;\n"
        "\n"
        "// ?\n"
        "#define MAXOPENINGS\tSCREENWIDTH*64\n"
        "short\t\t\topenings[MAXOPENINGS];\n"
        "short*\t\t\tlastopening;\n"
        "\n"
        "\n"
        "//\n"
        "// Clip values are the solid pixel bounding the range.\n"
        "//  floorclip starts out SCREENHEIGHT\n"
        "//  ceilingclip starts out -1\n"
        "//\n"
        "short\t\t\tfloorclip[SCREENWIDTH];\n"
        "short\t\t\tceilingclip[SCREENWIDTH];\n"
        "\n"
        "//\n"
        "// spanstart holds the start of a plane span\n"
        "// initialized to 0 at start\n"
        "//\n"
        "int\t\t\tspanstart[SCREENHEIGHT];\n"
        "int\t\t\tspanstop[SCREENHEIGHT];\n"
        "\n"
        "//\n"
        "// texture mapping\n"
        "//\n"
        "lighttable_t**\t\tplanezlight;\n"
        "fixed_t\t\t\tplaneheight;\n"
        "\n"
        "fixed_t\t\t\tyslope[SCREENHEIGHT];\n"
        "fixed_t\t\t\tdistscale[SCREENWIDTH];\n"
        "fixed_t\t\t\tbasexscale;\n"
        "fixed_t\t\t\tbaseyscale;\n"
        "\n"
        "fixed_t\t\t\tcachedheight[SCREENHEIGHT];\n"
        "fixed_t\t\t\tcacheddistance[SCREENHEIGHT];\n"
        "fixed_t\t\t\tcachedxstep[SCREENHEIGHT];\n"
        "fixed_t\t\t\tcachedystep[SCREENHEIGHT];",
        "#define MAXVISPLANES\t512\n"
        "visplane_t\t\tvisplanes[MAXVISPLANES];\n"
        "visplane_t*\t\tlastvisplane;\n"
        "visplane_t*\t\tfloorplane;\n"
        "visplane_t*\t\tceilingplane;\n"
        "\n"
        "// ?\n"
        "#define MAXOPENINGS\t(R_MAXWIDTH*256)\n"
        "short\t\t\topenings[MAXOPENINGS];\n"
        "short*\t\t\tlastopening;\n"
        "\n"
        "\n"
        "//\n"
        "// Clip values are the solid pixel bounding the range.\n"
        "//  floorclip starts out SCREENHEIGHT\n"
        "//  ceilingclip starts out -1\n"
        "//\n"
        "short\t\t\tfloorclip[R_MAXWIDTH];\n"
        "short\t\t\tceilingclip[R_MAXWIDTH];\n"
        "\n"
        "//\n"
        "// spanstart holds the start of a plane span\n"
        "// initialized to 0 at start\n"
        "//\n"
        "int\t\t\tspanstart[R_MAXHEIGHT];\n"
        "int\t\t\tspanstop[R_MAXHEIGHT];\n"
        "\n"
        "//\n"
        "// texture mapping\n"
        "//\n"
        "lighttable_t**\t\tplanezlight;\n"
        "fixed_t\t\t\tplaneheight;\n"
        "\n"
        "fixed_t\t\t\tyslope[R_MAXHEIGHT];\n"
        "fixed_t\t\t\tdistscale[R_MAXWIDTH];\n"
        "fixed_t\t\t\tbasexscale;\n"
        "fixed_t\t\t\tbaseyscale;\n"
        "\n"
        "fixed_t\t\t\tcachedheight[R_MAXHEIGHT];\n"
        "fixed_t\t\t\tcacheddistance[R_MAXHEIGHT];\n"
        "fixed_t\t\t\tcachedxstep[R_MAXHEIGHT];\n"
        "fixed_t\t\t\tcachedystep[R_MAXHEIGHT];",
        "r_plane.c declarations",
    )
    s = must_replace(
        s,
        "    basexscale = FixedDiv (finecosine[angle],centerxfrac);\n"
        "    baseyscale = -FixedDiv (finesine[angle],centerxfrac);",
        "    basexscale = FixedDiv (finecosine[angle],projection);\n"
        "    baseyscale = -FixedDiv (finesine[angle],projection);",
        "r_plane.c R_ClearPlanes projection",
    )
    s = must_replace(
        s,
        "    check->minx = SCREENWIDTH;\n"
        "    check->maxx = -1;\n"
        "    \n"
        "    memset (check->top,0xff,sizeof(check->top));",
        "    check->minx = viewwidth;\n"
        "    check->maxx = -1;\n"
        "    \n"
        "    memset (check->top,0xff,sizeof(check->top));\n"
        "    memset (check->bottom,0,sizeof(check->bottom));",
        "r_plane.c R_FindPlane",
    )
    s = must_replace(
        s,
        "    for (x=intrl ; x<= intrh ; x++)\n"
        "\tif (pl->top[x] != 0xff)\n"
        "\t    break;",
        "    for (x=intrl ; x<= intrh ; x++)\n"
        "\tif (pl->top[x] != 0xffffU)\n"
        "\t    break;",
        "r_plane.c R_CheckPlane sentinel",
    )
    s = must_replace(
        s,
        "    pl = lastvisplane++;\n"
        "    pl->minx = start;\n"
        "    pl->maxx = stop;\n"
        "\n"
        "    memset (pl->top,0xff,sizeof(pl->top));",
        "    if (lastvisplane - visplanes == MAXVISPLANES)\n"
        "\tI_Error (\"R_CheckPlane: no more visplanes\");\n"
        "    pl = lastvisplane++;\n"
        "    pl->minx = start;\n"
        "    pl->maxx = stop;\n"
        "\n"
        "    memset (pl->top,0xff,sizeof(pl->top));\n"
        "    memset (pl->bottom,0,sizeof(pl->bottom));",
        "r_plane.c R_CheckPlane init",
    )
    s = must_replace(
        s,
        "\tpl->top[pl->maxx+1] = 0xff;\n"
        "\tpl->top[pl->minx-1] = 0xff;",
        "\tpl->top[pl->maxx+1] = 0xffffU;\n"
        "\tpl->top[pl->minx-1] = 0xffffU;\n"
        "\tpl->bottom[pl->maxx+1] = 0;\n"
        "\tpl->bottom[pl->minx-1] = 0;",
        "r_plane.c R_DrawPlanes sentinel",
    )
    return s


def patch_r_things_h(s: str) -> str:
    s = must_replace(
        s,
        "#define MAXVISSPRITES  \t128",
        "#define MAXVISSPRITES  \t512",
        "r_things.h MAXVISSPRITES",
    )
    s = must_replace(
        s,
        "extern short\t\tnegonearray[SCREENWIDTH];\n"
        "extern short\t\tscreenheightarray[SCREENWIDTH];",
        "extern short\t\tnegonearray[R_MAXWIDTH];\n"
        "extern short\t\tscreenheightarray[R_MAXWIDTH];",
        "r_things.h arrays",
    )
    return s


def patch_r_things_c(s: str) -> str:
    s = must_replace(
        s,
        "short\t\tnegonearray[SCREENWIDTH];\n"
        "short\t\tscreenheightarray[SCREENWIDTH];",
        "short\t\tnegonearray[R_MAXWIDTH];\n"
        "short\t\tscreenheightarray[R_MAXWIDTH];",
        "r_things.c arrays",
    )
    s = must_replace(
        s,
        "    for (i=0 ; i<SCREENWIDTH ; i++)\n"
        "    {\n"
        "\tnegonearray[i] = -1;\n"
        "    }",
        "    for (i=0 ; i<R_MAXWIDTH ; i++)\n"
        "    {\n"
        "\tnegonearray[i] = -1;\n"
        "    }",
        "r_things.c R_InitSprites",
    )
    s = must_replace(
        s,
        "static short\t\tclipbot[SCREENWIDTH];\n"
        "static short\t\tcliptop[SCREENWIDTH];",
        "static short\t\tclipbot[R_MAXWIDTH];\n"
        "static short\t\tcliptop[R_MAXWIDTH];",
        "r_things.c clipbot/cliptop",
    )
    return s


def patch_r_bsp_c(s: str) -> str:
    return must_replace(
        s,
        "#define MAXSEGS\t\t32",
        "#define MAXSEGS\t\t256",
        "r_bsp.c MAXSEGS",
    )


def patch_r_main_c(s: str) -> str:
    s = must_replace(
        s,
        "angle_t\t\t\txtoviewangle[SCREENWIDTH+1];",
        "angle_t\t\t\txtoviewangle[R_MAXWIDTH+1];\n\n"
        "int\t\t\tI_HiresWidth = SCREENWIDTH * HIRES_SCALE;\n"
        "int\t\t\tI_HiresViewActive = 0;\n"
        "int\t\t\tI_RecordingUiOverlay = 0;\n"
        "byte\t\t\tI_HiresBuffer[R_MAXWIDTH * R_MAXHEIGHT];\n"
        "byte\t\t\tI_UiOverlayMask[SCREENWIDTH * SCREENHEIGHT];",
        "r_main.c globals",
    )
    s = must_replace(
        s,
        "    focallength = FixedDiv (centerxfrac,\n"
        "\t\t\t    finetangent[FINEANGLES/4+FIELDOFVIEW/2] );",
        "    focallength = FixedDiv (projection,\n"
        "\t\t\t    finetangent[FINEANGLES/4+FIELDOFVIEW/2] );",
        "r_main.c R_InitTextureMapping focallength",
    )
    s = must_replace(
        s,
        "    if (setblocks == 11)\n"
        "    {\n"
        "\tscaledviewwidth = SCREENWIDTH;\n"
        "\tviewheight = SCREENHEIGHT;\n"
        "    }\n"
        "    else\n"
        "    {\n"
        "\tscaledviewwidth = setblocks*32;\n"
        "\tviewheight = (setblocks*168/10)&~7;\n"
        "    }\n"
        "    \n"
        "    detailshift = setdetail;\n"
        "    viewwidth = scaledviewwidth>>detailshift;\n"
        "\t\n"
        "    centery = viewheight/2;\n"
        "    centerx = viewwidth/2;\n"
        "    centerxfrac = centerx<<FRACBITS;\n"
        "    centeryfrac = centery<<FRACBITS;\n"
        "    projection = centerxfrac;",
        "    {\n"
        "\tint w43;\n"
        "\tif (setblocks == 11)\n"
        "\t{\n"
        "\t    scaledviewwidth = I_HiresWidth;\n"
        "\t    viewheight = HIRES_HEIGHT;\n"
        "\t    w43 = SCREENWIDTH * HIRES_SCALE;\n"
        "\t}\n"
        "\telse\n"
        "\t{\n"
        "\t    scaledviewwidth = (setblocks * 32 * I_HiresWidth) / SCREENWIDTH;\n"
        "\t    viewheight = ((setblocks * 168 / 10) & ~7) * HIRES_SCALE;\n"
        "\t    w43 = setblocks * 32 * HIRES_SCALE;\n"
        "\t}\n"
        "\tdetailshift = setdetail;\n"
        "\tviewwidth = scaledviewwidth >> detailshift;\n"
        "\tcentery = viewheight / 2;\n"
        "\tcenterx = viewwidth / 2;\n"
        "\tcenterxfrac = centerx << FRACBITS;\n"
        "\tcenteryfrac = centery << FRACBITS;\n"
        "\tprojection = ((w43 >> detailshift) / 2) << FRACBITS;\n"
        "    }",
        "r_main.c R_ExecuteSetViewSize dimensions",
    )
    s = must_replace(
        s,
        "    // psprite scales\n"
        "    pspritescale = FRACUNIT*viewwidth/SCREENWIDTH;\n"
        "    pspriteiscale = FRACUNIT*SCREENWIDTH/viewwidth;\n"
        "    \n"
        "    // thing clipping\n"
        "    for (i=0 ; i<viewwidth ; i++)\n"
        "\tscreenheightarray[i] = viewheight;\n"
        "    \n"
        "    // planes\n"
        "    for (i=0 ; i<viewheight ; i++)\n"
        "    {\n"
        "\tdy = ((i-viewheight/2)<<FRACBITS)+FRACUNIT/2;\n"
        "\tdy = abs(dy);\n"
        "\tyslope[i] = FixedDiv ( (viewwidth<<detailshift)/2*FRACUNIT, dy);\n"
        "    }\n"
        "\t\n"
        "    for (i=0 ; i<viewwidth ; i++)\n"
        "    {\n"
        "\tcosadj = abs(finecosine[xtoviewangle[i]>>ANGLETOFINESHIFT]);\n"
        "\tdistscale[i] = FixedDiv (FRACUNIT,cosadj);\n"
        "    }\n"
        "    \n"
        "    // Calculate the light levels to use\n"
        "    //  for each level / scale combination.\n"
        "    for (i=0 ; i< LIGHTLEVELS ; i++)\n"
        "    {\n"
        "\tstartmap = ((LIGHTLEVELS-1-i)*2)*NUMCOLORMAPS/LIGHTLEVELS;\n"
        "\tfor (j=0 ; j<MAXLIGHTSCALE ; j++)\n"
        "\t{\n"
        "\t    level = startmap - j*SCREENWIDTH/(viewwidth<<detailshift)/DISTMAP;",
        "    // psprite scales\n"
        "    pspritescale = FixedDiv(projection, (SCREENWIDTH / 2) * FRACUNIT);\n"
        "    pspriteiscale = FixedDiv((SCREENWIDTH / 2) * FRACUNIT, projection);\n"
        "    \n"
        "    // thing clipping\n"
        "    for (i=0 ; i<viewwidth ; i++)\n"
        "\tscreenheightarray[i] = viewheight;\n"
        "    \n"
        "    // planes\n"
        "    for (i=0 ; i<viewheight ; i++)\n"
        "    {\n"
        "\tdy = ((i-viewheight/2)<<FRACBITS)+FRACUNIT/2;\n"
        "\tdy = abs(dy);\n"
        "\tyslope[i] = FixedDiv ( projection << detailshift, dy);\n"
        "    }\n"
        "\t\n"
        "    for (i=0 ; i<viewwidth ; i++)\n"
        "    {\n"
        "\tcosadj = abs(finecosine[xtoviewangle[i]>>ANGLETOFINESHIFT]);\n"
        "\tdistscale[i] = FixedDiv (FRACUNIT,cosadj);\n"
        "    }\n"
        "    \n"
        "    // Calculate the light levels to use\n"
        "    //  for each level / scale combination.\n"
        "    for (i=0 ; i< LIGHTLEVELS ; i++)\n"
        "    {\n"
        "\tstartmap = ((LIGHTLEVELS-1-i)*2)*NUMCOLORMAPS/LIGHTLEVELS;\n"
        "\tfor (j=0 ; j<MAXLIGHTSCALE ; j++)\n"
        "\t{\n"
        "\t    level = startmap - j * (SCREENWIDTH * HIRES_SCALE) / (((projection >> FRACBITS) * 2) << detailshift) / (DISTMAP * HIRES_SCALE);",
        "r_main.c R_ExecuteSetViewSize scales",
    )
    s = must_replace(
        s,
        "    R_DrawMasked ();\n"
        "\n"
        "    // Check for new console commands.\n"
        "    NetUpdate ();\t\t\t\t\n"
        "}",
        "    R_DrawMasked ();\n"
        "\n"
        "    // Check for new console commands.\n"
        "    NetUpdate ();\n"
        "\n"
        "    {\n"
        "\tint wy2d = viewwindowy / HIRES_SCALE;\n"
        "\tint vh2d = viewheight / HIRES_SCALE;\n"
        "\tint wx2d = (viewwindowx * SCREENWIDTH) / I_HiresWidth;\n"
        "\tint vw2d = (scaledviewwidth * SCREENWIDTH) / I_HiresWidth;\n"
        "\tint y2d, x2d;\n"
        "\tfor (y2d = wy2d; y2d < wy2d + vh2d && y2d < SCREENHEIGHT; y2d++)\n"
        "\t{\n"
        "\t    const byte *hrow = I_HiresBuffer + (y2d * HIRES_SCALE + 1) * I_HiresWidth;\n"
        "\t    byte *vrow = I_VideoBuffer + y2d * SCREENWIDTH;\n"
        "\t    for (x2d = wx2d; x2d < wx2d + vw2d && x2d < SCREENWIDTH; x2d++)\n"
        "\t    {\n"
        "\t\tint x_hr = (x2d * I_HiresWidth) / SCREENWIDTH;\n"
        "\t\tvrow[x2d] = hrow[x_hr];\n"
        "\t    }\n"
        "\t}\n"
        "\tI_HiresViewActive = 1;\n"
        "\tI_RecordingUiOverlay = 1;\n"
        "    }\n"
        "}",
        "r_main.c R_RenderPlayerView epilogue",
    )
    return s


def patch_r_draw_c(s: str) -> str:
    s = must_replace(
        s,
        "#define MAXWIDTH\t\t\t1120\n"
        "#define MAXHEIGHT\t\t\t832",
        "#define MAXWIDTH\t\t\tR_MAXWIDTH\n"
        "#define MAXHEIGHT\t\t\tR_MAXHEIGHT",
        "r_draw.c MAXWIDTH/MAXHEIGHT",
    )
    # Replace RANGECHECK bounds in R_DrawColumn*, R_DrawFuzzColumn*, R_DrawTranslatedColumn*, R_DrawSpan*
    s = s.replace(
        "if ((unsigned)dc_x >= SCREENWIDTH\n\t|| dc_yl < 0\n\t|| dc_yh >= SCREENHEIGHT)",
        "if ((unsigned)dc_x >= (unsigned)I_HiresWidth\n\t|| dc_yl < 0\n\t|| dc_yh >= HIRES_HEIGHT)",
    )
    s = s.replace(
        "if ((unsigned)dc_x >= SCREENWIDTH\n\t|| dc_yl < 0 || dc_yh >= SCREENHEIGHT)",
        "if ((unsigned)dc_x >= (unsigned)I_HiresWidth\n\t|| dc_yl < 0 || dc_yh >= HIRES_HEIGHT)",
    )
    s = s.replace(
        "if ((unsigned)x >= SCREENWIDTH\n\t|| dc_yl < 0 || dc_yh >= SCREENHEIGHT)",
        "if ((unsigned)x >= (unsigned)I_HiresWidth\n\t|| dc_yl < 0 || dc_yh >= HIRES_HEIGHT)",
    )
    s = s.replace(
        "if ((unsigned)x >= SCREENWIDTH\n\t|| dc_yl < 0\n\t|| dc_yh >= SCREENHEIGHT)",
        "if ((unsigned)x >= (unsigned)I_HiresWidth\n\t|| dc_yl < 0\n\t|| dc_yh >= HIRES_HEIGHT)",
    )
    s = s.replace(
        "if (ds_x2 < ds_x1\n\t|| ds_x1<0\n\t|| ds_x2>=SCREENWIDTH\n\t|| (unsigned)ds_y>SCREENHEIGHT)",
        "if (ds_x2 < ds_x1\n\t|| ds_x1<0\n\t|| ds_x2>=I_HiresWidth\n\t|| (unsigned)ds_y>=(unsigned)HIRES_HEIGHT)",
    )
    # Replace row stride increments in column drawers
    s = s.replace("\tdest += SCREENWIDTH; \n", "\tdest += I_HiresWidth; \n")
    s = s.replace("\tdest += SCREENWIDTH;\n", "\tdest += I_HiresWidth;\n")
    s = s.replace("\tdest2 += SCREENWIDTH;\n", "\tdest2 += I_HiresWidth;\n")

    # Replace FUZZOFF table with sign table + runtime initialization in R_InitBuffer
    s = must_replace(
        s,
        "#define FUZZTABLE\t\t50 \n"
        "#define FUZZOFF\t(SCREENWIDTH)\n"
        "\n"
        "\n"
        "int\tfuzzoffset[FUZZTABLE] =\n"
        "{\n"
        "    FUZZOFF,-FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,\n"
        "    FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,\n"
        "    FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,\n"
        "    FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,\n"
        "    FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,\n"
        "    FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,\n"
        "    FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF \n"
        "};",
        "#define FUZZTABLE\t\t50 \n"
        "#define FUZZOFF\t1\n"
        "\n"
        "static const int\tfuzzsign[FUZZTABLE] =\n"
        "{\n"
        "    FUZZOFF,-FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,\n"
        "    FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,\n"
        "    FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,\n"
        "    FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,\n"
        "    FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,\n"
        "    FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,\n"
        "    FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF \n"
        "};\n"
        "int\tfuzzoffset[FUZZTABLE];",
        "r_draw.c fuzzoffset",
    )

    s = must_replace(
        s,
        "void\n"
        "R_InitBuffer\n"
        "( int\t\twidth,\n"
        "  int\t\theight ) \n"
        "{ \n"
        "    int\t\ti; \n"
        "\n"
        "    // Handle resize,\n"
        "    //  e.g. smaller view windows\n"
        "    //  with border and/or status bar.\n"
        "    viewwindowx = (SCREENWIDTH-width) >> 1; \n"
        "\n"
        "    // Column offset. For windows.\n"
        "    for (i=0 ; i<width ; i++) \n"
        "\tcolumnofs[i] = viewwindowx + i;\n"
        "\n"
        "    // Samw with base row offset.\n"
        "    if (width == SCREENWIDTH) \n"
        "\tviewwindowy = 0; \n"
        "    else \n"
        "\tviewwindowy = (SCREENHEIGHT-SBARHEIGHT-height) >> 1; \n"
        "\n"
        "    // Preclaculate all row offsets.\n"
        "    for (i=0 ; i<height ; i++) \n"
        "\tylookup[i] = I_VideoBuffer + (i+viewwindowy)*SCREENWIDTH; \n"
        "}",
        "void\n"
        "R_InitBuffer\n"
        "( int\t\twidth,\n"
        "  int\t\theight ) \n"
        "{ \n"
        "    int\t\ti; \n"
        "\n"
        "    viewwindowx = (I_HiresWidth - width) >> 1; \n"
        "\n"
        "    for (i=0 ; i<width ; i++) \n"
        "\tcolumnofs[i] = viewwindowx + i;\n"
        "\n"
        "    if (width == I_HiresWidth) \n"
        "\tviewwindowy = 0; \n"
        "    else \n"
        "\tviewwindowy = (HIRES_HEIGHT - (SBARHEIGHT * HIRES_SCALE) - height) >> 1; \n"
        "\n"
        "    for (i=0 ; i<height ; i++) \n"
        "\tylookup[i] = I_HiresBuffer + (i + viewwindowy) * I_HiresWidth; \n"
        "\n"
        "    for (i=0 ; i<FUZZTABLE ; i++)\n"
        "\tfuzzoffset[i] = fuzzsign[i] * I_HiresWidth;\n"
        "}",
        "r_draw.c R_InitBuffer",
    )

    s = must_replace(
        s,
        "    if (scaledviewwidth == SCREENWIDTH)\n"
        "    {\n"
        "        if (background_buffer != NULL)\n"
        "        {\n"
        "            Z_Free(background_buffer);\n"
        "            background_buffer = NULL;\n"
        "        }\n"
        "\n"
        "\treturn;\n"
        "    }",
        "    int orig_vw = (scaledviewwidth * SCREENWIDTH) / I_HiresWidth;\n"
        "    int orig_vh = viewheight / HIRES_SCALE;\n"
        "    int orig_wx = (SCREENWIDTH - orig_vw) >> 1;\n"
        "    int orig_wy = (orig_vw == SCREENWIDTH) ? 0 : ((SCREENHEIGHT - SBARHEIGHT - orig_vh) >> 1);\n"
        "\n"
        "    if (orig_vw == SCREENWIDTH)\n"
        "    {\n"
        "        if (background_buffer != NULL)\n"
        "        {\n"
        "            Z_Free(background_buffer);\n"
        "            background_buffer = NULL;\n"
        "        }\n"
        "\n"
        "\treturn;\n"
        "    }",
        "r_draw.c R_FillBackScreen header",
    )
    s = must_replace(
        s,
        "    for (x=0 ; x<scaledviewwidth ; x+=8)\n"
        "\tV_DrawPatch(viewwindowx+x, viewwindowy-8, patch);\n"
        "    patch = W_CacheLumpName(DEH_String(\"brdr_b\"),PU_CACHE);\n"
        "\n"
        "    for (x=0 ; x<scaledviewwidth ; x+=8)\n"
        "\tV_DrawPatch(viewwindowx+x, viewwindowy+viewheight, patch);\n"
        "    patch = W_CacheLumpName(DEH_String(\"brdr_l\"),PU_CACHE);\n"
        "\n"
        "    for (y=0 ; y<viewheight ; y+=8)\n"
        "\tV_DrawPatch(viewwindowx-8, viewwindowy+y, patch);\n"
        "    patch = W_CacheLumpName(DEH_String(\"brdr_r\"),PU_CACHE);\n"
        "\n"
        "    for (y=0 ; y<viewheight ; y+=8)\n"
        "\tV_DrawPatch(viewwindowx+scaledviewwidth, viewwindowy+y, patch);\n"
        "\n"
        "    // Draw beveled edge. \n"
        "    V_DrawPatch(viewwindowx-8,\n"
        "                viewwindowy-8,\n"
        "                W_CacheLumpName(DEH_String(\"brdr_tl\"),PU_CACHE));\n"
        "    \n"
        "    V_DrawPatch(viewwindowx+scaledviewwidth,\n"
        "                viewwindowy-8,\n"
        "                W_CacheLumpName(DEH_String(\"brdr_tr\"),PU_CACHE));\n"
        "    \n"
        "    V_DrawPatch(viewwindowx-8,\n"
        "                viewwindowy+viewheight,\n"
        "                W_CacheLumpName(DEH_String(\"brdr_bl\"),PU_CACHE));\n"
        "    \n"
        "    V_DrawPatch(viewwindowx+scaledviewwidth,\n"
        "                viewwindowy+viewheight,\n"
        "                W_CacheLumpName(DEH_String(\"brdr_br\"),PU_CACHE));",
        "    for (x=0 ; x<orig_vw ; x+=8)\n"
        "\tV_DrawPatch(orig_wx+x, orig_wy-8, patch);\n"
        "    patch = W_CacheLumpName(DEH_String(\"brdr_b\"),PU_CACHE);\n"
        "\n"
        "    for (x=0 ; x<orig_vw ; x+=8)\n"
        "\tV_DrawPatch(orig_wx+x, orig_wy+orig_vh, patch);\n"
        "    patch = W_CacheLumpName(DEH_String(\"brdr_l\"),PU_CACHE);\n"
        "\n"
        "    for (y=0 ; y<orig_vh ; y+=8)\n"
        "\tV_DrawPatch(orig_wx-8, orig_wy+y, patch);\n"
        "    patch = W_CacheLumpName(DEH_String(\"brdr_r\"),PU_CACHE);\n"
        "\n"
        "    for (y=0 ; y<orig_vh ; y+=8)\n"
        "\tV_DrawPatch(orig_wx+orig_vw, orig_wy+y, patch);\n"
        "\n"
        "    // Draw beveled edge. \n"
        "    V_DrawPatch(orig_wx-8,\n"
        "                orig_wy-8,\n"
        "                W_CacheLumpName(DEH_String(\"brdr_tl\"),PU_CACHE));\n"
        "    \n"
        "    V_DrawPatch(orig_wx+orig_vw,\n"
        "                orig_wy-8,\n"
        "                W_CacheLumpName(DEH_String(\"brdr_tr\"),PU_CACHE));\n"
        "    \n"
        "    V_DrawPatch(orig_wx-8,\n"
        "                orig_wy+orig_vh,\n"
        "                W_CacheLumpName(DEH_String(\"brdr_bl\"),PU_CACHE));\n"
        "    \n"
        "    V_DrawPatch(orig_wx+orig_vw,\n"
        "                orig_wy+orig_vh,\n"
        "                W_CacheLumpName(DEH_String(\"brdr_br\"),PU_CACHE));",
        "r_draw.c R_FillBackScreen patches",
    )
    s = must_replace(
        s,
        "    if (scaledviewwidth == SCREENWIDTH) \n"
        "\treturn; \n"
        "  \n"
        "    top = ((SCREENHEIGHT-SBARHEIGHT)-viewheight)/2; \n"
        "    side = (SCREENWIDTH-scaledviewwidth)/2; \n"
        " \n"
        "    // copy top and one line of left side \n"
        "    R_VideoErase (0, top*SCREENWIDTH+side); \n"
        " \n"
        "    // copy one line of right side and bottom \n"
        "    ofs = (viewheight+top)*SCREENWIDTH-side; \n"
        "    R_VideoErase (ofs, top*SCREENWIDTH+side); \n"
        " \n"
        "    // copy sides using wraparound \n"
        "    ofs = top*SCREENWIDTH + SCREENWIDTH-side; \n"
        "    side <<= 1;\n"
        "    \n"
        "    for (i=1 ; i<viewheight ; i++) \n"
        "    { \n"
        "\tR_VideoErase (ofs, side); \n"
        "\tofs += SCREENWIDTH; \n"
        "    }",
        "    int orig_vw = (scaledviewwidth * SCREENWIDTH) / I_HiresWidth;\n"
        "    int orig_vh = viewheight / HIRES_SCALE;\n"
        "    if (orig_vw == SCREENWIDTH) \n"
        "\treturn; \n"
        "  \n"
        "    top = ((SCREENHEIGHT-SBARHEIGHT)-orig_vh)/2; \n"
        "    side = (SCREENWIDTH-orig_vw)/2; \n"
        " \n"
        "    // copy top and one line of left side \n"
        "    R_VideoErase (0, top*SCREENWIDTH+side); \n"
        " \n"
        "    // copy one line of right side and bottom \n"
        "    ofs = (orig_vh+top)*SCREENWIDTH-side; \n"
        "    R_VideoErase (ofs, top*SCREENWIDTH+side); \n"
        " \n"
        "    // copy sides using wraparound \n"
        "    ofs = top*SCREENWIDTH + SCREENWIDTH-side; \n"
        "    side <<= 1;\n"
        "    \n"
        "    for (i=1 ; i<orig_vh ; i++) \n"
        "    { \n"
        "\tR_VideoErase (ofs, side); \n"
        "\tofs += SCREENWIDTH; \n"
        "    }",
        "r_draw.c R_DrawViewBorder",
    )
    return s


def patch_hu_lib_c(s: str) -> str:
    return must_replace(
        s,
        "    if (!automapactive &&\n"
        "\tviewwindowx && l->needsupdate)\n"
        "    {\n"
        "\tlh = SHORT(l->f[0]->height) + 1;\n"
        "\tfor (y=l->y,yoffset=y*SCREENWIDTH ; y<l->y+lh ; y++,yoffset+=SCREENWIDTH)\n"
        "\t{\n"
        "\t    if (y < viewwindowy || y >= viewwindowy + viewheight)\n"
        "\t\tR_VideoErase(yoffset, SCREENWIDTH); // erase entire line\n"
        "\t    else\n"
        "\t    {\n"
        "\t\tR_VideoErase(yoffset, viewwindowx); // erase left border\n"
        "\t\tR_VideoErase(yoffset + viewwindowx + viewwidth, viewwindowx);\n"
        "\t\t// erase right border\n"
        "\t    }\n"
        "\t}\n"
        "    }",
        "    if (!automapactive &&\n"
        "\tviewwindowx && l->needsupdate)\n"
        "    {\n"
        "\tint wx = (viewwindowx * SCREENWIDTH) / I_HiresWidth;\n"
        "\tint wy = viewwindowy / HIRES_SCALE;\n"
        "\tint vw = (scaledviewwidth * SCREENWIDTH) / I_HiresWidth;\n"
        "\tint vh = viewheight / HIRES_SCALE;\n"
        "\tlh = SHORT(l->f[0]->height) + 1;\n"
        "\tfor (y=l->y,yoffset=y*SCREENWIDTH ; y<l->y+lh ; y++,yoffset+=SCREENWIDTH)\n"
        "\t{\n"
        "\t    if (y < wy || y >= wy + vh)\n"
        "\t\tR_VideoErase(yoffset, SCREENWIDTH); // erase entire line\n"
        "\t    else\n"
        "\t    {\n"
        "\t\tR_VideoErase(yoffset, wx); // erase left border\n"
        "\t\tR_VideoErase(yoffset + wx + vw, wx);\n"
        "\t    }\n"
        "\t}\n"
        "    }",
        "hu_lib.c HUlib_eraseTextLine",
    )


def patch_v_video_c(s: str) -> str:
    s = must_replace(
        s,
        "#include \"v_video.h\"",
        "#include \"v_video.h\"\n#include \"r_defs.h\"",
        "v_video.c include r_defs.h",
    )
    s = must_replace(
        s,
        "            while (count--)\n"
        "            {\n"
        "                *dest = *source++;\n"
        "                dest += SCREENWIDTH;\n"
        "            }",
        "            if (dest_screen == I_VideoBuffer && I_RecordingUiOverlay)\n"
        "            {\n"
        "                byte *m = I_UiOverlayMask + (dest - I_VideoBuffer);\n"
        "                while (count--)\n"
        "                {\n"
        "                    *dest = *source++;\n"
        "                    *m = 1;\n"
        "                    dest += SCREENWIDTH;\n"
        "                    m += SCREENWIDTH;\n"
        "                }\n"
        "            }\n"
        "            else\n"
        "            {\n"
        "                while (count--)\n"
        "                {\n"
        "                    *dest = *source++;\n"
        "                    dest += SCREENWIDTH;\n"
        "                }\n"
        "            }",
        "v_video.c V_DrawPatch mask",
    )
    s = must_replace(
        s,
        "            while (count--)\n"
        "            {\n"
        "                *dest = *source++;\n"
        "                dest += SCREENWIDTH;\n"
        "            }",
        "            if (dest_screen == I_VideoBuffer && I_RecordingUiOverlay)\n"
        "            {\n"
        "                byte *m = I_UiOverlayMask + (dest - I_VideoBuffer);\n"
        "                while (count--)\n"
        "                {\n"
        "                    *dest = *source++;\n"
        "                    *m = 1;\n"
        "                    dest += SCREENWIDTH;\n"
        "                    m += SCREENWIDTH;\n"
        "                }\n"
        "            }\n"
        "            else\n"
        "            {\n"
        "                while (count--)\n"
        "                {\n"
        "                    *dest = *source++;\n"
        "                    dest += SCREENWIDTH;\n"
        "                }\n"
        "            }",
        "v_video.c V_DrawPatchFlipped mask",
    )
    return s


def patch_d_main_c(s: str) -> str:
    s = must_replace(
        s,
        "    if (nodrawers)\n"
        "    \treturn;                    // for comparative timing / profiling\n"
        "\t\t\n"
        "    redrawsbar = false;",
        "    if (nodrawers)\n"
        "    \treturn;                    // for comparative timing / profiling\n"
        "\t\t\n"
        "    I_HiresViewActive = 0;\n"
        "    I_RecordingUiOverlay = 0;\n"
        "    memset(I_UiOverlayMask, 0, sizeof(I_UiOverlayMask));\n"
        "    redrawsbar = false;",
        "d_main.c D_Display init",
    )
    s = must_replace(
        s,
        "\t\tif (wipe || (viewheight != 200 && fullscreen) )\n"
        "\t\t\tredrawsbar = true;\n"
        "\t\tif (inhelpscreensstate && !inhelpscreens)\n"
        "\t\t\tredrawsbar = true;              // just put away the help screen\n"
        "\t\tST_Drawer (viewheight == 200, redrawsbar );\n"
        "\t\tfullscreen = viewheight == 200;",
        "\t\tif (wipe || (viewheight != HIRES_HEIGHT && fullscreen) )\n"
        "\t\t\tredrawsbar = true;\n"
        "\t\tif (inhelpscreensstate && !inhelpscreens)\n"
        "\t\t\tredrawsbar = true;              // just put away the help screen\n"
        "\t\tST_Drawer (viewheight == HIRES_HEIGHT, redrawsbar );\n"
        "\t\tfullscreen = viewheight == HIRES_HEIGHT;",
        "d_main.c ST_Drawer fullscreen check",
    )
    s = must_replace(
        s,
        "    if (gamestate == GS_LEVEL && !automapactive && scaledviewwidth != 320)",
        "    if (gamestate == GS_LEVEL && !automapactive && scaledviewwidth != I_HiresWidth)",
        "d_main.c border check",
    )
    s = must_replace(
        s,
        "\t\telse\n"
        "\t\t\ty = viewwindowy+4;\n"
        "\t\tV_DrawPatchDirect(viewwindowx + (scaledviewwidth - 68) / 2, y,\n"
        "\t\t\t\t\t\t\t  W_CacheLumpName (DEH_String(\"M_PAUSE\"), PU_CACHE));",
        "\t\telse\n"
        "\t\t\ty = (viewwindowy / HIRES_SCALE) + 4;\n"
        "\t\tV_DrawPatchDirect(((viewwindowx * SCREENWIDTH) / I_HiresWidth) + (((scaledviewwidth * SCREENWIDTH) / I_HiresWidth) - 68) / 2, y,\n"
        "\t\t\t\t\t\t\t  W_CacheLumpName (DEH_String(\"M_PAUSE\"), PU_CACHE));",
        "d_main.c M_PAUSE",
    )
    s = must_replace(
        s,
        "    // normal update\n"
        "    if (!wipe)\n"
        "    {\n"
        "\tI_FinishUpdate ();              // page flip or blit buffer\n"
        "\treturn;\n"
        "    }\n"
        "    \n"
        "    // wipe update\n"
        "    wipe_EndScreen(0, 0, SCREENWIDTH, SCREENHEIGHT);",
        "    I_RecordingUiOverlay = 0;\n"
        "    // normal update\n"
        "    if (!wipe)\n"
        "    {\n"
        "\tI_FinishUpdate ();              // page flip or blit buffer\n"
        "\treturn;\n"
        "    }\n"
        "    \n"
        "    // wipe update\n"
        "    I_HiresViewActive = 0;\n"
        "    wipe_EndScreen(0, 0, SCREENWIDTH, SCREENHEIGHT);",
        "d_main.c wipe handling",
    )
    return s


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <doomgeneric-dir>", file=sys.stderr)
        return 1
    dg_dir = sys.argv[1]
    patch_file(dg_dir, "r_defs.h", patch_r_defs_h)
    patch_file(dg_dir, "r_state.h", patch_r_state_h)
    patch_file(dg_dir, "r_main.h", patch_r_main_h)
    patch_file(dg_dir, "r_plane.h", patch_r_plane_h)
    patch_file(dg_dir, "r_plane.c", patch_r_plane_c)
    patch_file(dg_dir, "r_things.h", patch_r_things_h)
    patch_file(dg_dir, "r_things.c", patch_r_things_c)
    patch_file(dg_dir, "r_bsp.c", patch_r_bsp_c)
    patch_file(dg_dir, "r_main.c", patch_r_main_c)
    patch_file(dg_dir, "r_draw.c", patch_r_draw_c)
    patch_file(dg_dir, "hu_lib.c", patch_hu_lib_c)
    patch_file(dg_dir, "v_video.c", patch_v_video_c)
    patch_file(dg_dir, "d_main.c", patch_d_main_c)
    return 0


if __name__ == "__main__":
    sys.exit(main())
