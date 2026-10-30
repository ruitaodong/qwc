#!/usr/bin/env python3
"""Create a QGIS project (.qgs) from raster and vector files.

The project is served by QGIS Server and, once the theme list is refreshed
(done automatically unless --no-themes), shows up in QWC2.

    qgs.py [--crs EPSG:3857] [--keep-layer-crs] output.qgs input1 [input2 ...]

Inputs are added in order, so the last one is drawn on top (e.g. pass the
background image first, then the vector overlays).

Whole-slide images (Aperio .svs, Hamamatsu .ndpi/.vms, MIRAX .mrxs, Leica,
...) are opened through the OpenSlide GDAL driver automatically. A slide in
generic tiled TIFF form can't be told apart from an ordinary TIFF, so name it
explicitly: OPENSLIDE:slide.tif
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

# QGIS 3 runs headless with Qt's offscreen platform: no X server / xvfb needed.
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from qgis.core import (  # noqa: E402  (must follow the environment setup)
    QgsApplication,
    QgsContrastEnhancement,
    QgsCoordinateReferenceSystem,
    QgsMapLayer,
    QgsProject,
    QgsRasterLayer,
    QgsVectorLayer,
)


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--crs", default="EPSG:3857",
                   help="project CRS, also assigned to the layers (default: %(default)s)")
    p.add_argument("--keep-layer-crs", action="store_true",
                   help="only assign --crs to layers that have no CRS of their own "
                        "(by default every layer is forced to --crs, as before)")
    p.add_argument("--title", help="map title shown in QWC2 (default: the output file's name)")
    p.add_argument("--no-themes", action="store_true",
                   help="don't refresh the QWC2 theme list afterwards")
    p.add_argument("output", type=Path, help="project file to (over)write, e.g. name.qgs")
    p.add_argument("inputs", nargs="+",
                   help="raster, vector and/or whole-slide files (OPENSLIDE:file to force a slide)")
    return p.parse_args(argv)


OPENSLIDE = "OPENSLIDE:"


def is_slide(path: Path) -> bool:
    """True if the OpenSlide GDAL driver recognises *path* as a slide."""
    try:
        from osgeo import gdal
    except ImportError:
        return False
    if gdal.GetDriverByName("OpenSlide") is None:
        return False
    with gdal.quiet_errors():
        return gdal.IdentifyDriverEx(str(path), allowed_drivers=["OpenSlide"]) is not None


def load_layer(path: Path, slide: bool = False) -> QgsMapLayer | None:
    """Open *path* as a slide, vector layer or raster layer, in that order."""
    if slide or is_slide(path):
        # The prefix makes sure OpenSlide (with its pyramid as overviews)
        # reads the file, not GDAL's GeoTIFF driver at full resolution only.
        layer = QgsRasterLayer(OPENSLIDE + str(path), path.name, "gdal")
        if layer.isValid():
            return layer
        if slide:
            return None
    vector = QgsVectorLayer(str(path), path.name, "ogr")
    if vector.isValid():
        return vector
    raster = QgsRasterLayer(str(path), path.name)
    if raster.isValid():
        raster.setContrastEnhancement(QgsContrastEnhancement.NoEnhancement)
        return raster
    return None


def main(argv=None) -> int:
    args = parse_args(argv)
    output = args.output.resolve()
    # (path, forced through OpenSlide?) for each input, OPENSLIDE: prefix stripped
    inputs = []
    for arg in args.inputs:
        text = str(arg)
        forced = text.upper().startswith(OPENSLIDE)
        inputs.append((Path(text[len(OPENSLIDE):] if forced else text).resolve(), forced))

    missing = [str(p) for p, _ in inputs if not p.exists()]
    if missing:
        print("error: input not found:", ", ".join(missing), file=sys.stderr)
        return 2

    # Without the prefix path QGIS can't find its CRS database, so layers'
    # own CRS would silently read as invalid.
    QgsApplication.setPrefixPath(os.environ.get("QGIS_PREFIX_PATH", "/usr"), True)
    app = QgsApplication([], False)
    app.initQgis()
    project = None
    try:
        # QGIS objects (CRS included) may only be created once QGIS is initialised
        crs = QgsCoordinateReferenceSystem(args.crs)
        if not crs.isValid():
            print(f"error: invalid CRS {args.crs!r}", file=sys.stderr)
            return 2

        project = QgsProject.instance()
        project.setCrs(crs)
        project.setTitle(args.title or output.stem)

        for path, forced in inputs:
            layer = load_layer(path, slide=forced)
            if layer is None:
                what = "an OpenSlide slide" if forced else "a readable vector or raster file"
                print(f"error: {path} is not {what}", file=sys.stderr)
                return 1
            if not args.keep_layer_crs or not layer.crs().isValid():
                layer.setCrs(crs)
            project.addMapLayer(layer)
            kind = ("vector" if isinstance(layer, QgsVectorLayer)
                    else "slide" if layer.source().upper().startswith(OPENSLIDE)
                    else "raster")
            print(f"added {path.name} as {kind} layer ({layer.crs().authid()})")

        output.parent.mkdir(parents=True, exist_ok=True)
        output.unlink(missing_ok=True)  # overwrite without leaving a .qgs~ backup
        if not project.write(str(output)):
            print(f"error: could not write {output}: {project.error()}", file=sys.stderr)
            return 1
        print(f"wrote {output}")
    finally:
        if project is not None:
            project.clear()  # release layers before shutdown (avoids a segfault)
        app.exitQgis()

    if args.no_themes or not shutil.which("update-themes"):
        return 0
    if subprocess.run(["update-themes"]).returncode != 0:
        print("warning: project written, but the QWC2 theme list could not be "
              "refreshed; run update-themes once the server is up", file=sys.stderr)
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
