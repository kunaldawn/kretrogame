# kretrogame build.
#
# Nothing is compiled on the host. Two toolchain images do the work:
#
#   kretro-guibuilder  Ubuntu 26.04, the runtime's own base, so SDL and glibc
#                      headers match what we ship. Builds all the C++, and
#                      runs clang-format and clang-tidy.
#   kretro-builder     musl and the static DwarFS tool. Builds the bootstrap
#                      and links the final binary.
#
# The pieces live in mk/:
#
#   config.mk   tools, images and flags
#   sources.mk  what goes into each program, and the list of tests
#   rules.mk    compiling and linking
#   tests.mk    make test, the boot tests, make test-runtime
#   runtime.mk  the images, the runtimes, link and player-base
#   quality.mk  make format, format-check, lint and check-generic
#
# `make help` lists the targets.

.DEFAULT_GOAL := all

include mk/config.mk
include mk/sources.mk

.PHONY: all app boot link player-base kretro test boot-test test-runtime
.PHONY: runtime player-runtime images shell format format-check lint lint-run check-generic clean help

all: images
	@$(RUN_GUI) make -s app
	@$(RUN_BOOT) make -s boot
	@if [ -s $(BUILD)/player-runtime.dwarfs ]; then $(RUN_BOOT) make -s player-base; \
	 else echo "  warning: no $(BUILD)/player-runtime.dwarfs - run 'make runtime'; no player base this time"; fi
	@$(RUN_BOOT) make -s link

kretro: all

# The pieces, each already inside the right image.
app: $(BUILD)/kretro-gui $(BUILD)/kgpack $(BUILD)/kretro-player
boot: $(BUILD)/bootstrap

shell: images
	@docker run --rm -it -u $(shell id -u):$(shell id -g) \
		-v $(CURDIR):/src -w /src -e HOME=/tmp $(GUIBUILDER) bash

clean:
	rm -rf $(BUILD)

help:
	@printf '%s\n' \
	  'make                 build everything and link build/kretro' \
	  '                     (with build/player-base inside it, once make runtime has run)' \
	  'make kretro          the same as make' \
	  'make app             kretro-gui, kgpack and kretro-player (inside kretro-guibuilder)' \
	  'make boot            the static bootstrap (inside kretro-builder)' \
	  'make player-base     link build/player-base (inside kretro-builder)' \
	  'make link            link build/kretro (inside kretro-builder)' \
	  'make test            unit tests, the Bundles page and the bootstrap; no iso/ or runtime needed' \
	  'make test-runtime    check both runtimes and start their Wine (needs make runtime)' \
	  'make runtime         rebuild the bundled runtimes (slow)' \
	  'make player-runtime  rebuild only the player runtime' \
	  'make images          build the toolchain images if they are missing' \
	  'make format          rewrite src/, boot/ and tests/ to .clang-format' \
	  'make format-check    fail if anything is not formatted' \
	  'make lint            clang-tidy over src/ with .clang-tidy' \
	  'make check-generic   fail if a tracked file names a game from the local collection' \
	  'make shell           a shell in the C++ toolchain' \
	  'make clean           remove build/' \
	  'make help            this list'

include mk/rules.mk
include mk/tests.mk
include mk/runtime.mk
include mk/quality.mk
