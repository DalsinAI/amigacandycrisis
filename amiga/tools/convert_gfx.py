#!/usr/bin/env python3
"""Candy Crisis's pictures as 8-bit PNGs, and the screen palette, for acsdl12.

    python3 amiga/tools/convert_gfx.py [SRC [DST]]
    (default CandyCrisisResources amiga/gfx)

The game draws in 32-bit colour: it blends sprites, shadows, glows and text
over its backgrounds in software. On the Amiga it still does (amiga/
sdlu_amiga.c keeps 32-bit surfaces of its own); only what reaches the
screen goes down to 256 colours. So there are two jobs here:

1. Every PICT_*.jpg/png becomes a PNG that acsdl12's IMG_Load reads without
   loss, and that the game turns back into 32-bit when it loads it:
     grey pictures (the masks, fonts and alpha maps)  8-bit grey, exact
     256 colours or fewer                            paletted, exact
     more than 256 (the JPEGs, the blobs)            paletted, each picture
                                                     with its own 256
                                                     colours (libimagequant
                                                     when Pillow has it);
                                                     640x400 and larger are
                                                     dithered, sprites not
2. screen.pal: the one palette the 8-bit screen is shown in, and the map
   from 15-bit colour to it that sdlu_amiga.c uses when it puts the 32-bit
   frame on the screen:
     "CCPL"                magic
     768 bytes             256 colours, r g b
     4 x 32768 bytes       nearest palette index for each 5-5-5 colour,
                           one table per cell of a 2x2 ordered dither
   Colours within 6 of a palette colour are not dithered (flat black stays
   black). The palette is 27 colours of a 3x3x3 cube and 6 greys (for the game's
   own blends and coloured text) and 223 cut from all the pictures, small
   ones weighted up so the blobs' colours are not lost to the backgrounds.

Needs Pillow and numpy. The output is committed (amiga/gfx), so the Amiga
build itself needs neither. GPL 2, like Candy Crisis.
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image, features

BIG = 640 * 400
DITHER = (-6, 2, 6, -2)   # 2x2 ordered dither: (x&1) | (y&1)<<1


def rgb(path):
    im = Image.open(path)
    im.load()
    return np.asarray(im.convert("RGB"), dtype=np.uint8)


def convert(a, out):
    h, w, _ = a.shape
    grey = (a[..., 0] == a[..., 1]).all() and (a[..., 1] == a[..., 2]).all()
    if grey:
        Image.fromarray(a[..., 0].copy(), "L").save(out, optimize=True)
        return "grey"
    flat = a.reshape(-1, 3)
    cols, idx = np.unique(flat, axis=0, return_inverse=True)
    if len(cols) <= 256:
        im = Image.fromarray(idx.reshape(h, w).astype(np.uint8), "P")
        pal = cols.astype(np.uint8).tobytes()
        im.putpalette(pal + bytes(768 - len(pal)))
        im.save(out, optimize=True, bits=8)
        return "exact %d" % len(cols)
    method = (Image.Quantize.LIBIMAGEQUANT if features.check("libimagequant")
              else Image.Quantize.MEDIANCUT)
    dither = Image.Dither.FLOYDSTEINBERG if w * h >= BIG else Image.Dither.NONE
    q = Image.fromarray(a, "RGB").quantize(colors=256, method=method, dither=dither)
    q.save(out, optimize=True, bits=8)
    return "256 of %d%s" % (len(cols), " dithered" if dither else "")


def screen_palette(pics):
    rows = []
    for a in pics:
        px = a.reshape(-1, 3)
        if (px[:, 0] == px[:, 1]).all() and (px[:, 1] == px[:, 2]).all():
            continue                      # masks and fonts are never shown
        repeat = max(1, min(16, 65536 // len(px)))
        rows.append(np.repeat(px, repeat, axis=0))
    sample = np.concatenate(rows)
    width = 1024
    pad = (-len(sample)) % width
    sample = np.concatenate([sample, sample[:pad]])
    im = Image.fromarray(sample.reshape(-1, width, 3), "RGB")
    method = (Image.Quantize.LIBIMAGEQUANT if features.check("libimagequant")
              else Image.Quantize.MEDIANCUT)
    fixed = [(r, g, b) for r in (0, 128, 255) for g in (0, 128, 255) for b in (0, 128, 255)]
    fixed += [(v, v, v) for v in (36, 73, 109, 146, 182, 219)]
    n = 256 - len(fixed)
    q = im.quantize(colors=n, method=method, dither=Image.Dither.NONE)
    pal = np.array(q.getpalette()[:3 * n], dtype=np.uint8).reshape(-1, 3)
    colours = fixed + [tuple(int(v) for v in c) for c in pal]
    return np.array(colours[:256], dtype=np.int32)


def nearest_tables(pal):
    v = np.arange(32768)
    c = np.stack([(v >> 10) & 31, (v >> 5) & 31, v & 31], axis=1) * 8 + 4
    wt = np.array([3, 4, 2])

    def nearest(cc):
        best = np.zeros(32768, dtype=np.uint8)
        dist = np.zeros(32768, dtype=np.int64)
        for lo in range(0, 32768, 4096):
            d = (((cc[lo:lo + 4096, None, :] - pal[None, :, :]) ** 2) * wt).sum(axis=2)
            best[lo:lo + 4096] = d.argmin(axis=1)
            dist[lo:lo + 4096] = d.min(axis=1)
        return best, dist

    plain, dist = nearest(c)
    exact = dist <= wt.sum() * 6 * 6     # a palette colour: not dithered
    tables = []
    for bias in DITHER:
        best, _ = nearest(np.clip(c + bias, 0, 255))
        tables.append(np.where(exact, plain, best).astype(np.uint8))
    return tables


def main():
    src = Path(sys.argv[1] if len(sys.argv) > 1 else "CandyCrisisResources")
    dst = Path(sys.argv[2] if len(sys.argv) > 2 else "amiga/gfx")
    dst.mkdir(parents=True, exist_ok=True)
    pics = []
    for f in sorted(src.glob("PICT_*")):
        if f.suffix.lower() not in (".jpg", ".png"):
            continue
        a = rgb(f)
        pics.append(a)
        how = convert(a, dst / (f.stem + ".png"))
        print(f"{f.name:16} {a.shape[1]}x{a.shape[0]:<5} {how}")
    pal = screen_palette(pics)
    tables = nearest_tables(pal)
    with open(dst / "screen.pal", "wb") as out:
        out.write(b"CCPL")
        out.write(pal.astype(np.uint8).tobytes())
        for t in tables:
            out.write(t.tobytes())
    print(f"{len(pics)} pictures and screen.pal to {dst}")


if __name__ == "__main__":
    main()
