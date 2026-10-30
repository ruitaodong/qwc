#!/usr/bin/env python3
"""Write a small synthetic Aperio SVS slide for testing the OpenSlide driver.

Layout follows real Aperio files, which is what OpenSlide's Aperio reader
expects: tiled JPEG level 0, a stripped thumbnail, tiled reduced levels, then
a stripped label image. The picture makes mistakes easy to spot: red top-left,
green top-right, blue bottom-left, white bottom-right, black grid every 500 px.

    pip install tifffile imagecodecs numpy
    python3 make_test_svs.py slide.svs

(OpenSlide takes an associated image's name from the first word of the
second description line, so that one must use a plain "\n".)
"""
import sys

import numpy as np
import tifffile

W, H, TILE = 6000, 4000, 256
DESC = "Aperio Image Library v12.0.15 \r\n{w}x{h} [0,0 {w}x{h}] ({t}x{t}) JPEG/RGB Q=90|AppMag = 20|MPP = 0.4990|Title = synthetic test slide"


def picture(w, h):
    img = np.zeros((h, w, 3), np.uint8)
    img[: h // 2, : w // 2] = (220, 40, 40)     # red, top-left
    img[: h // 2, w // 2:] = (40, 200, 40)      # green, top-right
    img[h // 2:, : w // 2] = (40, 40, 220)      # blue, bottom-left
    img[h // 2:, w // 2:] = (250, 250, 250)     # white, bottom-right
    step = max(1, w * 500 // W)
    img[::step, :] = 0
    img[:, ::step] = 0
    return img


def shrink(img, factor):
    h, w = img.shape[0] // factor, img.shape[1] // factor
    return img[: h * factor, : w * factor].reshape(h, factor, w, factor, 3).mean((1, 3)).astype(np.uint8)


def main(path):
    base = picture(W, H)
    # metadata=None: otherwise tifffile adds its own JSON ImageDescription,
    # which hides the Aperio one OpenSlide detects the format by
    jpeg = dict(compression="jpeg", compressionargs={"level": 90}, photometric="rgb", metadata=None)
    with tifffile.TiffWriter(path, bigtiff=False) as tif:
        tif.write(base, tile=(TILE, TILE), description=DESC.format(w=W, h=H, t=TILE), **jpeg)
        thumb = shrink(base, 25)
        tif.write(thumb, description=f"Aperio Image Library v12.0.15 \r\n{W}x{H} -> {thumb.shape[1]}x{thumb.shape[0]} - |AppMag = 20|MPP = 0.4990", **jpeg)
        for factor in (4, 16):
            lvl = shrink(base, factor)
            tif.write(lvl, tile=(TILE, TILE), description=f"Aperio Image Library v12.0.15 \r\n{W}x{H} -> {lvl.shape[1]}x{lvl.shape[0]} - |AppMag = 20|MPP = 0.4990", **jpeg)
        label = np.full((300, 300, 3), 200, np.uint8)
        tif.write(label, description="Aperio Image Library v12.0.15\nlabel 300x300", compression="lzw", metadata=None)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "test.svs")
