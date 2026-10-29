#!/usr/bin/env python3
"""Adapt the QWC2 stock app's config.json to this standalone image.

The stock app expects the qwc-services backends (auth, search, editing,
permalinks, reports, ...) which this image doesn't run. Point those service
URLs nowhere and drop the plugins and menu entries that need them, so the UI
only offers what actually works against plain QGIS Server.
"""
import json
import sys

# Plugins that need a qwc-services backend, third-party services, or
# configuration this image doesn't provide.
REMOVE = {
    "Authentication", "AttributeTable", "Bookmark", "Editing", "FeatureForm",
    "FeatureSearch", "HeightProfile", "LayerCatalog", "MapInfoTooltip",
    "NewsPopup", "Panoramax", "Portal", "Print", "Reports", "Routing",
    "TourGuide", "View3D",
}
SERVICE_KEYS = (
    "searchServiceUrl", "searchDataServiceUrl", "editServiceUrl",
    "mapInfoService", "permalinkServiceUrl", "elevationServiceUrl",
    "featureReportService", "authServiceUrl", "routingServiceUrl",
)


def prune_items(items):
    """Drop menu/toolbar entries for removed plugins and external links."""
    kept = []
    for item in items:
        if item.get("key") in REMOVE or "url" in item:
            continue
        if "subitems" in item:
            item["subitems"] = prune_items(item["subitems"])
            if not item["subitems"]:
                continue
        kept.append(item)
    return kept


def main(path):
    with open(path, encoding="utf-8") as fh:
        config = json.load(fh)

    missing = [k for k in SERVICE_KEYS if k not in config]
    if missing:
        sys.exit(f"configure.py: unexpected config.json, missing {missing}")
    for key in SERVICE_KEYS:
        config[key] = ""
    config["trustWmsCapabilityURLs"] = False

    for group, plugins in config["plugins"].items():
        plugins[:] = [p for p in plugins if p["name"] not in REMOVE]
        for plugin in plugins:
            cfg = plugin.get("cfg", {})
            for key in ("menuItems", "toolbarItems"):
                if key in cfg:
                    cfg[key] = prune_items(cfg[key])
            cfg.pop("termsUrl", None)  # BottomBar link to the QWC project site

    with open(path, "w", encoding="utf-8") as fh:
        json.dump(config, fh, indent=2, ensure_ascii=False)
        fh.write("\n")


if __name__ == "__main__":
    main(sys.argv[1])
