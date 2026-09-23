# Tools, images and flags. Everything here can be overridden from the command
# line: `make BUILD=out`, `make CXXFLAGS=...`.

BUILDER    ?= kretro-builder:latest
GUIBUILDER ?= kretro-guibuilder:latest
RUNTIME    ?= kretro-runtime:latest
BUILD      ?= build

CXX        ?= g++
CC         ?= gcc
# -MMD -MP writes a .d file per object listing the headers it included, and the
# include in mk/rules.mk feeds those back to make. Without them, editing a header
# rebuilds nothing: objects compiled against the old layout of a struct get
# linked with objects using the new one, and the program corrupts its own heap.
CXXFLAGS   ?= -std=c++20 -O2 -g -Wall -Wextra -MMD -MP
CFLAGS     ?= -O3 -Wall
MUSL_CC    ?= musl-gcc
MUSL_FLAGS ?= -static -O2 -Wall -Wextra -std=c11

B3         = third_party/blake3
IMGUI      = third_party/imgui
# The portable BLAKE3 backend keeps the build to one set of flags; hashing a
# 3.6 GB disc still takes only seconds, well inside what an install costs.
B3_FLAGS   = -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512
# -Isrc lets the tests name a header by its place in the tree: "pack/kgpack.h".
INCLUDES   = -Isrc -I$(B3) -I$(IMGUI) -Ithird_party $(shell pkg-config --cflags sdl2 2>/dev/null)
LIBS       = -lzstd $(shell pkg-config --libs sdl2 2>/dev/null) -lX11 -lXtst -lpthread

# Nothing is compiled on the host: each of these runs a command in an image
# with this tree mounted at /src.
DOCKER     = docker run --rm -i -u $(shell id -u):$(shell id -g) \
	-v $(CURDIR):/src -w /src -e HOME=/tmp
RUN_GUI    = $(DOCKER) $(GUIBUILDER)
RUN_BOOT   = $(DOCKER) $(BUILDER)
