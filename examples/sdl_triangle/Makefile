CC ?= cc
PKG_CONFIG ?= pkg-config
ARCH_FLAGS ?= -m32 -fno-pie -no-pie
CFLAGS ?= -O2 -g -Wall -Wextra
SDL_GL_CFLAGS = $(shell $(PKG_CONFIG) --cflags sdl2 gl)
SDL_GL_LIBS = $(shell $(PKG_CONFIG) --libs sdl2 gl)
ELFLIFT ?= ../../build/lifter/elflift
EMXX ?= em++

.PHONY: all wasm smoke clean
all: triangle

triangle: triangle.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(ARCH_FLAGS) $(SDL_GL_CFLAGS) $< $(LDFLAGS) $(SDL_GL_LIBS) -o $@

smoke: triangle
	$(RUNNER) ./triangle --frames 3

wasm: triangle.js

triangle.js: triangle $(ELFLIFT) ../../scripts/elfconv.sh ../../bin/exe.sh $(wildcard ../../runtime/*.cpp ../../runtime/*.h ../../utils/*.cpp)
	TARGET=i386-wasm ECV_OUT_DIR="$(CURDIR)" ELFLIFT="$(abspath $(ELFLIFT))" EMCC="$(EMXX)" bash ../../bin/exe.sh ./triangle

clean:
	rm -f triangle triangle.bc triangle.js triangle.wasm
