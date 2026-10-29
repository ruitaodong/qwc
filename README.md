# qwc — QGIS Server + QWC2 in Docker

A single container that serves raster/vector files as web maps: Apache with
[QGIS Server](https://docs.qgis.org/latest/en/docs/server_manual/) (QGIS 3.40 LTR
on Debian 13 "trixie") and [QWC2](https://qwc.app), the current QGIS web client,
plus a small tool (`qgs.py`) that turns a set of files into a QGIS project.

There is no PHP in the image: QWC2 is a static JavaScript app, and the list of
maps it offers (`themes.json`) is generated ahead of time.

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

## The theme list

QWC2 reads its list of maps from `themes.json`. It is rebuilt automatically when
the container starts and every time `qgs.py` writes a project. If you put
`.qgs` files into `/web` some other way, refresh it yourself:

```sh
docker exec qwc update-themes
```

Projects QGIS Server can't open are left out and reported (details in
`docker logs qwc`); the previous list is kept if the server isn't reachable.

## `qgs.py` options

```
qgs.py [--crs EPSG:3857] [--keep-layer-crs] [--no-themes] output.qgs input [input ...]
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
the image: `docker cp qwc:/QWC2/config.json .`):

```sh
docker run ... -v ./config.json:/QWC2/config.json:ro ghcr.io/ruitaodong/qwc
```

Apache access logs go to stdout and Apache/QGIS Server errors to stderr, so
everything shows up in `docker logs qwc`. The container has a health check.

## Running it on a LAN or a tailnet

The image speaks plain HTTP on port 80 and is meant to sit on a private
network. Two setups work well.

**LAN.** Publish the port on the LAN interface only:

```sh
docker run -d --name=qwc --restart=unless-stopped \
  -p 192.168.1.20:8080:80 -v data-dir:/web ghcr.io/ruitaodong/qwc
```

Use the host's LAN address rather than a bare `-p 8080:80`, which listens on
every interface. Docker's published ports bypass host firewalls such as ufw, so
a firewall rule won't save you if the host also has a public interface.

**Tailscale.** Keep the container on loopback and let Tailscale add HTTPS:

```sh
docker run -d --name=qwc --restart=unless-stopped \
  -p 127.0.0.1:8080:80 -v data-dir:/web ghcr.io/ruitaodong/qwc
tailscale serve --bg localhost:8080
```

The maps are then at `https://<machine>.<tailnet>.ts.net/`, reachable only from
your tailnet (narrow it further with Tailscale ACLs). HTTPS matters for more
than privacy: browsers only allow QWC2's *locate me* button and copy-link
buttons on secure pages, so over plain `http://192.168.x.x` those two don't
work. Don't use `tailscale funnel`, which publishes the site to the internet.

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
docker compose exec qwc qgs.py demo.qgs image.png overlay.json
```

The image is built from a single multi-stage `Dockerfile`. Stage 1 downloads a
pinned QWC2 LTS release (the prebuilt stock app plus QWC2's theme generator,
both checksum-verified) and adapts its `config.json`
(`etc/qwc2/configure.py`); stage 2 installs Apache and QGIS Server from Debian.
To move to a newer QWC2, update the `QWC2_*` build arguments.

CI (`.github/workflows/docker-publish.yml`) builds the image and smoke-tests it
(no PHP present, project creation, theme generation, client page, redirects,
WMS GetProjectSettings and GetMap, theme rebuild on restart), then publishes
signed multi-arch images (`linux/amd64`, `linux/arm64`) with SBOM and
provenance to `ghcr.io`. Tags: `latest` and `master` from the default branch,
`X.Y.Z` / `X.Y` from `vX.Y.Z` git tags, and a dated tag from the weekly rebuild
that picks up Debian security updates. Dependabot keeps the (SHA-pinned)
actions current.

Verify an image signature with:

```sh
cosign verify ghcr.io/ruitaodong/qwc:latest \
  --certificate-identity-regexp 'https://github.com/ruitaodong/qwc/' \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com
```
