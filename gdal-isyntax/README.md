# gdal-isyntax — GDAL driver for Philips iSyntax

A GDAL raster plugin for Philips iSyntax whole-slide images (`.isyntax`, from
Philips IntelliSite scanners), built on the open-source
[libisyntax](https://github.com/amspath/libisyntax) (BSD-2-Clause). OpenSlide
can't read iSyntax; with this plugin QGIS, QGIS Server, `gdalinfo` and other
GDAL tools can.

It mirrors the [OpenSlide driver](../gdal-openslide/):

- **The pyramid as GDAL overviews**, so zoomed-out views read the small levels.
- **4 bands, RGBA.** Unscanned areas come back white and opaque.
- **Level-0 pixel coordinates**, origin top-left, y down the slide
  (geotransform `0, 1, 0, 0, 0, -1`). No CRS.
- **Metadata:** `isyntax.mpp-x`/`mpp-y` (microns per pixel), scanner, model,
  serial number, acquisition time, software versions, compression.
- **The slide's ICC colour profile**, in the `COLOR_PROFILE` metadata domain as
  `SOURCE_ICC_PROFILE` (base64), the way GDAL's TIFF, JPEG and PNG drivers
  report one.
- **Label and macro images** as subdatasets (`ISYNTAX_ASSOC:label:/path/slide.isyntax`).

```sh
gdalinfo slide.isyntax
gdal_translate -outsize 2000 0 slide.isyntax overview.png
gdalinfo -mdd COLOR_PROFILE slide.isyntax      # the ICC profile
```

## Configuration options

| Option | Default | Meaning |
| --- | --- | --- |
| `ISYNTAX_CACHE_SIZE` | `500` | Tiles libisyntax keeps decoded per open slide, very roughly 0.4 MB each. Each QGIS Server process has its own. |
| `ISYNTAX_EXPOSE_BARCODE` | `NO` | `YES` to report the slide barcode as `isyntax.barcode`. Off by default because on clinical slides it often identifies the specimen or patient. The label image may show the same information. |

## Limitations

- Needs real local files, like libisyntax itself: GDAL's virtual file systems
  (`/vsicurl/`, `/vsizip/`, …) don't work.
- Read-only. iSyntax files are opened in their default (non-"display") view.

## Building

Needs a C++17 compiler, CMake, GDAL development files
(Debian/Ubuntu: `g++ make cmake pkg-config libgdal-dev`) and a libisyntax
source tree.

```sh
git clone https://github.com/amspath/libisyntax ../libisyntax
make build-libisyntax LIBISYNTAX=../libisyntax
make LIBISYNTAX=../libisyntax
make check LIBISYNTAX=../libisyntax SLIDE=/path/slide.isyntax
sudo make install LIBISYNTAX=../libisyntax
```

`build-libisyntax` matters for a plugin. It builds libisyntax as a static
library for the architecture baseline: upstream's build uses `-march=native`,
which ties the binary to the build machine's CPU. It also disables its bundled
allocator's override of C++ `new`/`delete`, which would otherwise take over
memory allocation for the whole host process (QGIS, GDAL). The plugin keeps
libisyntax's symbols private, exporting only its registration functions.

A plugin must be built against the same GDAL major/minor version it runs with;
the driver refuses to register otherwise.
