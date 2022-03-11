#!/usr/bin/env python

import os, sys

from qgis.core import *
from qgis.gui import QgsMapCanvas, QgsLayerTreeMapCanvasBridge

def writeOldLegend(doc):
    legendElem = QgsLayerTreeUtils().writeOldLegend(doc, root, False, [None])
    doc.firstChild().appendChild(legendElem)

if '__main__'  == __name__:
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument('--crs', default='EPSG:3857')
    p.add_argument('output', default='output.qgs')
    p.add_argument('inputs', nargs=argparse.REMAINDER)
    args = p.parse_args()

    fQgs = args.output
    fDir, _ = os.path.split(fQgs)

    if os.path.isfile(fQgs):
        os.remove(fQgs)

    if fDir:                            # ''
        os.chdir(fDir)

    app = QgsApplication([], True)
    app.initQgis()

    project = QgsProject.instance()
    project.setFileName(fQgs)

    crs = QgsCoordinateReferenceSystem(args.crs)

    canvas = QgsMapCanvas()
    root = project.layerTreeRoot()
    maps = QgsMapLayerRegistry.instance()

    bridge = QgsLayerTreeMapCanvasBridge(root, canvas)

    for fName in args.inputs:
        # Assume all in the same directory as fQgs
        fBase, fExt = os.path.splitext(os.path.split(fName)[1])
        if fExt in ('.shp', '.xml', '.json'):
            layer = QgsVectorLayer(fName, fBase, 'ogr')
        else:
            layer = QgsRasterLayer(fName, fBase)
            layer.setContrastEnhancement(QgsContrastEnhancement.NoEnhancement)
        layer.setCrs(crs)
        maps.addMapLayer(layer)

    bridge.setCanvasLayers()

    project.writeProject.connect(bridge.writeProject)
    project.writeProject.connect(writeOldLegend)

    project.write()

    app.exitQgis()
