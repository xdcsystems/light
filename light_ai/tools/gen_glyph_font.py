#!/usr/bin/env python3
"""gen_glyph_font.py -- bake the callout label font into include/core/glyph_font_table.hpp.

DejaVu Sans (fonts-dejavu-core; Bitstream Vera / DejaVu license: externals/fonts/DejaVu-LICENSE)
rendered at ATLAS_PX px with 8-bit antialiasing, one shelf-packed atlas of two 8-bit planes,
interleaved per texel as (L, A): the GL_LUMINANCE_ALPHA upload source.
  - L, the fill: the glyph's own coverage (the white text);
  - A, the outline: the dark outline that keeps white text readable on any background (#91
    item 5), baked here instead of drawn as 8 offset passes (#108). By default it is exactly
    what those passes accumulated: each drew the glyph offset by (+-OUTLINE_PX, +-OUTLINE_PX),
    (0, +-OUTLINE_PX), (+-OUTLINE_PX, 0) in one gray at alpha --outline-alpha, and "over" with
    one color is associative, so together they cover
        A(p) = 1 - prod_d (1 - alpha * L(p - d))       (alpha = --outline-alpha / 255)
    with that gray: the renderers composite the fill (vertex color x L) over the outline
    (kOutlineGray x A), and the result equals the 8-pass image at the atlas texel centers.
    --outline disk bakes a disk dilation of radius OUTLINE_PX instead (alpha x max L in the
    disk; an A/B option, not the default);
  - every glyph stored as its ink rect plus a BORDER px border: OUTLINE_PX px for the outline,
    plus 1 px whose both planes are 0 (the outer ring of every rect, asserted below);
    glyphs GAP px apart, the gap all zeros. The CPU sampler clamps into the rect; GL_LINEAR
    does not, but without mipmaps a fragment inside a quad taps at most 1 texel outside its
    rect, into the gap: the invariant is "the outer ring of every rect is 0 in both planes,
    and the gap is at least 1 px of zeros", so no sample ever reads a neighbour's ink;
  - a 4x4 block at (0, 0) with L = 255, A = 0: the texel the solid quads (box outlines,
    connector lines) sample, safe under GL_LINEAR at its center (2, 2).
The atlas is baked at 2x the default on-screen size (11 px at --label-scale 1, #91
item 5): at --label-scale 1 the renderers scale it DOWN (0.5 on a 4:3 USB tile, 0.67
on a portrait phone tile), so the glyphs stay crisp; at 2 it is drawn 1:1 to 1.33x,
and at 3..4 it is magnified 1.5..2.67x (GL_LINEAR: the text gets soft). The outline is
OUTLINE_PX atlas px wide, so 2 * scale px in the target (the same width on every tile).

Metrics per glyph (atlas px): ax, ay = the padded rect in the atlas; w, h = its size;
dx, dy = its offset from the pen (dx) and from the line top (dy); adv = the advance.
The line box is [0, LINE_H): from the highest ink top to the lowest ink bottom of the
charset (tight: the label height is the text height; the outline may stick out of it).

Usage (from the project root, with the venv's Pillow):
    venv/bin/python tools/gen_glyph_font.py [--font PATH] [--out PATH] [--outline exact|disk]
                                            [--outline-alpha 0..255]
"""
import argparse
import sys

from PIL import Image, ImageDraw, ImageFont

ATLAS_PX = 22
ATLAS_W = 256
OUTLINE_PX = 2
BORDER = OUTLINE_PX + 1
GAP = 2
SOLID = 4
CHARSET = (
    "0123456789"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "АБВГДЕЁЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ"
    "абвгдеёжзийклмнопрстуфхцчшщъыьэюя"
    "#№.,;:!?(){}«»–—×·°"
)
# The 8 offsets of the former outline passes, in atlas px.
OUTLINE_DIRS = [(dx * OUTLINE_PX, dy * OUTLINE_PX) for dy in (-1, 0, 1) for dx in (-1, 0, 1) if (dx, dy) != (0, 0)]


def render(font, ch):
    """The ink of ch: (image cropped to the ink, ink left, ink top) in pen coordinates
    (x from the pen, y from the ascender line)."""
    l, t, r, b = font.getbbox(ch)
    img = Image.new("L", (r - l + 4, b - t + 4), 0)
    ImageDraw.Draw(img).text((2 - l, 2 - t), ch, font=font, fill=255)
    box = img.getbbox()
    if box is None:
        raise SystemExit(f"no ink for {ch!r}")
    ink = img.crop(box)
    return ink, l - 2 + box[0], t - 2 + box[1]


