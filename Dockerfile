# syntax=docker/dockerfile:1

ARG DEBIAN_RELEASE=trixie

# ---------------------------------------------------------------------------
# Stage 1: fetch and configure QGIS Web Client (archived upstream, pinned)
# ---------------------------------------------------------------------------
FROM debian:${DEBIAN_RELEASE}-slim AS qwc

ARG QWC_COMMIT=bf160af0562cf074eb295ebdbd890b3029ebce6f
SHELL ["/bin/bash", "-o", "pipefail", "-c"]

# hadolint ignore=DL3008
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /qgis-web-client
RUN curl -fsSL "https://codeload.github.com/qgis/qgis-web-client/tar.gz/${QWC_COMMIT}" \
    | tar -xz --strip-components=1

# Each edit is checked, so the build fails loudly if upstream ever changes.
# hadolint ignore=SC2016
RUN set -eu; \
    opts=site/js/GlobalOptions.js; init=site/js/WebgisInit.js; \
    html=site/qgiswebclient.html; php=site/index.php; \
    sed -i 's|var serverAndCGI = "/cgi-bin/qgis_mapserv.fcgi"|var serverAndCGI = "/wms"|' "$opts"; \
    sed -i 's|var enableOSMMaps = true;|var enableOSMMaps = false;|' "$opts"; \
    sed -i 's|var enableGoogleCommercialMaps = true;|var enableGoogleCommercialMaps = false;|' "$opts"; \
    sed -i 's|<absolute-path-to-qgis-server-projects>/|/web/|' "$php"; \
    # PHP 8: end() needs a variable, use basename() instead
    sed -i 's|\$filename = end(explode(./., \$filename));|$filename = basename($filename);|' "$php"; \
    # Google Maps is disabled, so don't load its API (needs a key nowadays anyway)
    sed -i '\|maps.googleapis.com/maps/api/js|d' "$html"; \
    # Serve the minified ExtJS builds instead of the debug ones
    sed -i 's|ext-base-debug.js|ext-base.js|; s|ext-all-debug-w-comments.js|ext-all.js|' "$html"; \
    # QGIS 3 Server lists the root extent in several CRSs (QGIS 2 listed one);
    # use the one matching the client's map projection (authid)
    sed -i 's|if (key.match(/^EPSG:\*/)) {|if (key == authid \|\| (!(authid in boundingBox) \&\& key.match(/^EPSG:*/))) {|' "$init"; \
    grep -q 'var serverAndCGI = "/wms"' "$opts"; \
    grep -q 'var enableOSMMaps = false;' "$opts"; \
    grep -q 'var enableGoogleCommercialMaps = false;' "$opts"; \
    grep -q "PROJECTS_DIR = '/web/'" "$php"; \
    grep -q 'basename(\$filename)' "$php"; \
    if grep -q 'maps.googleapis.com' "$html"; then exit 1; fi; \
    grep -q 'libs/ext/ext-all.js' "$html"; \
    grep -q 'if (key == authid' "$init"

# ---------------------------------------------------------------------------
# Stage 2: Apache + PHP + QGIS Server (+ PyQGIS for the qgs.py tool)
# ---------------------------------------------------------------------------
FROM debian:${DEBIAN_RELEASE}-slim

ENV DEBIAN_FRONTEND=noninteractive \
    QT_QPA_PLATFORM=offscreen \
    QGIS_SERVER_LOG_LEVEL=1

# Packages are deliberately unpinned: CI rebuilds weekly to pick up security fixes.
# hadolint ignore=DL3008
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        apache2 libapache2-mod-fcgid libapache2-mod-php \
        qgis-server qgis-providers python3-qgis \
        fonts-dejavu-core \
 && rm -rf /var/lib/apt/lists/*

COPY --from=qwc /qgis-web-client/ /QGIS-Web-Client/
COPY etc/qgis-web-client.conf /etc/apache2/sites-available/qgis-web-client.conf
COPY bin/ /usr/local/bin/

RUN a2enmod headers rewrite fcgid \
 && a2dissite 000-default \
 && a2ensite qgis-web-client \
 && a2disconf other-vhosts-access-log \
 && echo 'ServerName localhost' > /etc/apache2/conf-available/servername.conf \
 && a2enconf servername \
 && ln -sfT /dev/stderr /var/log/apache2/error.log \
 && ln -sfT /dev/stdout /var/log/apache2/access.log \
 && mkdir -p /web \
 && apache2ctl configtest

WORKDIR /web
EXPOSE 80

HEALTHCHECK --interval=30s --timeout=5s --start-period=10s \
  CMD ["python3", "-c", "import urllib.request as u; u.urlopen('http://localhost/', timeout=4)"]

# Apache's graceful stop
STOPSIGNAL SIGWINCH
CMD ["apache2-foreground"]
