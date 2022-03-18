FROM	ghcr.io/ruitaodong/qwc-base
ENV  	DEBIAN_FRONTEND noninteractive

RUN	apt-get -y update && apt-get install --no-install-recommends -y\
	python-qgis qgis make xvfb xfonts-base openssl1.0\
	&& apt-get clean && /bin/rm -r -f /var/lib/apt/lists/*

COPY	bin/	/usr/local/bin/

WORKDIR	/web
