# GDAL plugin for whole-slide images via OpenSlide.
#   make && make install            (install into GDAL's plugin directory)
#   make check SLIDE=slide.svs      (gdalinfo through the freshly built plugin)

PLUGIN   := gdal_OpenSlide.so
PKGS     := gdal openslide
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -fPIC -fvisibility=hidden -Wall -Wextra $(shell pkg-config --cflags $(PKGS))
LDFLAGS  += -shared -Wl,--no-undefined -Wl,-z,relro,-z,now
LDLIBS   += $(shell pkg-config --libs $(PKGS))
PLUGINDIR ?= $(shell gdal-config --plugindir)

$(PLUGIN): openslidedataset.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

install: $(PLUGIN)
	install -D -m 644 $(PLUGIN) $(DESTDIR)$(PLUGINDIR)/$(PLUGIN)

check: $(PLUGIN)
	GDAL_DRIVER_PATH=$(CURDIR) gdalinfo --format OpenSlide
	GDAL_DRIVER_PATH=$(CURDIR) gdalinfo "OPENSLIDE:$(SLIDE)"

clean:
	rm -f $(PLUGIN)

.PHONY: install check clean
