# GDAL plugin for Philips iSyntax whole-slide images via libisyntax.
#
#   make LIBISYNTAX=/path/to/libisyntax     (source tree, built with build-libisyntax)
#   make install
#
# libisyntax is linked statically. Its bundled allocator must be built with
# LTALLOC_DISABLE_OPERATOR_NEW_OVERRIDE, or it would replace C++ new/delete
# inside the host process (QGIS, GDAL); build-libisyntax does that, and
# builds for a portable CPU baseline.

PLUGIN     := gdal_iSyntax.so
LIBISYNTAX ?= ../libisyntax
LIBA       := $(LIBISYNTAX)/build/libisyntax.a
CXXFLAGS   ?= -O2 -g
CXXFLAGS   += -std=c++17 -fPIC -fvisibility=hidden -Wall -Wextra \
              -I$(LIBISYNTAX)/src $(shell pkg-config --cflags gdal)
# --exclude-libs: keep libisyntax's own symbols private to the plugin
LDFLAGS    += -shared -Wl,--no-undefined -Wl,--exclude-libs,ALL -Wl,-z,relro,-z,now
LDLIBS     += $(LIBA) $(shell pkg-config --libs gdal) -lpthread -lm
PLUGINDIR  ?= $(shell gdal-config --plugindir)

$(PLUGIN): isyntaxdataset.cpp $(LIBA)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

# libisyntax's CMakeLists adds -march=native, which ties the binary to the
# build machine's CPU (a CI runner, or QEMU's CPU model for arm64) and can
# crash elsewhere with "illegal instruction". Build for the architecture
# baseline instead, as Debian does for all its packages.
ARCH := $(shell uname -m)
ifeq ($(ARCH),x86_64)
  MARCH ?= -march=x86-64
else ifeq ($(ARCH),aarch64)
  MARCH ?= -march=armv8-a
endif

build-libisyntax:
	sed -i 's/ -march=native//' $(LIBISYNTAX)/CMakeLists.txt
	cmake -S $(LIBISYNTAX) -B $(LIBISYNTAX)/build -DCMAKE_BUILD_TYPE=Release \
	      -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_TESTING=OFF \
	      -DCMAKE_C_FLAGS="$(MARCH)" \
	      -DCMAKE_CXX_FLAGS="$(MARCH) -DLTALLOC_DISABLE_OPERATOR_NEW_OVERRIDE"
	cmake --build $(LIBISYNTAX)/build --target isyntax -j

install: $(PLUGIN)
	install -D -m 644 $(PLUGIN) $(DESTDIR)$(PLUGINDIR)/$(PLUGIN)

check: $(PLUGIN)
	GDAL_DRIVER_PATH=$(CURDIR) gdalinfo --format iSyntax
	GDAL_DRIVER_PATH=$(CURDIR) gdalinfo "$(SLIDE)"

clean:
	rm -f $(PLUGIN)

.PHONY: build-libisyntax install check clean
