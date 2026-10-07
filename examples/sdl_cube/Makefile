CC ?= cc
PKG_CONFIG ?= pkg-config
ARCH_FLAGS ?= -m32 -msse2 -mfpmath=sse -fno-pie -no-pie
CFLAGS ?= -O2 -g -Wall -Wextra
SDL_GL_CFLAGS = $(shell $(PKG_CONFIG) --cflags sdl2 gl)
SDL_GL_LIBS = $(shell $(PKG_CONFIG) --libs sdl2 gl)
ELFLIFT ?= ../../build/lifter/elflift
EMXX ?= em++

.PHONY: all wasm smoke clean
all: cube

cube: cube.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(ARCH_FLAGS) $(SDL_GL_CFLAGS) $< $(LDFLAGS) $(SDL_GL_LIBS) -o $@

smoke: cube
	$(RUNNER) ./cube --frames 90

wasm: cube.js

cube.js: cube $(ELFLIFT) ../../scripts/elfconv.sh ../../bin/exe.sh $(wildcard ../../runtime/*.cpp ../../runtime/*.h ../../utils/*.cpp)
	TARGET=i386-wasm ECV_LEGACY_GL=0 ECV_OUT_DIR="$(CURDIR)" ELFLIFT="$(abspath $(ELFLIFT))" EMCC="$(EMXX)" bash ../../bin/exe.sh ./cube

clean:
	rm -f cube cube.bc cube.js cube.wasm
