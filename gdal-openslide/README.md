# gdal-openslide — GDAL driver for whole-slide images

A GDAL raster plugin that reads digital-pathology whole-slide images through
[OpenSlide](https://openslide.org/), so QGIS, QGIS Server, `gdalinfo`,
`gdal_translate` and anything else built on GDAL can open them efficiently.

What it gives you over opening a slide with GDAL's GeoTIFF driver:

- **The slide's pyramid as GDAL overviews.** Zoomed-out views read the small
  levels instead of decoding the full-resolution image (often 100,000 px+).
- **Formats GDAL can't read at all**: Hamamatsu `.vms`/`.vmu`, MIRAX `.mrxs`,
  and others whose image data isn't plain TIFF.
- **4 bands, RGBA**: OpenSlide's premultiplied ARGB is un-premultiplied, and
  empty regions (e.g. between MIRAX scan areas) are transparent.
- **Metadata**: every OpenSlide property (`openslide.mpp-x`, `aperio.AppMag`,
  …), and the associated images (label, macro, thumbnail) as subdatasets.

Formats are whatever the installed OpenSlide supports. Debian 13 ships
OpenSlide 3.4.1: Aperio, Hamamatsu, Leica, MIRAX, Philips, Sakura, Trestle,
Ventana and generic tiled TIFF.

## Opening slides

| Dataset name | Opens |
| --- | --- |
| `OPENSLIDE:/path/slide.svs` | the slide, any OpenSlide format |
| `/path/slide.mrxs` | the slide, without prefix, if OpenSlide recognises a vendor format |
| `OPENSLIDE_ASSOC:label:/path/slide.svs` | an associated image (see `SUBDATASETS` in `gdalinfo`) |

Use the prefix for TIFF-based slides (`.svs`, `.ndpi`, `.scn`, `.tif`, …):
depending on how GDAL orders its drivers, its GeoTIFF driver may claim them
first and read only the full-resolution level. Without the prefix the driver
never claims *generic* tiled TIFFs, so ordinary GeoTIFFs keep their
georeferencing.

Coordinates are level-0 pixels, origin at the top-left corner with y
increasing down the slide (geotransform `0, 1, 0, 0, 0, -1`), so slides appear
upright. There is no CRS. Physical pixel size is in the `openslide.mpp-x` /
`openslide.mpp-y` metadata (microns per pixel).

```sh
gdalinfo OPENSLIDE:slide.svs
gdal_translate -outsize 2000 0 OPENSLIDE:slide.svs overview.png
CPL_DEBUG=OpenSlide gdal_translate ...     # shows which level each read uses
```

## Building

Needs a C++17 compiler and the GDAL and OpenSlide development files
(Debian/Ubuntu: `g++ make pkg-config libgdal-dev libopenslide-dev`).

```sh
make
make check SLIDE=/path/slide.svs   # gdalinfo through the freshly built plugin
sudo make install                  # into $(gdal-config --plugindir)
```

GDAL loads `gdal_OpenSlide.so` from its plugin directory (or from
`GDAL_DRIVER_PATH`). A plugin must be built against the same GDAL major/minor
version it runs with; the driver refuses to register otherwise.

`../tests/make_test_svs.py` writes a small synthetic Aperio slide for testing.
