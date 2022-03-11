#!/bin/sh

docker stop qwc
docker run --rm -ti --name=qwc\
    -v ${PWD}:${PWD} -w ${PWD}\
    -v ${PWD}/QGIS-Web-Client:/QGIS-Web-Client\
    -v ${PWD}/web:/web:\
    -p 8080:80 qwc