def outline_plane(fill, mode, alpha):
    """The A plane of one padded glyph (fill: its L plane, an "L" image): see the docstring."""
    w, h = fill.size
    f = fill.load()
    out = Image.new("L", (w, h), 0)
    o = out.load()
    disk = [(dx, dy) for dy in range(-OUTLINE_PX, OUTLINE_PX + 1) for dx in range(-OUTLINE_PX, OUTLINE_PX + 1)
            if dx * dx + dy * dy <= OUTLINE_PX * OUTLINE_PX]

    def at(x, y):
        return f[x, y] / 255.0 if 0 <= x < w and 0 <= y < h else 0.0

    for y in range(h):
        for x in range(w):
            if mode == "exact":
                keep = 1.0
                for dx, dy in OUTLINE_DIRS:
                    keep *= 1.0 - alpha * at(x - dx, y - dy)
                cov = 1.0 - keep
            else:
                cov = alpha * max(at(x - dx, y - dy) for dx, dy in disk)
            o[x, y] = int(round(255.0 * cov))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--font", default="/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf")
    ap.add_argument("--out", default="include/core/glyph_font_table.hpp")
    ap.add_argument("--outline", choices=("exact", "disk"), default="exact")
    ap.add_argument("--outline-alpha", type=int, default=200)
    args = ap.parse_args()
    if not 0 <= args.outline_alpha <= 255:
        raise SystemExit("--outline-alpha: 0..255")

    font = ImageFont.truetype(args.font, ATLAS_PX)
    glyphs = []
    for ch in CHARSET:
        ink, ix, iy = render(font, ch)
        glyphs.append({"ch": ch, "ink": ink, "ix": ix, "iy": iy, "adv": int(round(font.getlength(ch)))})
    top = min(g["iy"] for g in glyphs)
    bottom = max(g["iy"] + g["ink"].height for g in glyphs)
    line_h = bottom - top
    space_adv = int(round(font.getlength(" ")))

    # Shelf packing, tallest first; the solid block opens the first shelf.
    order = sorted(range(len(glyphs)), key=lambda i: -glyphs[i]["ink"].height)
    x, y, shelf_h = SOLID + GAP, 0, SOLID
    for i in order:
        g = glyphs[i]
        pw, ph = g["ink"].width + 2 * BORDER, g["ink"].height + 2 * BORDER
        if x + pw > ATLAS_W:
            x, y, shelf_h = 0, y + shelf_h + GAP, 0
        g["ax"], g["ay"], g["w"], g["h"] = x, y, pw, ph
        x += pw + GAP
        shelf_h = max(shelf_h, ph)
    atlas_h = (y + shelf_h + 3) & ~3

    fill = Image.new("L", (ATLAS_W, atlas_h), 0)
    outl = Image.new("L", (ATLAS_W, atlas_h), 0)
    fill.paste(255, (0, 0, SOLID, SOLID))
    for g in glyphs:
        gf = Image.new("L", (g["w"], g["h"]), 0)
        gf.paste(g["ink"], (BORDER, BORDER))
        ga = outline_plane(gf, args.outline, args.outline_alpha / 255.0)
        for plane in (gf, ga):  # the outer ring is 0 in both planes
            p = plane.load()
            ring = [p[i, 0] for i in range(g["w"])] + [p[i, g["h"] - 1] for i in range(g["w"])]
            ring += [p[0, j] for j in range(g["h"])] + [p[g["w"] - 1, j] for j in range(g["h"])]
            assert max(ring) == 0, f"ink on the outer ring of {g['ch']!r}"
        fill.paste(gf, (g["ax"], g["ay"]))
        outl.paste(ga, (g["ax"], g["ay"]))
        g["dx"] = g["ix"] - BORDER
        g["dy"] = g["iy"] - top - BORDER
        assert -128 <= g["dx"] < 128 and -128 <= g["dy"] < 128 and g["w"] < 256 and g["h"] < 256
    assert GAP >= 1

    lp = fill.tobytes()
    ap_ = outl.tobytes()
    px = bytearray(2 * len(lp))
    px[0::2] = lp
    px[1::2] = ap_
    out = []
    w = out.append
    w("// DejaVu Sans, %d px, 8-bit fill + outline (L, A) atlas. GENERATED by tools/gen_glyph_font.py -- do not edit."
      % ATLAS_PX)
    w("// Font: DejaVu Sans. Copyright (c) 2003 by Bitstream, Inc. All Rights Reserved. Bitstream Vera is a")
    w("// trademark of Bitstream, Inc. DejaVu changes are in public domain. License: externals/fonts/DejaVu-LICENSE.")
    w("// Layout and the meaning of every field: the docstring of tools/gen_glyph_font.py.")
    w("// Generated with: --outline %s --outline-alpha %d" % (args.outline, args.outline_alpha))
    w("")
    w("#pragma once")
    w("")
    w("#include <cstdint>")
    w("")
    w("namespace glyph_font")
    w("{")
    w("    constexpr int kGlyphCount = %d;" % len(glyphs))
    for decl, note in (("kAtlasPx = %d;" % ATLAS_PX, "the pixel size the atlas was rendered at"),
                       ("kLineH = %d;" % line_h, "line box height: highest ink top to lowest ink bottom"),
                       ("kSpaceAdv = %d;" % space_adv, "advance of the space (no glyph)"),
                       ("kOutlinePx = %d;" % OUTLINE_PX, "outline width (atlas px) baked into the A plane"),
                       ("kOutlineAlpha = %d;" % args.outline_alpha, "the outline alpha folded into the A plane"),
                       ("kGlyphBorder = %d;" % BORDER, "padded rect = ink + this border on each side")):
        w("    constexpr int %-22s// %s" % (decl, note))
    w("    constexpr int kAtlasW = %d;" % ATLAS_W)
    w("    constexpr int kAtlasH = %d;" % atlas_h)
    w("    constexpr int kAtlasBytes = kAtlasW * kAtlasH * 2;  // (L, A) per texel")
    w("    constexpr int kSolidX = %d;  // center of the %dx%d block at (0, 0): L = 255, A = 0" % (SOLID // 2, SOLID, SOLID))
    w("    constexpr int kSolidY = %d;" % (SOLID // 2))
    w("")
    w("    // ax, ay, w, h: the padded glyph rect in the atlas (ink + kGlyphBorder px on each side);")
    w("    // dx: its x offset from the pen; dy: its y offset from the line top; adv: the advance.")
    w("    struct GlyphMetric")
    w("    {")
    w("        uint16_t ax, ay;")
    w("        uint8_t w, h;")
    w("        int8_t dx, dy;")
    w("        uint8_t adv;")
    w("    };")
    w("")
    w("    constexpr uint32_t kCodepoints[kGlyphCount] = {")
    cps = ["0x%04Xu" % ord(g["ch"]) for g in glyphs]
    for i in range(0, len(cps), 10):
        w("        " + ", ".join(cps[i:i + 10]) + ",")
    w("    };")
    w("")
    w("    constexpr GlyphMetric kGlyphMetrics[kGlyphCount] = {")
    for g in glyphs:
        w("        { %d, %d, %d, %d, %d, %d, %d },  // U+%04X" % (g["ax"], g["ay"], g["w"], g["h"], g["dx"], g["dy"], g["adv"],
                                                            ord(g["ch"])))
    w("    };")
    w("")
    w("    // Row-major, kAtlasW texels a row, each texel (L, A).")
    w("    constexpr uint8_t kAtlas[kAtlasBytes] = {")
    row_bytes = 2 * ATLAS_W
    for r in range(atlas_h):
        row = px[r * row_bytes:(r + 1) * row_bytes]
        for c in range(0, row_bytes, 32):
            w("        " + ",".join(str(v) for v in row[c:c + 32]) + ",")
    w("    };")
    w("")
    w("}  // namespace glyph_font")
    with open(args.out, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")
    adv = {g["ch"]: g["adv"] for g in glyphs}
    adv[" "] = space_adv
    print(f"{args.out}: {len(glyphs)} glyphs, atlas {ATLAS_W}x{atlas_h} x 2 B = {len(px)} B, line {line_h} px, "
          f"'Person #7' = {sum(adv[c] for c in 'Person #7')} px, outline {args.outline} alpha {args.outline_alpha}",
          file=sys.stderr)


if __name__ == "__main__":
    main()
