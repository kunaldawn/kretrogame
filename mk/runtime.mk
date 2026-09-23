# The images, the runtimes, and linking them with the programs into one file.

images:
	@docker image inspect $(BUILDER) >/dev/null 2>&1 || \
		docker build -q -t $(BUILDER) -f runtime/Dockerfile.builder runtime/ >/dev/null
	@docker image inspect $(GUIBUILDER) >/dev/null 2>&1 || \
		docker build -q -t $(GUIBUILDER) -f runtime/Dockerfile.guibuilder runtime/ >/dev/null

# The runtime image is the slow one; it is built explicitly, not on demand.
# One image, two runtimes: build/runtime.dwarfs for kretro and
# build/player-runtime.dwarfs for the player. PLAYER_DLL_WHITELIST=1 enforces
# runtime/player-keep-dlls.txt on the player's Wine (see that file).
PLAYER_DLL_WHITELIST ?= 0
runtime:
	@docker build -t $(RUNTIME) -f runtime/Dockerfile.runtime runtime/
	@BUILD=$(BUILD) RUNTIME=$(RUNTIME) PLAYER_DLL_WHITELIST=$(PLAYER_DLL_WHITELIST) ./runtime/build-runtime.sh

# The player's runtime alone, for when only its prune lists changed.
player-runtime:
	@docker build -t $(RUNTIME) -f runtime/Dockerfile.runtime runtime/
	@BUILD=$(BUILD) RUNTIME=$(RUNTIME) PLAYER_DLL_WHITELIST=$(PLAYER_DLL_WHITELIST) ./runtime/build-runtime.sh player

# link runs in the musl builder, which has no SDL or ImGui headers. It consumes
# artifacts built elsewhere rather than naming them as prerequisites, because a
# prerequisite here would have make try to rebuild the GUI in the wrong image.
#
# kretro carries the player base, so that building a player needs no network
# and no source tree. Without one kretro still links and still plays; it just
# cannot build players, and says so here rather than on the Bundles page.
link: $(BUILD)/bootstrap $(BUILD)/kretro-b3
	@for f in $(BUILD)/kretro-gui $(BUILD)/runtime.dwarfs; do \
	  [ -s "$$f" ] || { echo "missing $$f - run 'make app' and 'make runtime' first"; exit 1; }; \
	done
	@if [ -s $(BUILD)/player-base ]; then pb="--player-base $(BUILD)/player-base"; \
	 else echo "  warning: no $(BUILD)/player-base - kretro is linked without one and cannot build players"; pb=; fi; \
	python3 scripts/kretro-link.py \
		--bootstrap $(BUILD)/bootstrap \
		--tools /usr/local/bin/dwarfs-universal \
		--image $(BUILD)/runtime.dwarfs \
		--app $(BUILD)/kretro-gui \
		--b3 $(BUILD)/kretro-b3 \
		$$pb \
		--out $(BUILD)/kretro

# The player base: the player's bootstrap, runtime and app, linked the same way
# kretro is, with no games. Like link, it runs in the musl builder and only
# checks that its inputs are there; they are built by their own targets.
# One bootstrap serves kretro and every player: it tells them apart by whether
# the table carries a bundle.meta.
PLAYER_BOOTSTRAP ?= $(BUILD)/bootstrap
player-base: $(BUILD)/kretro-b3
	@for f in $(PLAYER_BOOTSTRAP) $(BUILD)/player-runtime.dwarfs $(BUILD)/kretro-player; do \
	  [ -s "$$f" ] || { echo "missing $$f - build the player's bootstrap, runtime and app first"; exit 1; }; \
	done
	@python3 scripts/kretro-link.py \
		--bootstrap $(PLAYER_BOOTSTRAP) \
		--tools /usr/local/bin/dwarfs-universal \
		--image $(BUILD)/player-runtime.dwarfs \
		--app $(BUILD)/kretro-player \
		--b3 $(BUILD)/kretro-b3 \
		--out $(BUILD)/player-base
