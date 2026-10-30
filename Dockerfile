# syntax=docker/dockerfile:1

ARG DEBIAN_RELEASE=trixie

# ---------------------------------------------------------------------------
# Stage 1: fetch the prebuilt QWC2 stock app (pinned LTS release, checksummed)
# ---------------------------------------------------------------------------
FROM debian:${DEBIAN_RELEASE}-slim AS qwc2

ARG QWC2_VERSION=v2026.0.27-lts
ARG QWC2_COMMIT=903c97e4f828ea1d0a4d4c8f8c768797deef4bec
ARG QWC2_APP_SHA256=f477a50d33192192e033449ab46c65d29e5232125c8c7e71664694b6c08749ad
ARG QWC2_THEMESCONFIG_SHA256=c54311be2bb3dc92ad925119aa66adf5622e951c10999196cafe0fd14e2c4bf1
SHELL ["/bin/bash", "-o", "pipefail", "-c"]

# hadolint ignore=DL3008
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl unzip python3 \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /build
RUN curl -fsSL -o app.zip \
        "https://github.com/qgis/qwc2/releases/download/${QWC2_VERSION}/qwc2-stock-app.zip" \
 && echo "${QWC2_APP_SHA256}  app.zip" | sha256sum -c - \
 && curl -fsSL -o themesConfig.py \
        "https://raw.githubusercontent.com/qgis/qwc2/${QWC2_COMMIT}/scripts/themesConfig.py" \
 && echo "${QWC2_THEMESCONFIG_SHA256}  themesConfig.py" | sha256sum -c - \
 && unzip -q app.zip \
 && rm prod/themes.json prod/themesConfig.json \
 && find prod -name '*.map' -delete

COPY etc/qwc2/configure.py .
RUN python3 configure.py prod/config.json

# ---------------------------------------------------------------------------
# Stage 2: GDAL plugin for whole-slide images (gdal-openslide/), built
# against the same Debian GDAL and OpenSlide the runtime image installs
# ---------------------------------------------------------------------------
FROM debian:${DEBIAN_RELEASE}-slim AS gdal-openslide

# hadolint ignore=DL3008
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ make pkg-config libgdal-dev libopenslide-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY gdal-openslide/ .
RUN make && make install DESTDIR=/out

# ---------------------------------------------------------------------------
# Stage 3: Apache + QGIS Server (+ PyQGIS for qgs.py). No PHP.
# ---------------------------------------------------------------------------
FROM debian:${DEBIAN_RELEASE}-slim

ENV DEBIAN_FRONTEND=noninteractive \
    QT_QPA_PLATFORM=offscreen \
    QGIS_SERVER_LOG_LEVEL=1
SHELL ["/bin/bash", "-o", "pipefail", "-c"]

# Packages are deliberately unpinned: CI rebuilds weekly to pick up security fixes.
# hadolint ignore=DL3008
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        apache2 libapache2-mod-fcgid \
        qgis-server qgis-providers python3-qgis python3-gdal \
        libopenslide0 \
        fonts-dejavu-core tini \
 && rm -rf /var/lib/apt/lists/*

COPY --from=qwc2 /build/prod/ /QWC2/
COPY --from=qwc2 /build/themesConfig.py /opt/qwc2/themesConfig.py
COPY --from=gdal-openslide /out/ /
COPY demo/ /QWC2/demo/
COPY etc/qwc.conf /etc/apache2/sites-available/qwc.conf
COPY bin/ /usr/local/bin/

RUN a2enmod headers rewrite fcgid \
 && a2dissite 000-default \
 && a2ensite qwc \
 && a2disconf other-vhosts-access-log serve-cgi-bin \
 && echo 'ServerName localhost' > /etc/apache2/conf-available/servername.conf \
 && a2enconf servername \
 && ln -sfT /dev/stderr /var/log/apache2/error.log \
 && ln -sfT /dev/stdout /var/log/apache2/access.log \
 && ln -s /QWC2 /opt/qwc2/static \
 && echo '{"themes": {"items": []}}' > /QWC2/themes.json \
 && mkdir -p /web \
 && python3 -c "from osgeo import gdal; gdal.AllRegister(); assert gdal.GetDriverByName('OpenSlide'), 'OpenSlide GDAL plugin not loaded'" \
 && apache2ctl configtest \
 && mods="$(apache2ctl -M 2>/dev/null)" \
 && grep -q mpm_event_module <<<"$mods" \
 && if grep -qi php <<<"$mods"; then echo "PHP must not be installed" >&2; exit 1; fi

WORKDIR /web
EXPOSE 80

HEALTHCHECK --interval=30s --timeout=5s --start-period=20s \
  CMD ["python3", "-c", "import urllib.request as u; u.urlopen('http://localhost/', timeout=4)"]

# Apache's graceful stop; tini reaps the background theme generator
STOPSIGNAL SIGWINCH
ENTRYPOINT ["/usr/bin/tini", "--"]
CMD ["qwc-start"]
