# qwc2 — QGIS Server + QWC2 in Docker

This is the `qwc2` branch of [ruitaodong/qwc](https://github.com/ruitaodong/qwc); it publishes `ghcr.io/ruitaodong/qwc2`.

A single container that serves raster/vector files, including digital-pathology
whole-slide images, as web maps: Apache with
[QGIS Server](https://docs.qgis.org/latest/en/docs/server_manual/) (QGIS 3.40 LTR
on Debian 13 "trixie") and [QWC2](https://qwc.app), the current QGIS web client,
plus a small tool (`qgs.py`) that turns a set of files into a QGIS project.

There is no PHP in the image: QWC2 is a static JavaScript app, and the list of
maps it offers (`themes.json`) is generated ahead of time.

## Quick start

```sh
docker pull ghcr.io/ruitaodong/qwc2
docker run --name=qwc2 --rm -d -p 8080:80 -v data-dir:/web ghcr.io/ruitaodong/qwc2
```

Put your data into the `/web` volume, then create one project per map:

```sh
docker exec qwc2 qgs.py name1.qgs image1.png vector1.json vector2.json
docker exec qwc2 qgs.py name2.qgs image2.png vector3.json vector4.json
```

Open <http://localhost:8080>. QWC2 opens the first project; its theme switcher
(*Map & Tools* → *Theme*) lists `name1` and `name2`. Projects are stacked in the order
given, so the last file is drawn on top: list the background image first.
Running `qgs.py` again with the same name replaces the project.

| URL | What it is |
| --- | --- |
| `/` | QWC2, opened on the default project |
| `/?t=<name>` | QWC2 opened on `/web/<name>.qgs` |
| `/wms/<name>` | WMS endpoint for `/web/<name>.qgs` (e.g. `?SERVICE=WMS&REQUEST=GetCapabilities`) |
| `/maps/<name>` | old QGIS Web Client link, redirects to `/?t=<name>` |

## Whole-slide images

Slides are read through two GDAL plugins built into the image, which serve
each slide's own pyramid levels as overviews, so zoomed-out views stay fast
however large the slide is: an OpenSlide plugin
([`gdal-openslide/`](gdal-openslide/)) for most formats, and a libisyntax
plugin ([`gdal-isyntax/`](gdal-isyntax/)) for Philips iSyntax, which OpenSlide
can't read. `qgs.py` recognises slides by itself:

```sh
docker exec qwc2 qgs.py biopsy.qgs biopsy.svs
```

Supported (OpenSlide 3.4.1, as shipped by Debian): Aperio `.svs`/`.tif`,
Hamamatsu `.ndpi`/`.vms`/`.vmu`, Leica `.scn`, MIRAX `.mrxs`, Philips `.tiff`,
Sakura `.svslide`, Trestle `.tif`, Ventana `.bif`/`.tif`; and Philips
iSyntax `.isyntax` through libisyntax. A slide stored as a
*generic* tiled TIFF looks like any other TIFF, so name it explicitly:
`qgs.py slide.qgs OPENSLIDE:slide.tif`. Formats added in OpenSlide 4
(DICOM, Zeiss `.czi`) are not available. MIRAX slides are an `.mrxs` file plus
a directory of the same name; copy both into `/web`.

Slides have no geographic position: map units are slide pixels (level 0),
with the top-left corner at 0,0 and y negative downwards, so QWC2's
coordinate display shows pixel positions and its scale bar's "metres" are
pixels. Annotations to overlay need to use the same pixel coordinates. The
physical pixel size is in the layer metadata (`openslide.mpp-x`, microns).

## Demo: two scanners, one tissue microarray

`demo/` is a small page showing two scans of the same tissue microarray from
the [PLISM dataset](https://github.com/ruitaodong/plism-icc/releases/tag/PLISM_wsi),
one per Aperio scanner (AT2 and GT450).

**In the container, at full resolution.** Install the slides (about 3.3 GB,
checksum-verified, skipped if already present) and open `/demo/`:

```sh
docker exec qwc2 demo-slides
# then http://<host>:8080/demo/
```

**As a static site, no server.** `build-static-demo` pre-renders both slides to
map tiles and writes a self-contained copy of QWC2 that any static host can
serve. Zoom stops at about 0.6 µm per pixel (roughly a 17× objective) so the
site stays under GitHub Pages' 1 GB limit. The
[Pages workflow](.github/workflows/pages.yml) builds and publishes it to
`https://<owner>.github.io/<repo>/demo/` on demand or when the demo changes.
To enable it: Settings → Pages → Source "GitHub Actions", and allow the
`qwc2` branch under Settings → Environments → github-pages.

The scans aren't aligned with each other (different scan areas and pixel
sizes), and colours are the scanners' raw values: the AT2's embedded ICC
profile isn't applied.

## The theme list

QWC2 reads its list of maps from `themes.json`. It is rebuilt automatically when
the container starts and every time `qgs.py` writes a project. If you put
`.qgs` files into `/web` some other way, refresh it yourself:

```sh
docker exec qwc2 update-themes
```

Projects QGIS Server can't open are left out and reported (details in
`docker logs qwc2`); the previous list is kept if the server isn't reachable.

## `qgs.py` options

```
qgs.py [--crs EPSG:3857] [--keep-layer-crs] [--no-themes] output.qgs input [input ...]
       (an input can be OPENSLIDE:file to force reading it as a slide)
```

`--crs` sets the project CRS (default `EPSG:3857`). By default every layer is
also *assigned* that CRS, which suits images with a world file but no `.prj` and
GeoJSON whose coordinates are in Web Mercator. With `--keep-layer-crs`, layers
that already declare a CRS (a `.prj`, a GeoTIFF, standard lon/lat GeoJSON, …)
keep it and are reprojected on the fly; only layers without one get `--crs`.
`--no-themes` skips the theme list refresh (useful when creating many projects
in a row; run `update-themes` once at the end).

QGIS 3 stores a small style database next to each project as
`<name>_attachments.zip`; keep it together with the `.qgs` file.

## Configuration

| Environment variable | Default | Meaning |
| --- | --- | --- |
| `QWC_DEFAULT_THEME` | first project by name | project QWC2 opens at `/` |
| `QGIS_SERVER_LOG_LEVEL` | `1` | QGIS Server log level: 0 info, 1 warning, 2 critical |
| `QWC_CORS_ORIGIN` | unset (no CORS) | origin allowed to read the WMS from another web app, e.g. `https://app.example` |

The client is the QWC2 stock app, trimmed to what works against plain QGIS
Server: layer tree, legend, feature info, measuring, drawing, map export,
compare/swipe, filters, time manager and sharing links. Features that need the
[qwc-services](https://github.com/qwc-services/qwc-docker) backends
(login, editing, search, permalinks, reports) or third-party services (routing,
street-level imagery) are switched off, so the app makes no requests outside
your server. Printing is off because `qgs.py` projects have no print layout.

To customise the client, mount your own `config.json` (start from the one in
the image: `docker cp qwc2:/QWC2/config.json .`):

```sh
docker run ... -v ./config.json:/QWC2/config.json:ro ghcr.io/ruitaodong/qwc2
```

Apache access logs go to stdout and Apache/QGIS Server errors to stderr, so
everything shows up in `docker logs qwc2`. The container has a health check.

## Running it on a LAN or a tailnet

The image speaks plain HTTP on port 80 and is meant to sit on a private
network. Two setups work well.

**LAN.** Publish the port on the LAN interface only:

```sh
docker run -d --name=qwc2 --restart=unless-stopped \
  -p 192.168.1.20:8080:80 -v data-dir:/web ghcr.io/ruitaodong/qwc2
```

Use the host's LAN address rather than a bare `-p 8080:80`, which listens on
every interface. Docker's published ports bypass host firewalls such as ufw, so
a firewall rule won't save you if the host also has a public interface.

**Tailscale.** Keep the container on loopback and let Tailscale add HTTPS:

```sh
docker run -d --name=qwc2 --restart=unless-stopped \
  -p 127.0.0.1:8080:80 -v data-dir:/web ghcr.io/ruitaodong/qwc2
tailscale serve --bg localhost:8080
```

The maps are then at `https://<machine>.<tailnet>.ts.net/`, reachable only from
your tailnet (narrow it further with Tailscale ACLs). HTTPS matters for more
than privacy: browsers only allow QWC2's *locate me* button and copy-link
buttons on secure pages, so over plain `http://192.168.x.x` those two don't
work. Don't use `tailscale funnel`, which publishes the site to the internet.

**One hostname on both.** If the server's short name resolves on both networks
(LAN DNS on the LAN, the MagicDNS search domain on the tailnet), publish the
port on every interface so one URL, `http://<host>:8080/`, works everywhere:

```sh
docker run -d --name=qwc2 --restart=unless-stopped \
  -p 8080:80 -v data-dir:/web ghcr.io/ruitaodong/qwc2
```

This is fine when the host's only interfaces are the LAN and Tailscale (a
typical server behind a home or office router). Binding the LAN and Tailscale
IPs individually (`-p 192.168.1.20:8080:80 -p 100.x.y.z:8080:80`) also works,
but the container can then fail to start at boot if Docker comes up before
Tailscale has its address. Links, redirects and WMS URLs follow whichever name
the browser used, so shared links keep working for anyone with the same search
domain. HTTPS isn't available on a short name (the Tailscale certificate is for
`<host>.<tailnet>.ts.net`), so QWC2's *locate me* button won't work there; use
the full tailnet name with `tailscale serve` if you need it.

