# What goes into each program. Objects land under $(BUILD) at the path of
# their source: src/pack/tree.cpp becomes $(BUILD)/src/pack/tree.o.

DISC_SRC = src/disc/cue.cpp src/disc/sector.cpp src/disc/audio.cpp src/disc/container.cpp \
          src/disc/disc.cpp src/disc/drive.cpp src/disc/serial.cpp
LIB_SRC = $(DISC_SRC) src/config/config.cpp src/config/scaling.cpp src/util/hash.cpp src/util/cbor.cpp \
          src/util/paths.cpp src/util/proc.cpp \
          src/util/toml.cpp src/pack/tree.cpp src/pack/kgpack.cpp src/gpu/probe.cpp \
          src/rt/env.cpp src/install/iso.cpp src/install/install.cpp src/install/build.cpp \
          src/install/discs.cpp src/install/keys.cpp src/install/registry.cpp src/install/share.cpp \
          src/session/session.cpp
# GPU, backend and diagnostics policy: pure logic the player and kretro share.
GPU_POLICY_SRC = src/gpu/host.cpp src/gpu/nvidia.cpp src/gpu/caps.cpp src/util/pe.cpp \
          src/player/policy.cpp src/player/doctor.cpp
LIB_SRC += $(GPU_POLICY_SRC)
# The table of contents, trailer v4, bundle.meta, and building a player. Shared
# by kretro, which builds players, and the player, which reads itself.
BUNDLE_SRC = src/bundle/toc.cpp src/bundle/meta.cpp src/bundle/build.cpp
LIB_SRC += $(BUNDLE_SRC)
# The player's own logic - its state, a game's prefix, the desktop entry, its
# command line and playing a pack out of its own file - with no window in it,
# so the tests reach all of it.
PLAYER_LIB_SRC = src/player/state.cpp src/player/prefix.cpp src/player/desktop.cpp \
          src/player/cli.cpp src/player/player.cpp
LIB_SRC += $(PLAYER_LIB_SRC)
# The Bundles page's decisions, apart from the page so the tests can reach them.
BUILDER_SRC = src/bundle/builder.cpp src/bundle/preview.cpp
LIB_SRC += $(BUILDER_SRC)

IMGUI_SRC = $(IMGUI)/imgui.cpp $(IMGUI)/imgui_draw.cpp $(IMGUI)/imgui_tables.cpp \
          $(IMGUI)/imgui_widgets.cpp $(IMGUI)/imgui_impl_sdl2.cpp \
          $(IMGUI)/imgui_impl_sdlrenderer2.cpp
# kretro's window: the shelf, the wizard, staging and the Bundles page. The
# order is the link order, kept as it was.
GUI_SRC = src/gui/app.cpp src/gui/library.cpp src/gui/input.cpp src/gui/stage.cpp src/gui/wizard.cpp \
          src/gui/widgets.cpp $(IMGUI_SRC) src/gui/bundles.cpp
# The player's window: the launcher, the widgets it shares with the shelf, and
# the gamepad helper a session starts beside a game. None of kretro's pages.
PLAYER_GUI_SRC = src/gui/launcher.cpp src/gui/widgets.cpp src/gui/library.cpp src/gui/input.cpp \
          $(IMGUI_SRC)
B3_SRC  = $(B3)/blake3.c $(B3)/blake3_dispatch.c $(B3)/blake3_portable.c

LIB_OBJ        = $(LIB_SRC:%.cpp=$(BUILD)/%.o)
GUI_OBJ        = $(GUI_SRC:%.cpp=$(BUILD)/%.o)
PLAYER_GUI_OBJ = $(PLAYER_GUI_SRC:%.cpp=$(BUILD)/%.o)
B3_OBJ         = $(B3_SRC:%.c=$(BUILD)/%.o)
# The bootstrap checks a v4 file's table of contents with the same BLAKE3 the
# rest of kretro uses, compiled a second time with musl: the objects of B3_OBJ are
# glibc's and cannot go into a static musl binary. They get their own directory
# so the two builds never mistake each other's objects for their own.
BOOT_B3_OBJ = $(B3_SRC:%.c=$(BUILD)/musl/%.o)

MAIN_OBJ = $(BUILD)/src/main_kretro.o $(BUILD)/src/main_player.o $(BUILD)/src/pack/main_kgpack.o

# The unit tests, in the order `make test` runs them. Each is one file,
# tests/unit/test_<name>.cpp, linked with the library.
UNIT_TESTS     = pack disc install config wizard stage bundle policy player builder
UNIT_TEST_BIN  = $(UNIT_TESTS:%=$(BUILD)/test_%)
# The Bundles page drawn into no window. It links the GUI, and so SDL, which
# the host need not have: `make test` runs it in the image that built it.
GUI_TEST_BIN   = $(BUILD)/test_bundles_page
TEST_OBJ       = $(UNIT_TESTS:%=$(BUILD)/tests/unit/test_%.o) $(BUILD)/tests/unit/test_bundles_page.o

# What `make format` and `make lint` look at: our C and C++, never
# third_party/, which keeps its upstream's style.
FORMAT_SRC = $(sort $(shell find src boot tests -name '*.c' -o -name '*.cpp' -o -name '*.h'))
# clang rejects the four files that instantiate cbor::Value, whose
# std::vector<std::pair<Value, Value>> names Value before it is complete: GCC
# and libstdc++ accept it, clang with libstdc++ 15 does not. They are linted
# again once cbor::Value holds its map another way.
LINT_SKIP  = src/util/cbor.cpp src/pack/kgpack.cpp src/bundle/meta.cpp src/bundle/builder.cpp
LINT_SRC   = $(filter-out $(LINT_SKIP),$(sort $(shell find src -name '*.cpp')))
