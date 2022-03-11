Pulling Docker Image
====================

> docker pull ghcr.io/ruitaodong/qwc

Running Docker Container
========================

> docker run --name=qwc --rm -d -p 8080:80 -v data-dir:/web ghcr.io/ruitaodong/qwc

Creating Project(s)
===================

> docker exec qwc qgs.sh name1.qgs image1.png vector1.json vector2.json

> docker exec qwc qgs.sh name2.qgs image2.png vector3.json vector4.json

Will create two projects name1.qgs & name2.qgs

Pointing browser to ip-addr:8080 will show name1 & name2

Clicking name1 will show image1.png vector1.json vector2.json and

clicking name2 will show image2.png vector3.json vector3.json.