Both setups are handled correctly behind the proxy: the client only uses
relative URLs, redirects and QGIS Server's advertised WMS URLs honour
`X-Forwarded-Host`/`X-Forwarded-Proto`, so desktop GIS clients connecting to
`https://<machine>.<tailnet>.ts.net/wms/<name>` get working HTTPS links back.

The WMS sends no CORS header unless you set `QWC_CORS_ORIGIN`. On a private
network a wildcard would let any website a user happens to visit read your
maps through their browser; desktop GIS clients don't need CORS at all.

## Development

```sh
docker compose up --build          # serves ./web on http://localhost:8080 (loopback only)
docker compose exec qwc2 qgs.py demo.qgs image.png overlay.json
```

The image is built from a single multi-stage `Dockerfile`. Stage 1 downloads a
pinned QWC2 LTS release (the prebuilt stock app plus QWC2's theme generator,
both checksum-verified) and adapts its `config.json`
(`etc/qwc2/configure.py`); stage 2 compiles the OpenSlide GDAL plugin against
Debian's GDAL; stage 3 installs Apache and QGIS Server from Debian and the
plugin, and fails the build if GDAL can't load it.
To move to a newer QWC2, update the `QWC2_*` build arguments.

CI (`.github/workflows/docker-publish.yml`) builds the image and smoke-tests it
(no PHP present, project creation, theme generation, client page, redirects,
WMS GetProjectSettings and GetMap, theme rebuild on restart, and a synthetic
Aperio slide through the OpenSlide plugin, checked pixel by pixel, and
libisyntax's public iSyntax test slide, checked for pyramid alignment), then publishes
signed multi-arch images (`linux/amd64`, `linux/arm64`) with SBOM and
provenance to `ghcr.io/ruitaodong/qwc2`. Tags: `latest` and `qwc2` from this
branch, `X.Y.Z` / `X.Y` from `vX.Y.Z` git tags on this branch, and a dated tag
from manual or weekly rebuilds.

Two GitHub features only read their configuration from the repository's
default branch, so for this branch they need a copy there:

- **Weekly rebuild** (Debian security updates): copy
  `.github/workflows/weekly-rebuild-qwc2.yml` to the default branch.
- **Dependabot** updates for this branch's actions: add an entry with
  `target-branch: qwc2` to the default branch's `.github/dependabot.yml`.

Verify an image signature with:

```sh
cosign verify ghcr.io/ruitaodong/qwc2:latest \
  --certificate-identity-regexp 'https://github.com/ruitaodong/qwc/' \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com
```
