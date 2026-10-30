# qwc — QGIS Server + QGIS Web Client in Docker

A single container that serves raster/vector files as web maps: Apache with
[QGIS Server](https://docs.qgis.org/latest/en/docs/server_manual/) (QGIS 3.40 LTR
on Debian 13 "trixie") and the classic ExtJS-based
[QGIS Web Client](https://github.com/qgis/qgis-web-client), plus a small tool
(`qgs.py`) that turns a set of files into a QGIS project.

## Quick start

```sh
docker pull ghcr.io/ruitaodong/qwc
docker run --name=qwc --rm -d -p 8080:80 -v data-dir:/web ghcr.io/ruitaodong/qwc
```

Put your data into the `/web` volume, then create one project per map:

```sh
docker exec qwc qgs.py name1.qgs image1.png vector1.json vector2.json
docker exec qwc qgs.py name2.qgs image2.png vector3.json vector4.json
```

Open <http://localhost:8080>: it lists `name1` and `name2`. Clicking `name1` shows
`image1.png`, `vector1.json` and `vector2.json`; clicking `name2` shows
`image2.png`, `vector3.json` and `vector4.json`.

Layers are stacked in the order given, so the last file is drawn on top: list the
background image first. Running `qgs.py` again with the same project name
replaces the project, and QGIS Server picks up the change automatically.
(`qgs.sh` still works as an alias for existing scripts.)

| URL | What it is |
| --- | --- |
| `/` | list of projects in `/web` |
| `/maps/<name>` | web client for `/web/<name>.qgs` |
| `/wms/<name>` | WMS endpoint for `/web/<name>.qgs` (e.g. `?SERVICE=WMS&REQUEST=GetCapabilities`) |

## `qgs.py` options

```
qgs.py [--crs EPSG:3857] [--keep-layer-crs] output.qgs input [input ...]
```

`--crs` sets the project CRS (default `EPSG:3857`, the web client's map
projection). By default every layer is also *assigned* that CRS, which suits
images with a world file but no `.prj` and GeoJSON whose coordinates are in
Web Mercator. With `--keep-layer-crs`, layers that already declare a CRS (a
`.prj`, a GeoTIFF, standard lon/lat GeoJSON, …) keep it and are reprojected on
the fly; only layers without one get `--crs`.

QGIS 3 stores a small style database next to each project as
`<name>_attachments.zip`; keep it together with the `.qgs` file.

## Configuration

| Environment variable | Default | Meaning |
| --- | --- | --- |
| `QGIS_SERVER_LOG_LEVEL` | `1` | QGIS Server log level: 0 info, 1 warning, 2 critical |

Apache access logs go to stdout and Apache/QGIS Server errors to stderr, so
everything shows up in `docker logs qwc`. The container has a health check.

## Development

```sh
docker compose up --build          # serves ./web on http://localhost:8080
docker compose exec qwc qgs.py demo.qgs image.png overlay.json
```

The image is built from a single multi-stage `Dockerfile`. Stage 1 downloads
QGIS Web Client at a pinned commit and patches its configuration (each patch is
verified, so the build fails if anything doesn't apply); stage 2 installs
Apache, PHP, QGIS Server and PyQGIS from Debian.

CI (`.github/workflows/docker-publish.yml`) builds the image, runs a smoke test
against it (project creation, listing, client page, WMS GetProjectSettings and
GetMap), then publishes signed multi-arch images (`linux/amd64`,
`linux/arm64`) with SBOM and provenance to `ghcr.io`. Tags: `latest` and
`master` from the default branch, `X.Y.Z` / `X.Y` from `vX.Y.Z` git tags, and a
dated tag from the weekly rebuild that picks up Debian security updates.
Dependabot keeps the (SHA-pinned) actions current.

Verify an image signature with:

```sh
cosign verify ghcr.io/ruitaodong/qwc:latest \
  --certificate-identity-regexp 'https://github.com/ruitaodong/qwc/' \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com
```

## Note on the web client

QGIS Web Client (v1) has been archived upstream for years; this image pins its
last useful commit and patches it to work with QGIS 3 Server. Its successor is
[QWC2](https://github.com/qgis/qwc2), a modern React client, which would be the
natural next step if the old ExtJS interface becomes a limitation.
