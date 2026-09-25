# Architecture

How kretrogame is put together, as the source stands. For the on-disk
formats see [file-format.md](file-format.md). For the build see
[building.md](building.md), and for the tests see [testing.md](testing.md).

- [Programs](#programs)
- [Process model](#process-model)
- [The KRETRO_* environment](#the-kretro_-environment)
- [Module map](#module-map)
- [Global state](#global-state)
- [Threads](#threads)
- [Playing a game](#playing-a-game)
- [The GUI: pages, hosts and jobs](#the-gui-pages-hosts-and-jobs)
- [Two runtimes from one image](#two-runtimes-from-one-image)

## Programs

Three programs are compiled from `src/apps/`:

| Program | Source | Built as | What it is |
|---|---|---|---|
| kretro | `src/apps/kretro/main.cpp` and `src/cli/` | `build/kretro-gui`, linked into `build/kretro` | The builder. With no arguments it opens the shelf (`gui::run`). Otherwise `cli::run` dispatches a subcommand. |
| the player | `src/apps/player/` | `build/kretro-player`, linked into `build/player-base` | What kretro builds and people receive. It opens its own file, settles its state directory, and runs the launcher or one command. |
| kgpack | `src/apps/kgpack/main.cpp` | `build/kgpack` | A small development tool that creates, inspects and verifies `.kgpack` files. It is not shipped. |

The build tools are:

- `boot/`: the static musl bootstrap at the front of every kretro and player
  file.
- `scripts/kretro-link.py`: appends the tools, runtime, app and optionally the
  player base to the bootstrap, and writes the table and trailer.
- `scripts/kretro-b3.c`: the linker's BLAKE3.
- `runtime/`: the toolchain and runtime images, and the scripts that cut the
  runtimes from them.

## Process model

Every kretro file and every player starts the same way. The kernel runs the
bootstrap (`boot/main.c`), which does this, one named step at a time:

1. It finds its own file (`boot_self_path`) and reads the trailer and table
   (`boot_read_layout`). A damaged file stops here, before anything is mounted.
2. It decides who chooses the state directory (`choose_state`). For kretro, a
   `kretro-data` directory beside the file becomes `KRETRO_STATE`. For a
   player, an inherited `KRETRO_STATE` is removed and the player app decides.
3. It places the DwarFS tool (`boot_place_tool`).
4. It makes the runtime reachable (`find_runtime`): a FUSE mount of the
   runtime image straight out of the file at its offset, or an unpacked copy.
5. It unpacks the app next to the runtime (`place_app`) and applies the
   `KRETRO_GUI` development override (kretro only).
6. It hands down what it did through the environment (`hand_down`).
7. It execs the runtime's own loader, `<runtime>/lib/ld-linux-x86-64.so.2
   --library-path <runtime libs> <app> args...` (`exec_loader`).

From step 7 on, nothing links the host's C library, which is why one file runs
on glibc and musl systems alike. The app then starts everything else from the
runtime: Weston, Xwayland, Wine, the DwarFS mounts of games, and the gamepad
helper, which is the app itself run again through `KRETRO_APP`.

### The fallback ladder

Each step prefers the cheaper way and falls back when the machine refuses it:

- **The DwarFS tool** runs from an executable memfd, sealed and inherited by
  the app as `/proc/self/fd/N`. When the kernel will not create one
  (`vm.memfd_noexec`, or a kernel without `MFD_EXEC`), or later refuses to
  execute it, the tool is written to `<cache>/kretro/tools-<key>/` and then
  to the runtime directory. The first of those that is not mounted noexec
  wins. `KRETRO_BOOT_NO_MEMFD` forces the file path for tests.
- **The runtime** is mounted with FUSE at
  `<rtdir>/kretro/rt-<key>-<off>/`. The mount point is never beside the
  executable, because fusermount3 may only mount under `$HOME`, `/tmp` and
  `/run/user`. When the mount fails, the runtime is unpacked with
  dwarfsextract into the cache, or into the runtime directory when the cache
  is noexec. The unpack happens in a directory of the run's own, is marked
  finished, and is then renamed into place, so an interrupted unpack is never
  taken for a whole one.
- **Games** follow the same rule inside the app (`session::open_layers`). A
  game's pack body is mounted in place and a fuse-overlayfs writable layer is
  put over it. When either cannot be done, the body is unpacked instead.
  A player does not try to mount at all when `KRETRO_MOUNT_MODE=extract`.
  kretro unpacks at once. A player's session throws `session::NeedsUnpack`,
  and the player asks before it unpacks.

**User namespaces are not used.** `boot_userns_probe` only reports, under
`KRETRO_DEBUG`, whether an unprivileged user namespace could have mounted the
runtime. Using one would need every process, including the app's own DwarFS
daemons, to live in that namespace. The app's `fusermount3 -u` would not work
there, and nothing would be left to reap the daemons when the app exits.
Ubuntu 24.04 and later also create such namespaces without the right to
mount. Supporting it would need a resident bootstrap that mounts and unmounts
on the app's behalf.

## The KRETRO_* environment

| Name | Set by | Read by | Meaning |
|---|---|---|---|
| `KRETRO_SELF` | bootstrap | player `main`, which opens it with `player::Bundle::open`; kretro's builder (`bundle::shelf_build_inputs`, and the Bundles page's `find_player_base`) | The file that was run. `/proc/self/exe` is the loader by then. |
| `KRETRO_TOC` | bootstrap: v4 files only; removed otherwise | player `main`, through `player::Bundle::open` | `<toc_off>:<toc_len>` of the table the bootstrap checked. The player refuses to start if it disagrees with its own reading. |
| `KRETRO_RUNTIME` | bootstrap | `kg::runtime_dir`, `rt::make`, `bundle::preview_env` | The runtime root, mounted or unpacked. |
| `KRETRO_MOUNT_MODE` | bootstrap | `gpu` capabilities (the doctor), `player::Player::no_fuse` | `fusermount` or `extract`: how the runtime got there, and so whether a game's pack can be mounted. |
| `KRETRO_DWARFS` | bootstrap; removed when there is no tool | `kg::dwarfs_tool` (unset or empty means no tool), and through it install and the session's pack unpacking and layers; save transfer reads it directly (only unset means no tool); repack, the Bundles page's executable probe and licences, and the builder read it with `env_or_empty` and pass it on as given | The DwarFS tool, usually `/proc/self/fd/N`. Callers must pass `--tool=`. |
| `KRETRO_APP` | bootstrap | `session` compositor | The unpacked app, which a session runs again as the gamepad helper. |
| `KRETRO_STATE` | bootstrap (kretro's `kretro-data` portable mode; removed for a player), `player::settle_state`, the compositor for the gamepad helper, tests | `kg::state_dir`, `player::inherited_state` | The state directory. |
| `KRETRO_GUI` | developer | bootstrap, kretro only; always removed from the environment afterwards | Run this app instead of the embedded one. |
| `KRETRO_PLAYER_BASE` | developer, tests | `bundle::find_player_base` | Build players from this player base instead of the embedded one. |
| `KRETRO_DEBUG` | user | bootstrap (`boot_trace`) | Print the bootstrap's decisions to standard error. |
| `KRETRO_BOOT_NO_MEMFD` | tests | bootstrap (`tool.c`) | Never run the DwarFS tool from memory. |
| `KRETRO_BUNDLED_GAME` | old v3 bootstraps | nothing | Always removed by the bootstrap, so an old kretro's environment cannot reach a newer app. |
| `KRETRO_ISO_DIR` | user, tests | `install::iso_dir` | Where disc images are. Otherwise `<state>/iso` when it exists, else `iso/` in the working directory. |
| `KRETRO_MANIFESTS` | user, tests | `install` manifest search | An extra manifest directory, searched first. |
| `KRETRO_DISC_DB` | user | `disc` database | An extra known-disc list, searched first. |
| `KRETRO_UI_SCALE` | user, screenshot tools | `gui::forced_ui_scale` (kretro and the player) | An absolute UI scale, 0.5 to 4, in place of the one worked out from the window's size and the screen's density. Ctrl +, Ctrl - and Ctrl 0 still zoom on top of it. |
| `KRETRO_REDUCE_MOTION` | user, screenshot tools | `gui::reduce_motion` (kretro and the player) | `1` turns motion off: the page fade-in, focus glides, scrolling, the carousels' easing, dialog fades, the hero band's fade-in and its cross-fade between games snap to where they are going, and the blinking cursors are solid. |

`bundle::preview_env` removes every `KRETRO_*` variable before it starts a
built player, so that a preview sees what a stranger would.

The build and the tests have their own variables, which the programs never
read: `KRETRO_LICENSES` and `KRETRO_RTSRC` (`runtime/assemble-runtime.sh`),
`KRETRO_B3` (`test_bundle`, `bundle.sh`), `KRETRO_PARTS` (`test_boot.sh`),
`KRETRO_BIN`, and the `KRETRO_TEST_*` and `KRETRO_PLAYER*` settings of the
integration tests (see [testing.md](testing.md)). `KRETRO_VERSION` is a
preprocessor macro, not an environment variable, and is never defined (see
[building.md](building.md#kretro_version)).

## Module map

Each directory under `src/` is one module, usually in namespace
`kg::<module>`. `util` and `pack` are in `kg` itself (with `kg::cbor`,
`kg::fmt` and `kg::pe` in `util`), and `disc` also has `kg::iso`. A module
includes only modules below it:

```
util <- {config, gpu, pack} <- rt <- disc <- {backend, wine} <- session <- install <- bundle <- player <- gui <- cli <- apps
```

| Module | What it holds |
|---|---|
| `util/` | bytes, CBOR, BLAKE3 wrappers, paths, processes, TOML, file I/O, text, PE imports, safe names |
| `config/` | `config.toml`, and scaling geometry |
| `gpu/` | the host probe, NVIDIA matching, driver file links, capabilities |
| `pack/` | the kgpack header, metadata, tree and Merkle root, reading, writing, the DwarFS tool |
| `rt/` | `rt::Env`: the runtime's paths and the environment programs run under |
| `disc/` | disc images, containers, cue sheets, ISO 9660, serials, drives, CD audio, the known-disc list |
| `backend/` | the graphics backend policy table (`backend::plan`) |
| `wine/` | a prefix's registry fragment and the system files an installer left outside the game |
| `session/` | playing: lock, layers and mounts, prefix, compositor, backend, journal, saves, snapshots, unpacking, save transfer, the gamepad helper's command line |
| `install/` | the install engine (`install::Build`, split over `build_*.cpp`), manifests, the collection, drafts, sharing, keys, staging, the body layout |
| `bundle/` | the table of contents, `bundle.meta`, gamepad maps, building and verifying a player; `bundle/builder/` holds the Bundles page's decisions without the page |
| `player/` | the player's own logic without a window: its file, state directory, settings, verified memo, unpacking, prefix, desktop entry, command line, doctor, and `player::Player` |
| `gui/` | both windows. Shared pieces are at the top (`page.h`, `event_loop`, `focus`, `anim`, `scroll`, `layout`, `blocks`, `dialog`, `draw`, `status_bar.cpp`, `session_log`, `job`, `job_modal`, `widgets`, `window`, `scale`, `palette`, `texture`, `screen`, `file_list`, `gamepad_bridge`). `shelf/`, `wizard/`, `bundles/` and `stage/` are kretro's. `launcher/` is the player's. |
| `cli/` | kretro's command table and handlers |
| `apps/` | the three `main`s, and the player's command handlers |

The library, `LIB_SRC` in `mk/sources.mk`, is every module from `util` up to
`player`. kretro and kgpack link its objects
directly. The player links the same objects as the archive
`build/libkg.a`, so it takes only the objects something in it reaches. The
disc scanner, the recipe resolver and the wizard's engine therefore stay out
of the player unless the session or the save export needs them. The GUI and
the CLI are never in the library, so the tests and kgpack link no window
toolkit.

Registration is always explicit. The CLI is one table, `kCommands` in
`src/cli/commands.cpp`, in the order the help prints it. Each window's hosts
call `Pages::set` for each page in their constructor. Nothing registers itself
from a static initialiser, so the order is visible in one place and does not
depend on link order.

## Global state

- **`kg::state_dir`** is resolved once, on first use, from `KRETRO_STATE`, the
  XDG variables and `HOME`, and is never recomputed. Launching a game changes
  `HOME` for the child, and a later recomputation would answer with a
  different directory. Anything that sets `KRETRO_STATE` (the player's
  `settle_state`, tests) must do so before the first call.
- **`kg::use_bundle_layout`** switches every `game_*_dir` to the player's
  per-game layout. The player calls it once, from `settle_state`, before
  anything asks where a game lives.
- **The session's signal handlers.** `session::detail::MountInterruptGuard`
  installs handlers for SIGINT, SIGTERM and SIGHUP before the first mount and
  points `detail::g_active` at the session's layers. The handlers are never
  restored. A signal after the session still runs `release_on_signal`, which
  exits with 128 + the signal.
- **The bootstrap's static buffers.** `boot_runtime_dir` and `boot_cache_dir`
  return pointers to static buffers that callers keep. The bootstrap is
  single-threaded and execs away, so this is safe.
- The GPU probe's result and `rt::Env` are values that `main` builds once and
  passes down. They are not globals.

## Threads

The programs are single-threaded except for these threads:

- **The GUI main thread** handles SDL events, ImGui and all drawing, in both
  windows.
- **`gui::Job`** is one worker thread per owner: the shelf's
  `ShelfContext::job`, the wizard's `job_` (which runs the install, including
  `session::run_in_compositor`) and the launcher's `LauncherContext::job`. The
  log and the error are guarded by the job's mutex. `Job::locked` lets a body
  publish results under that lock.
- **The Bundles page** has its own threads: `prober_` reads each game's
  executable, `worker_` runs `build_from_draft`, and `repacker_` repacks one
  pack. Each has atomics and `build_mutex_` or `probe_mutex_` for what the page
  reads.
- **The stage** (`gui::Stage`) has a capture thread that reads the installer's
  X screen into a back buffer and swaps it under a mutex. The UI thread only
  uploads.
- **`install::Build`** has a counter thread that counts the files written
  under `drive_c` once a second, so the frame never walks the tree.
- **Compositor callbacks.** `CompositorOptions::on_display_ready` and
  `on_pgid` fire on the thread that called `run_in_compositor`. During an
  install that is the wizard's job thread. The UI thread then opens its own
  X connection to the display; no Xlib connection is shared between threads.
- **The preview** (`bundle::Preview`) starts the player in its own process
  group and reads its output through a non-blocking pipe. `poll` is called
  from the UI thread and never blocks.
- **Playing is synchronous.** `ShelfContext::play` and the launcher's play
  call `session::play` inside a frame, with the window hidden until the game
  exits. The event loop does not run meanwhile.

## Playing a game

`session::play` (`src/session/play.cpp`) is a list of named steps, and their
order is its behaviour:

```
refuse_fixed_backend  check_installed  take_lock  Pack::open  check_pack
MountInterruptGuard   mount            resolve_exe  choose_backend
make_prefix_dirs      link_install_dir build_wine_env_and_prefix  attach_discs
map_game_drive        run_after_prefix apply_dgvoodoo  apply_graphics
apply_renderer        geometry         compositor_options  command_line
[dry run returns]     launch           keep_frame  record
```

The ordering constraints:

1. A backend the caller already refused is reported before the lock is taken
   and before anything is mounted.
2. The lock is taken before the stale-mount sweep in `open_layers`, which
   unmounts whatever is at the game's mount points. Without the lock, that
   could be another copy's live game.
3. The signal handlers are in place before the first mount.
4. `BackendSettings::for_exe` is asked, and may refuse, before anything is
   written into the prefix.
5. The `install_dir` symlink is made before `prepare_prefix` imports the
   registry fragment that names it.
6. `Hooks::after_prefix` runs before dgVoodoo and the backend, which add to or
   replace the environment it was given.
7. The renderer is set last of the prefix writes, so the renderer comparison's
   explicit choice wins over the backend's.
8. A dry run returns after every prefix write and before anything is
   launched.

The lock, the pack, the layers and the guard are locals declared in that
order, so the mounts are closed before the lock is released.

### The panel and its units

`geometry` picks the scale from the panel the caller measured
(`DisplayOptions::panel_w/h` and `usable_w/h`, from `gui::desktop_size` in
`src/gui/screen.cpp`). The session layer links no SDL, so it cannot measure
the panel itself.

The panel has to be in the units the nested Weston sizes its window in. Under
a Wayland host Weston runs `--backend=wayland` and makes a window of
width x scale by height x scale in the host's *logical* units: it sets no
buffer scale and binds neither fractional scaling nor the viewporter. Under
an X11 host it runs `--backend=x11` and those units are X pixels. A Wayland
desktop at a fractional scale with native Xwayland scaling gives an X root
larger than the logical desktop (6144x3456 against 3072x1728 at 125% on a 4K
monitor), so asking SDL's x11 driver there would double the scale.

`desktop_size` therefore:

- with SDL's video down (`kretro play`, `kretro display`, the player's
  `play`): brings it up with the wayland driver for the logical size, then
  with the x11 driver for the work area (`_NET_WORKAREA`, which Wayland has no
  protocol for), which it scales by the ratio of the two desktops; on an X11
  host only the x11 driver is asked;
- with video up under a Wayland host (the shelf and the launcher, whose
  windows are X11 ones, and SDL runs one video driver at a time): runs
  `$KRETRO_APP panel`, a hidden command of both apps that measures as above
  and prints `W H UW UH`. It is run at each play rather than remembered,
  because the shelf outlives a change of monitor, scale or dock. If it
  fails, the driver that is up is asked, as before.

`config::compute_geometry` gives the fullscreen modes (Fit, Native) the whole
panel and a windowed Integer scale only the work area, so the window is not
squeezed by the window manager. A work area of zero means the whole panel.
Only the first display is measured.

### Getting the prefix ready without a display

Every Wine command that runs before the nested compositor exists runs with no
display: `wineboot --init`, the Graphics pin, `winecfg /v` and the Explorer
keys in `prepare_prefix`, the fragment import (`wine::apply_fragment`), the
CD-ROM drive (`disc::attach_cdrom`, at play and at install), the backend's
`regedit`, the renderer and Native geometry `reg add`s, and in the player the
`wineboot -u` of a prefix upgrade and the author's key. Each takes its
environment from `rt::offscreen`, a copy with `DISPLAY` and `WAYLAND_DISPLAY`
set to the empty string.

With the host's display these commands drew on the real desktop: wineboot's
"The Wine configuration is being updated" dialog on a first play, and, once
the prefix names the `kretro` virtual desktop, a full-screen "Wine Desktop"
for a second or so on every launch. The variables are set empty rather than
removed on purpose. With `DISPLAY` missing Wine goes on to its Wayland driver,
and libwayland connects to `wayland-0` when `WAYLAND_DISPLAY` is missing, so
either one unset still reaches the host. An empty name makes both connections
fail and leaves Wine on its null driver, which is all these commands need; the
runtime's prefix template is built the same way. The environment the game and
Weston get (`PlayState::we`) keeps the host's display, so only copies are
blanked.

`launch` stops the prefix's wineserver (`wineserver -k`, about 0.1 s) before
the game starts, so the game gets a server of its own whose processes,
explorer's desktop included, all start on the nested display. `-w` would wait
about three seconds for the lingering server instead.

### PlayRequest: who sets what

| Caller | Fields |
|---|---|
| `kretro play` (`src/cli/play.cpp`) | `id`, the display flags, the panel and its work area (`gui::desktop_size`), `backend.dgvoodoo`, `record`, `note`, `dry_run` |
| `kretro compare` | `id`, and per run `backend.wined3d_renderer`, `stop_after` and `capture_to` |
| the shelf (`ShelfContext::play`) | `id`, the panel and its work area |
| the player (`player::Player::play`) | `id`, `source` (the pack's range in the player file), `held_lock`, the game's display settings, `fullscreen`, the panel, `dry_run`, `backend.for_exe`, `game_drive` and `hooks.after_prefix` |
| the tests | `id`, `source`, `dry_run` and a fixed `backend.fixed` |

## The GUI: pages, hosts and jobs

The shelf and the launcher are built from the same parts:

- **`gui::Page`** (`src/gui/page.h`) is one screen. It has `draw()`, and
  optionally `back()` (Escape and pad B), `dropped(path)` and `busy()`.
- **`gui::Pages<Id, N>`** holds one page per value of the host's `Screen`
  enum, and knows which one is current. Changing page runs no code: there
  are deliberately no enter or leave hooks.
- **`gui::Host`** (`src/gui/event_loop.h`) is what `run_event_loop` drives:
  `frame()`, `on_event()`, `request_quit()`, `quit()` and `page_failed()`.
  `ShelfHost` (`src/gui/shelf/host.h`) and `LauncherHost`
  (`src/gui/launcher/host.h`) implement it. Each owns a context and its pages
  as members. The context is declared first, so it is destroyed last, because
  every page holds a reference to it.
- **The context** (`ShelfContext`, `LauncherContext`) is what the pages
  share: the collection or the player, textures, the status line, the job,
  and `go(Screen)`, the router the host installs. A page moves to another
  screen through `go` without knowing which pages exist. The few pages that
  need another page (the Game page opening the wizard, for example) are
  given it when they are constructed.
- **Focus** (`src/gui/focus.h`) is where the keyboard and the pad are. Each
  page window remembers the item that had the focus and comes back to it;
  otherwise it opens on the item it marked with `default_focus()`. Layout
  children and item panels are flattened (`ImGuiChildFlags_NavFlattened`),
  so the arrows cross them; panels of text are one stop that the keys scroll
  (`begin_text_panel`). The hosts take Escape and pad B only when
  `back_allowed()` (no popup, no field being typed in); a modal answers them
  itself through `modal_cancelled()`. The event loop's `note_input` keeps the
  focus shown while the keyboard or the pad is in use.
- **Blocks** (`src/gui/blocks.h`) are the larger pieces pages are built
  from, each one or more ordinary ImGui items under the caller's labels. The
  two game pages (kretro's and the player's) are a hero band over the game's
  picture and its blurred copy (`Textures::backdrop_*`), an action bar with
  the play or install button, the stats (`stat`) and the secondary actions
  as ghost buttons, and the page's sections under it, all in one region that
  scrolls the page from edge to edge (`begin_page_scroll`). Their header is a
  one-line trail (`set_page_trail`) since the hero says the title, and both
  list sessions with `session_log`.
  kretro's shelf is a `nav_sidebar` of the other screens, whose rows call
  `ShelfContext::nav(letter)`, the same actions the host's typed letters take
  (`ShelfHost::shortcut`), with a carousel of recent games and a grid beside
  it. Its tiles are noted with `tiles_item()` (`focus.h`), so Home and End
  go to the first and last and PageUp and PageDown page the main column and
  move to the nearest tile. Its view and sort are members of `ShelfPage` and never saved; the typed
  filter stays the host's (TEXTINPUT), shown in a field that is not an ImGui
  input.
  The player's grid (`GridPage`) has no header (`set_page_bare`): a hero band
  from the window's top edge for the game the focus is on, with the banner
  and the settings button laid over its top and Play with its stats over its
  foot (`HeroSpec::top` / `foot`), cross-fading from the game it showed
  (`HeroSpec::was_*`, `mix`). Under it the games are a carousel of covers,
  or past twelve games a grid in its own scrolling region. On the wide
  breakpoint the page keeps to the grid's centred column
  (`content_max_w(Content::Grid)`, `HeroSpec::content_right`); the band's
  picture still spans the window.
  The wizard's steps and the Bundles editor are flows: a one-line trail for
  a header, a `stepper` across the top (its nodes are buttons only for the
  steps the flow already lets you go to: for the wizard, the ones Escape
  would walk back to, `Wizard::can_go_back_to`; for a bundle, every step),
  the step in a scrolling pane, and a `flow_footer` pinned at the foot with
  Back and the step's primary. Back calls the same code Escape does
  (`Wizard::back`, `Bundles::back`; on the wizard's first step, where the
  shelf answers Escape, it presses Escape with `press_escape`), and the
  primary keeps the label and ID of the button it replaced. The wizard heads
  each step's content with its title (`step_heading`) in a form-width
  column; the install step folds the stepper to one line and gives the stage
  the rest. The Bundles list is a grid of cards (`tile_ex` with the bundle's
  banner or first cover) after a New bundle card, each with Open and Build
  again under it, shown while the card has the focus or the pointer.
  The other pages have the same trail for a header and put their title at
  the head of a centred column (`centre_column`, `content_max_w`,
  `step_heading`). Settings pages are rows (`setting_begin` /
  `setting_end`) whose combos are `step_combo`: Left and Right, while the
  combo has the keys, set the same value picking from its list does, and the
  focus stays on it. A game's timeline and a player's saves list snapshots
  as `snapshot_card`s, whose ghost button keeps the old Restore label and ID;
  the doctor's headings carry their verdict (`section(label, BadgeKind)`);
  the player's Checking and Blocked screens are a `centred_card` over the
  bundle's blurred banner with no header (`set_page_bare`).
- **Motion** (`src/gui/anim.h`) is asked for, never run: a widget asks each
  frame for the value to draw at (`anim_to`, `anim01`, `fade_in`), kept in
  its window's storage and stepped by the frame's time held to 50 ms, so it
  takes the same time at any frame rate. `PageWindow` fades the page in over
  120 ms by scaling the alpha of what the page and its child windows drew
  (the palette colours pages draw with by hand do not go through the
  style's alpha); the status bar is on the background layer and is not
  faded. `reduce_motion()` (`KRETRO_REDUCE_MOTION`) snaps every one.
- **Sections** (`nav_section_begin` / `nav_section_end`, `focus.h`) split a
  page into the parts Tab, Shift+Tab and the pad's LB and RB move between,
  each landing on the item it last had or its first. The shelf's are the
  sidebar, the recent row and the grid; the launcher grid's are the hero band
  and the row (or grid) of games; a wizard or bundle step's are the stepper,
  the step and the footer; kretro's settings' are the window, scaling and
  folders, the Library's the folders and the discs, the Import page's the
  pack and its browser, and a player's saves' the transfer band and the
  snapshots.
- **`gui::Job`** runs a long task on a worker thread with a log. The shelf and
  the wizard show it in the shared "busy" modal (`draw_job_modal`). The
  launcher has its own smaller working modal, and chains work with
  `LauncherContext::run_job(title, fn, then)`. `finish_job` takes the
  continuation out of `then` before calling it, so the continuation can start
  the next job.

kretro's window holds the shelf's nine screens (Shelf, Game, Create, Import,
Doctor, Library, Settings, Timeline and Bundles). The wizard is eight steps
inside Create, and the Bundles page has its own steps inside Bundles. The
player's window holds the launcher's ten screens (Checking, Blocked, Grid,
Game, Saves, Display, Controls, Bundle, Licenses and About). The player links
none of kretro's pages.

## Two runtimes from one image

`runtime/Dockerfile.runtime` builds one image, `kretro-runtime`, with
Ubuntu 26.04, Wine 11 (new WoW64), DXVK 3 and 2.7, cnc-ddraw, Mesa, Weston,
Xwayland, SDL and GStreamer, and a prefix template made once at build time.
`runtime/build-runtime.sh` cuts it twice with `assemble-runtime`:

- `build/runtime.dwarfs` is kretro's runtime. It is everything, including the
  authoring tools that read discs and archives and run installers, plus the
  local manifests and known-disc list when the checkout has them.
- `build/player-runtime.dwarfs` is the player's runtime. It is the same tree
  with everything in `runtime/player-prune.txt` removed, packed with
  `mkdwarfs --categorize`. `PLAYER_DLL_WHITELIST=1` additionally keeps only
  the Wine DLLs in `runtime/player-keep-dlls.txt`.

Both carry the same Wine and graphics stack, so what an author test-plays in
kretro is what a player runs. The only path the bootstrap relies on inside
either runtime is `lib/ld-linux-x86-64.so.2`. The app finds everything else
through `rt::Env`.
