# What goes into each program. Objects land under $(BUILD) at the path of
# their source: src/pack/tree.cpp becomes $(BUILD)/src/pack/tree.o.

DISC_SRC = src/disc/cue.cpp src/disc/sector.cpp src/disc/audio.cpp src/disc/container.cpp \
          src/disc/disc.cpp src/disc/drive.cpp src/disc/serial.cpp src/disc/iso.cpp \
          src/disc/members.cpp src/disc/database.cpp
LIB_SRC = $(DISC_SRC) src/config/config.cpp src/config/scaling.cpp src/util/hash.cpp src/util/cbor.cpp \
          src/util/paths.cpp src/util/proc.cpp \
          src/util/toml.cpp src/util/format.cpp src/util/file_io.cpp src/util/safe_names.cpp \
          src/util/text.cpp src/util/fs_ci.cpp src/pack/tree.cpp src/pack/header.cpp src/pack/pack_meta.cpp \
          src/pack/pack.cpp src/pack/write.cpp src/pack/dwarfs.cpp src/gpu/probe.cpp \
          src/rt/env.cpp src/install/install.cpp src/install/manifest.cpp \
          src/install/collection.cpp src/install/body.cpp \
          src/install/build.cpp \
          src/install/build_sources.cpp src/install/build_installer.cpp src/install/build_extract.cpp \
          src/install/build_capture.cpp src/install/build_write.cpp \
          src/install/draft.cpp src/install/setup_ref.cpp src/install/source.cpp \
          src/install/survey.cpp src/install/preset.cpp src/install/game_id.cpp \
          src/install/staging.cpp \
          src/install/discs.cpp src/install/keys.cpp src/install/share.cpp
# Wine's side of a prefix that is not the session's: the registry an
# installer wrote, and the files it left on C: outside the game.
WINE_SRC = src/wine/registry.cpp src/wine/system_files.cpp
LIB_SRC += $(WINE_SRC)
# Playing a game: its layers, its lock, its prefix, the compositor it runs in,
# and what it leaves behind - one file per concern, all of it in the library.
SESSION_SRC = src/session/internal.cpp src/session/saves_layout.cpp src/session/lock.cpp \
          src/session/layers.cpp src/session/interrupt.cpp src/session/unpack.cpp \
          src/session/saves.cpp src/session/journal.cpp src/session/prefix.cpp \
          src/session/compositor.cpp src/session/backend.cpp src/session/play.cpp \
          src/session/input_helper.cpp src/session/saves_transfer.cpp
LIB_SRC += $(SESSION_SRC)
# GPU, backend and diagnostics policy: pure logic the player and kretro share.
GPU_POLICY_SRC = src/gpu/host.cpp src/gpu/nvidia.cpp src/gpu/files.cpp src/gpu/caps.cpp src/util/pe.cpp \
          src/backend/policy.cpp src/player/doctor.cpp
LIB_SRC += $(GPU_POLICY_SRC)
# The table of contents, trailer v4, bundle.meta, and building a player. Shared
# by kretro, which builds players, and the player, which reads itself.
BUNDLE_SRC = src/bundle/toc.cpp src/bundle/meta.cpp src/bundle/gamepad.cpp src/bundle/build.cpp
LIB_SRC += $(BUNDLE_SRC)
# The player's own logic - its file, where its state goes, its settings and
# verified memo, unpacking, a game's prefix, the desktop entry, its command
# line and playing a pack out of its own file - with no window in it, so the
# tests reach all of it.
PLAYER_LIB_SRC = src/player/bundle_file.cpp src/player/state_dir.cpp src/player/settings.cpp \
          src/player/verify_memo.cpp src/player/unpack.cpp src/player/prefix.cpp \
          src/player/desktop.cpp src/player/cli.cpp src/player/player.cpp
LIB_SRC += $(PLAYER_LIB_SRC)
# The Bundles page's decisions, apart from the page so the tests can reach them.
BUILDER_SRC = src/bundle/builder/draft.cpp src/bundle/builder/draft_codec.cpp \
          src/bundle/builder/draft_store.cpp src/bundle/builder/pack_facts.cpp \
          src/bundle/builder/repack.cpp src/bundle/builder/exe_probe.cpp \
          src/bundle/builder/key_fragment.cpp src/bundle/builder/checks.cpp \
          src/bundle/builder/size_report.cpp src/bundle/builder/player_base.cpp \
          src/bundle/builder/draft_build.cpp src/bundle/builder/preview.cpp
