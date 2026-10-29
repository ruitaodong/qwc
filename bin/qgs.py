#!/usr/bin/env python3
"""Create a QGIS project (.qgs) from raster and vector files.

The resulting project is served by QGIS Server and listed by QGIS Web Client.

    qgs.py [--crs EPSG:3857] [--keep-layer-crs] output.qgs input1 [input2 ...]

Inputs are added in order, so the last one is drawn on top (e.g. pass the
background image first, then the vector overlays).
"""

import argparse
import os
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
    p.add_argument("output", type=Path, help="project file to (over)write, e.g. name.qgs")
    p.add_argument("inputs", type=Path, nargs="+", help="raster and/or vector files")
    return p.parse_args(argv)


def load_layer(path: Path) -> QgsMapLayer | None:
    """Open *path* as a vector layer, falling back to a raster layer."""
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
    inputs = [p.resolve() for p in args.inputs]

    missing = [str(p) for p in inputs if not p.exists()]
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
        project.setTitle(output.stem)

        for path in inputs:
            layer = load_layer(path)
            if layer is None:
                print(f"error: {path} is neither a readable vector nor raster file",
                      file=sys.stderr)
                return 1
            if not args.keep_layer_crs or not layer.crs().isValid():
                layer.setCrs(crs)
            project.addMapLayer(layer)
            kind = "vector" if isinstance(layer, QgsVectorLayer) else "raster"
            print(f"added {path.name} as {kind} layer ({layer.crs().authid()})")

        output.parent.mkdir(parents=True, exist_ok=True)
        output.unlink(missing_ok=True)  # overwrite without leaving a .qgs~ backup
        if not project.write(str(output)):
            print(f"error: could not write {output}: {project.error()}", file=sys.stderr)
            return 1
        print(f"wrote {output}")
        return 0
    finally:
        if project is not None:
            project.clear()  # release layers before shutdown (avoids a segfault)
        app.exitQgis()


if __name__ == "__main__":
    sys.exit(main())