LIB_SRC += $(BUILDER_SRC)

IMGUI_SRC = $(IMGUI)/imgui.cpp $(IMGUI)/imgui_draw.cpp $(IMGUI)/imgui_tables.cpp \
          $(IMGUI)/imgui_widgets.cpp $(IMGUI)/imgui_impl_sdl2.cpp \
          $(IMGUI)/imgui_impl_sdlrenderer2.cpp
# The wizard: its wiring, a file per step or pair of steps, and its wording.
WIZARD_SRC = src/gui/wizard/wizard.cpp src/gui/wizard/sources_step.cpp \
          src/gui/wizard/identity_step.cpp src/gui/wizard/what_step.cpp \
          src/gui/wizard/installing_step.cpp src/gui/wizard/result_steps.cpp \
          src/gui/wizard/build_steps.cpp src/gui/wizard/words.cpp
# The Bundles page: its wiring, a file per step or pair of steps, and the
# file list it picks pictures, extra files and a folder from.
BUNDLES_PAGE_SRC = src/gui/bundles/bundles_page.cpp src/gui/bundles/identity_step.cpp \
          src/gui/bundles/games_step.cpp src/gui/bundles/per_game_step.cpp \
          src/gui/bundles/review_steps.cpp src/gui/bundles/build_step.cpp \
          src/gui/bundles/preview_step.cpp src/gui/bundles/files.cpp
# The shelf: its host and the context its pages share, a file per page, and
# the scan of the collection it shows.
SHELF_SRC = src/gui/shelf/host.cpp src/gui/shelf/run.cpp src/gui/shelf/context.cpp \
          src/gui/shelf/shelf_page.cpp src/gui/shelf/game_page.cpp src/gui/shelf/import_page.cpp \
          src/gui/shelf/doctor_page.cpp src/gui/shelf/library_page.cpp \
          src/gui/shelf/settings_page.cpp src/gui/shelf/timeline_page.cpp \
          src/gui/shelf/wizard_page.cpp src/gui/shelf/bundles_page.cpp src/gui/shelf/scan.cpp
# kretro's window: the shelf, the wizard, staging and the Bundles page. The
# order is the link order, kept as it was.
GUI_SRC = $(SHELF_SRC) src/gui/gamepad_bridge.cpp src/gui/stage/stage.cpp \
          src/gui/stage/x_errors.cpp src/gui/stage/stage_probe.cpp $(WIZARD_SRC) \
          src/gui/widgets.cpp src/gui/window.cpp src/gui/texture.cpp src/gui/job.cpp \
          src/gui/job_modal.cpp src/gui/file_list.cpp $(IMGUI_SRC) $(BUNDLES_PAGE_SRC) \
          src/gui/screen.cpp src/gui/event_loop.cpp
# The player's launcher: its host and the context its pages share, a file per
# page or group of pages, and its modals.
LAUNCHER_SRC = src/gui/launcher/host.cpp src/gui/launcher/context.cpp \
          src/gui/launcher/first_run.cpp src/gui/launcher/grid_page.cpp \
          src/gui/launcher/game_page.cpp src/gui/launcher/saves_page.cpp \
          src/gui/launcher/settings_pages.cpp src/gui/launcher/bundle_pages.cpp \
          src/gui/launcher/working_modal.cpp
# The player's window: the launcher, the window, pictures, widgets, the
# worker and the event loop it shares with the shelf, and the gamepad helper a
# session starts beside a game.
# None of kretro's pages - nothing from shelf/, wizard/, bundles/ or stage/ -
# and not the shelf's scan of the collection.
# texture.cpp holds stb_image's one definition in each of the two programs.
PLAYER_GUI_SRC = $(LAUNCHER_SRC) src/gui/widgets.cpp src/gui/window.cpp src/gui/texture.cpp \
          src/gui/job.cpp src/gui/gamepad_bridge.cpp $(IMGUI_SRC) src/gui/screen.cpp \
          src/gui/event_loop.cpp
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
# The bootstrap: static musl C, a unit per concern beside main.c, and boot.h
# the only header between them. Every .c in boot/ goes in.
BOOT_SRC = $(sort $(wildcard boot/*.c))
BOOT_OBJ = $(BOOT_SRC:%.c=$(BUILD)/musl/%.o)

# kretro's command line: one table of commands, and a file for each group of
# them. It opens the shelf and the wizard, so it links the GUI and stays out of
# the library the player links.
KRETRO_CLI_SRC = src/cli/commands.cpp src/cli/system.cpp src/cli/catalogue.cpp \
          src/cli/install.cpp src/cli/library.cpp src/cli/play.cpp src/cli/saves.cpp \
          src/cli/share.cpp src/cli/bundle.cpp
KRETRO_APP_SRC = src/apps/kretro/main.cpp $(KRETRO_CLI_SRC)
KRETRO_APP_OBJ = $(KRETRO_APP_SRC:%.cpp=$(BUILD)/%.o)

# The player's command line: main settles its state and builds the Player,
# and a file of commands does the rest.
PLAYER_APP_SRC = src/apps/player/main.cpp src/apps/player/commands.cpp
PLAYER_APP_OBJ = $(PLAYER_APP_SRC:%.cpp=$(BUILD)/%.o)
KGPACK_APP_SRC = src/apps/kgpack/main.cpp
KGPACK_APP_OBJ = $(KGPACK_APP_SRC:%.cpp=$(BUILD)/%.o)

MAIN_OBJ = $(KRETRO_APP_OBJ) $(PLAYER_APP_OBJ) $(KGPACK_APP_OBJ)

# The unit tests, in the order `make test` runs them. Each is one file,
# tests/unit/test_<name>.cpp, linked with the library.
UNIT_TESTS     = pack disc install config wizard stage bundle policy player builder
UNIT_TEST_BIN  = $(UNIT_TESTS:%=$(BUILD)/test_%)
# The Bundles page drawn into no window. It links the GUI, and so SDL, which
# the host need not have: `make test` runs it in the image that built it.
GUI_TEST_BIN   = $(BUILD)/test_bundles_page
TEST_OBJ       = $(UNIT_TESTS:%=$(BUILD)/tests/unit/test_%.o) $(BUILD)/tests/unit/test_bundles_page.o

# Every object the rules compile, so mk/rules.mk can include each one's header
# dependencies. A stage that adds an object list appends it here.
ALL_OBJ = $(LIB_OBJ) $(GUI_OBJ) $(PLAYER_GUI_OBJ) $(MAIN_OBJ) $(TEST_OBJ) $(B3_OBJ) $(BOOT_B3_OBJ) \
          $(BOOT_OBJ)

# What `make format-all`, `make format-check-all` and `make lint` look at:
# our C and C++, never third_party/, which keeps its upstream's style.
FORMAT_SRC = $(sort $(shell find src boot tests -name '*.c' -o -name '*.cpp' -o -name '*.h'))
LINT_SRC   = $(sort $(shell find src -name '*.cpp'))

# The files kept wholly in .clang-format's style, which `make format` and
# `make format-check` (and so CI) look at: code written new, not code moved.
# Moved and older files keep deliberate multi-statement lines, so they are
# formatted only on the lines an edit changes. A new file goes in this list,
# in order.
FORMAT_CLEAN = \
               boot/boot.h \
               src/apps/kgpack/main.cpp \
               src/apps/kretro/main.cpp \
               src/apps/player/commands.cpp \
               src/apps/player/commands.h \
               src/apps/player/main.cpp \
               src/cli/cli.h \
               src/cli/command.h \
               src/cli/commands.cpp \
               src/cli/handlers.h \
               src/gui/event_loop.cpp \
               src/gui/event_loop.h \
               src/gui/file_list.cpp \
               src/gui/file_list.h \
               src/gui/format.h \
               src/gui/job.cpp \
               src/gui/job.h \
               src/gui/job_modal.cpp \
               src/gui/job_modal.h \
               src/gui/page.h \
               src/gui/palette.h \
               src/gui/screen.cpp \
               src/gui/screen.h \
               src/pack/dwarfs.cpp \
               src/pack/dwarfs.h \
               src/session/input_helper.cpp \
               src/session/input_helper.h \
               src/util/bytes.h \
               src/util/env.h \
               src/util/file_io.cpp \
               src/util/file_io.h \
               src/util/format.cpp \
               src/util/format.h \
               src/util/fs_ci.cpp \
               src/util/fs_ci.h \
               src/util/safe_names.cpp \
               src/util/safe_names.h \
               src/util/text.cpp \
               src/util/text.h \
               tests/unit/support/bundle_fixtures.h \
               tests/unit/support/check.h \
               tests/unit/support/files.h
