# kretrogame

kretrogame turns classic Windows games that you install from your own disc
images into self-contained Linux executables. It has two parts:

- **kretro**, the authoring tool. It reads disc images, runs each game's
  original installer in a window you control, stores the installed game as a
  single pack file, lets you test-play it, and builds players.
- **The player**, the file kretro produces. A player is one executable that
  holds a compact runtime and one or more games. Whoever receives it runs it,
  picks a game from its launcher, and plays. It needs no installation, no Wine
  on the host, no root access and no network connection.

Both programs are single, statically bootstrapped ELF files. They carry their
own C library, Wine, Mesa and display stack, so they run on glibc and musl
distributions alike, including Alpine.

## Contents

- [Features](#features)
- [How it works](#how-it-works)
- [Requirements](#requirements)
- [Building from source](#building-from-source)
- [Quick start](#quick-start)
- [Using kretro](#using-kretro)
- [Using a player](#using-a-player)
- [Graphics and display](#graphics-and-display)
- [Saves and snapshots](#saves-and-snapshots)
- [Distributing players](#distributing-players)
- [Known limitations](#known-limitations)
- [Development](#development)
- [License](#license)

## Features

- **Disc images as they come.** Reads `.iso`, `.bin`/`.cue`, `.img` and `.mdf`
  images, and `.zip`, `.7z` and `.rar` archives, including archives nested
  inside other archives. Multi-disc sets and duplicate dumps are recognised.
  CD audio tracks are preserved as FLAC.
- **Installers run as intended.** The game's own setup program runs inside
  kretro's window with every disc of the set mounted as a CD-ROM drive that
  carries the original volume label and serial number.
- **One file per game.** An installed game is a single DwarFS-based pack with a
  Merkle root, mounted rather than copied when played.
- **One file to distribute.** A player bundles the runtime and any number of
  games, with a launcher, per-game settings and optional cover art.
- **Portable.** Runs on any x86-64 Linux distribution with a Wayland or X11
  session. Games mount in place through FUSE, and the player falls back to
  unpacking a game, with your consent, where FUSE is unavailable.
- **Pixel-exact scaling.** Integer, fit and native scaling modes, chosen per
  game.
- **Automatic save history.** Every play session becomes a snapshot that can be
  restored, exported or imported.
- **Offline by design.** Neither program makes network connections.

## How it works

Every kretro and player file is laid out the same way:

```
┌──────────────────────────┐
│ static bootstrap (musl)  │  finds the payloads below and starts the program
├──────────────────────────┤
│ DwarFS tool              │  mounts or unpacks the images below
├──────────────────────────┤
│ runtime image            │  glibc, Wine, Mesa, Weston, Xwayland, SDL, ...
├──────────────────────────┤
│ application              │  kretro, or the player
├──────────────────────────┤
│ bundle metadata + packs  │  players only: title, settings and the games
├──────────────────────────┤
│ table of contents        │  offset, length and BLAKE3 hash of each entry
└──────────────────────────┘
```

When the file is started, the bootstrap reads the table of contents, verifies
it, mounts the runtime image directly from the file, and runs the application
through the runtime's own dynamic loader. Nothing is linked against host
libraries, apart from the NVIDIA driver described under
[Graphics and display](#graphics-and-display).

Games run under Wine 11 in its WoW64 mode, so 32-bit games need no 32-bit host
libraries. Each game is displayed in a nested Weston compositor with its own
Xwayland server, which is how kretro controls scaling, screenshots and input
without depending on the host desktop.

## Requirements

### Running kretro or a player

- Linux on x86-64 (glibc or musl).
- A Wayland or X11 desktop session.
- FUSE 3 (`fusermount3`) is recommended. Without it, the runtime and each game
  are unpacked to disk before use.
- A GPU with Vulkan 1.3 or later is recommended for Direct3D games. Mesa drivers
  for Intel, AMD and nouveau are included. NVIDIA's proprietary driver is used
  from the host when its user-space libraries match the loaded kernel module.

### Building

- GNU Make, Docker and Python 3. All compilation happens inside two Docker
  images defined in `runtime/`, so no compiler toolchain is needed on the host.
- Network access and roughly 10 GB of free disk space for the first runtime
  build, which downloads Wine, DXVK and other components.

## Building from source

```sh
make runtime   # build the kretro and player runtime images (slow, first time only)
make           # build the programs and link build/kretro
```

`make runtime` is needed only once and again whenever `runtime/` changes. The
result is `build/kretro`, about 780 MB, which carries the player base (about
380 MB) that every player is built from.

Run `make help` for the full list of targets.

## Quick start

```sh
# 1. Install a game from its disc images. Opens the installation wizard.
build/kretro create

# 2. Check that it runs.
build/kretro list
build/kretro play example-game

# 3. Build a player containing one or more installed games.
build/kretro bundle build -o classics.run --title "Classics" --version 1.0 \
    --rights example-game another-game

# 4. Run the player, or give it to someone else.
./classics.run
```

Running `build/kretro` with no arguments opens the graphical interface (the
*shelf*), where all of the above is also available.

## Using kretro

### Installing a game

`kretro create` opens the installation wizard, which takes a game through eight
steps:

1. **Sources**: add the game's files, such as disc images, archives, a folder
   or an installer executable. The wizard works out how many discs they hold.
2. **Identity**: name the game. If a manifest matches the discs, the wizard
   offers to prefill its answers.
3. **Method**: choose how to install. The options are running the setup program
   under Wine, running a standalone installer, copying a directory from the
   disc, or unpacking an archive from the disc. Only methods the files support
   are offered.
4. **Installation**: the installer runs inside the wizard's window with every
   disc mounted. If it asks for another disc, you can swap discs from a menu.
5. **Location**: the wizard shows what the installer wrote and asks which
   directory is the game.
6. **Launch**: choose the executable that starts the game, and any arguments.
7. **Presentation**: set the resolution and display settings.
8. **Build**: write the pack, optionally including the disc images so that
   games that check for their disc can find it.

Games that install by copying or unpacking can be installed without a display
using `kretro install <id> --headless`, provided a manifest (see below)
describes how. Installers that need interaction always use the wizard.

### Manifests

A manifest is an optional TOML file that records what is known about a game:
its setup program, installation method, executable, arguments, resolution and
Windows version. kretro reads manifests from `games/` in a source checkout and
from `manifests/` in its data directory. None are shipped with kretro. When a
manifest matches the discs you add, the wizard offers to prefill its answers,
and every field stays editable.

### Serial numbers

kretro never generates or retrieves serial numbers. `kretro key <id> <serial>`
stores a serial locally so that reinstalls can show it next to the installer
that asks for it. Serials are removed from packs. A player includes one only if
the author explicitly chooses to embed it for that game.

### Building players

The **Bundles** page in the graphical interface walks through building a player:

1. **Identity**: title, bundle id, version, banner and icon. The bundle id
   determines where players store their data. Keep it unchanged across versions
   so that players can find earlier saves.
2. **Games**: choose installed games and their order.
3. **Per-game settings**: name, year, cover art, graphics backend, display mode,
   gamepad mapping, additional files, and an optional embedded serial.
4. **Checks**: warnings that must be resolved or acknowledged, such as detected
   copy protection or missing cover art.
5. **Size**: the exact output size, with warnings at 2 GiB and 4 GiB, the upload
   limits of common hosting sites and the FAT32 file size limit.
6. **Rights**: confirmation that you are entitled to distribute the games.
7. **Build**: the player is written to a temporary file, verified entry by
   entry, and renamed only when complete.
8. **Preview**: runs the new player in a clean environment, as a recipient would
   see it.

The page remembers every bundle, so a new version is a single rebuild. The same
build is available from the command line:

```sh
kretro bundle build -o FILE|DIR [--id ID] [--title TITLE] [--version VERSION] \
    [--base PLAYER-BASE] --rights GAME...
kretro bundle list              # bundles remembered by the Bundles page
kretro bundle rebuild <id>      # rebuild a remembered bundle
```

### Command reference

| Command | Description |
|---|---|
| `kretro` | Open the graphical interface. |
| `kretro create` | Install a game with the wizard. |
| `kretro install <id> [--headless] [--force] [--keep-tree] [--no-discs]` | Install a game described by a manifest. |
| `kretro scan [dir] [--write-db]` | Identify every disc image in a directory. |
| `kretro identify <image>` | Identify one disc image. |
| `kretro contents <archive[#LABEL]>` | List the files on a disc. |
| `kretro games` | List manifests and whether their discs are present. |
| `kretro list` | List installed games. |
| `kretro play <id> [--fullscreen] [--integer\|--fit\|--native] [--scale N] [--dry-run] [--note TEXT]` | Play a game. |
| `kretro show <id>` | Open the graphical interface on one game. |
| `kretro display <id>` | Show how a game will be scaled on this screen. |
| `kretro compare <id>` | Capture the game under each renderer for comparison. |
| `kretro swap <id> <n>` | Insert disc *n* during an installation. |
| `kretro key <id> [serial]` | Show or store a game's serial number. |
| `kretro journal <id>` | Show play history. |
| `kretro saves <id>` | List save snapshots. |
| `kretro restore <id> <generation>` | Restore a snapshot. |
| `kretro export <id> [--capsule\|--recipe] [-o FILE]` | Export a game for another kretro user. |
| `kretro export-saves <id>` | Export a game's saves. |
| `kretro import <file>` | Import a capsule, or rebuild a game from a recipe. |
| `kretro import-saves <id> <file>` | Import saves. |
| `kretro bundle build\|list\|rebuild ...` | Build players (see above). |
| `kretro verify <id>` | Verify a pack's integrity. |
| `kretro uninstall <id>` | Remove an installed game. |
| `kretro doctor [--save FILE]` | Check this machine's capabilities. |
| `kretro info` | Show runtime and data paths. |
| `kretro wine [args...]` | Run the bundled Wine. |
| `kretro exec <program> [args...]` | Run a program from the runtime. |

A *capsule* is a complete pack for another kretro user. A *recipe* contains only
a game's identity, the fingerprints of its discs and the expected Merkle root,
so the recipient can rebuild the game from their own copy of the discs.
Rebuilding through an interactive installer may not reproduce identical files,
and kretro reports whether the result matches.

### Data locations

| Path | Contents |
|---|---|
| `~/.local/share/kretro/` | Installed games, saves, Wine prefixes, remembered bundles |
| `~/.local/share/kretro/config.toml` | Settings: window, scaling (global and per game), library folders |
| `~/.local/share/kretro/manifests/` | Local manifests |

`$XDG_DATA_HOME` is respected. A directory named `kretro-data` next to the
kretro executable is used instead when present, which makes kretro portable on
external storage.

## Using a player

Running a player opens its launcher: the bundle's title and banner, and a tile
for each game with **Play**, **Saves**, **Display** and **Controls**. A player
that holds a single game starts that game directly. On first run, the player
checks the system, reports anything that would prevent a game from running, and
offers to add itself to the applications menu.

```
./classics.run                            open the launcher, or start the only game
./classics.run play <game> [--dry-run]    start a game directly
./classics.run --doctor [--save FILE]     check this machine's capabilities
./classics.run saves <game> export FILE   export a game's saves
./classics.run saves <game> import FILE   import saves; current saves are kept as a snapshot
./classics.run --extract-to DIR [<game>]  unpack a game for machines without FUSE
./classics.run --licenses                 show the licences of the included components
```

### Where a player stores data

| Path | Contents |
|---|---|
| `~/.local/share/<bundle-id>/` | Default data directory (`$XDG_DATA_HOME` is respected) |
| `<data>/launcher.toml` | Window state and first-run choices |
| `<data>/<game>/prefix/` | The game's Wine prefix |
| `<data>/<game>/saves/` | Files the game has written, and snapshots |
| `<data>/<game>/settings.toml` | Display and controller settings |

Data is keyed by the bundle id, so a newer version of the same player finds the
saves of an older one.

**Portable mode.** If a directory named after the player with `-data` appended
(for example `classics.run-data/`) exists next to the player, is writable and
supports symbolic links, it is used instead. This does not work on FAT or exFAT
file systems, which lack symbolic links; the player falls back to the default
location and says so.

### Systems without FUSE

Without FUSE, the runtime is unpacked to `~/.cache/kretro` once. Games are
larger, so the player asks before unpacking one, shows the space required and
checks that it is available. `--extract-to DIR` unpacks a game to a location of
your choice instead.

### Integrity

Every entry in a player carries a BLAKE3 hash. Each game is verified the first
time it is played. If one game is damaged, the player reports it and the other
games remain playable.

## Graphics and display

kretro selects a rendering path per game from the Direct3D or OpenGL libraries
its executable imports, and from what the host supports. The author of a player
can override the choice per game.

| Game API | Default renderer |
|---|---|
| Direct3D 8 and 9 | DXVK 3 (Vulkan 1.4) or DXVK 2.7 (Vulkan 1.3) |
| Direct3D 5 to 7, 3D DirectDraw | WineD3D on Vulkan |
| 2D DirectDraw | cnc-ddraw |
| OpenGL | Mesa, or the host NVIDIA driver |

Without a suitable GPU, games fall back to software rendering, unless the
player's author has marked a game as requiring a GPU.

**NVIDIA.** The proprietary driver's user-space libraries must match the
running kernel module exactly, so they are taken from the host after checking
the version in `/sys/module/nvidia/version`. If they are missing or do not
match, the player explains what to install. It never downloads drivers.

**Scaling.** Each game runs in a nested compositor at its native resolution and
is scaled in whole-number steps:

- `integer`: a whole-number scale in a window. It defaults to the largest scale
  that fits, and `--scale N` sets it explicitly.
- `fit`: the largest whole-number scale that fits, full screen with borders.
- `native`: the game renders at the screen's resolution, which suits later 3D
  titles.

`kretro display <id>` shows the resulting size on the current screen.

**Steam Deck.** Under gamescope (Game Mode), the nested compositor is skipped
and the game runs full screen on gamescope's display.

## Saves and snapshots

Everything a game writes goes to a separate writable layer on top of the
read-only pack, so saves never need to be located by hand. Each play session
ends with a snapshot, including a screenshot of the last frame. The Timeline
page in kretro, and the Saves page in a player, list snapshots with their date,
play time and size. Restoring a snapshot first snapshots the current state, so
a restore can always be undone.

## Distributing players

Before distributing a player, make sure that:

- **You have the right to distribute the games.** kretro records your
  confirmation in the player but cannot verify it.
- **You meet the obligations of the bundled components.** A player includes
  Wine, glibc, Mesa, GStreamer, SDL, DXVK, cnc-ddraw, FUSE and other components
  under their own licences, including the LGPL and GPL. Their notices are
  included in every player and shown by `--licenses`. `SOURCES.txt` in the same
  directory lists the exact upstream source of each component, which you can
  use to provide corresponding source code where those licences require it.
- **Any additional files you add are yours to distribute.** dgVoodoo, for
  example, is never bundled by kretro because its licence does not allow it in
  general-purpose launchers. It may only be added to an individual game by the
  author.

A player's size is roughly the 380 MB player base plus the size of each game's
pack. Packs may include disc images, which are deduplicated against the
installed files. The Bundles page shows the exact size before building.

## Known limitations

- **CD audio is not played.** Audio tracks are preserved in the pack, but
  nothing plays them yet. Games that stream music from the disc run without it.
- **Sector-level copy protection is not supported.** SafeDisc, SecuROM and
  similar schemes read the disc below the file system, which a directory-backed
  CD-ROM drive cannot provide. Such games install but may refuse to start.
  kretro warns about this when building the pack and the player.
- **Relative mouse movement through the nested compositor is untested.**
  First-person games that rely on unbounded mouse movement may not turn past
  the window edge.
- **Wine is not yet pruned.** The player runtime removes authoring tools but
  keeps all of Wine's libraries. A pruned build is available with
  `make runtime PLAYER_DLL_WHITELIST=1` but is off by default.
- **No user-namespace mounting.** On systems without FUSE, the runtime is
  unpacked rather than mounted inside a user namespace.
- **Strictly confined sandboxes.** When a player is started from inside a
  strictly confined snap, AppArmor can prevent the game's writable layer from
  mounting. The player then offers to unpack the game instead.

## Development

### Repository layout

| Path | Contents |
|---|---|
| `boot/` | The static musl bootstrap shared by kretro and players: `main.c` runs the start-up sequence, and `layout.c`, `tool.c`, `unpack.c`, `dirs.c`, `io.c`, `proc.c`, `userns.c`, `diag.c` and `util.c` hold one concern each, sharing only `boot.h` |
| `src/apps/` | The `main` of each program: `kretro/`, `player/` (with its command handlers) and `kgpack/` |
| `src/cli/` | kretro's command table and a file for each group of commands |
| `src/util/` | Bytes, CBOR, hashing, paths, processes, TOML, text and safe names |
| `src/config/` | `config.toml` and scaling |
| `src/gpu/` | The GPU probe, NVIDIA matching and capabilities |
| `src/pack/` | The kgpack format: header, metadata, tree and Merkle root, reading and writing |
| `src/rt/` | The runtime's paths and environment |
| `src/disc/` | Disc images, containers, ISO 9660, serials, drives and CD audio |
| `src/backend/` | The graphics backend policy |
| `src/wine/` | A prefix's registry fragment and system files |
| `src/session/` | Playing a game: lock, layers, prefix, compositor, journal, saves and snapshots, one file per concern |
| `src/install/` | Installing a game: the install engine (`build_*.cpp`), manifests, the collection, drafts, sharing and keys |
| `src/bundle/` | The table of contents, `bundle.meta`, and building and verifying players; `builder/` holds the Bundles page's decisions without the page |
| `src/player/` | The player's logic without a window: its file, state, settings, unpacking, prefix and command line |
| `src/gui/` | Both windows. Shared pieces at the top; `shelf/`, `wizard/`, `bundles/` and `stage/` are kretro's, and `launcher/` is the player's |
| `runtime/` | Dockerfiles and scripts that build the toolchain images and runtime images |
| `scripts/` | Linker for the final executables, and repository checks |
| `tests/unit/` | One C++ test program per subsystem, and the shared helpers in `tests/unit/support/` |
| `tests/integration/`, `tests/fixtures/` | Shell tests against real builds, runtimes and discs, and the stand-ins the tests build from |
| `docs/` | Architecture, file formats, building and testing |
| `mk/` | Makefile fragments |
| `licenses/` | Licence texts and source list for bundled third-party components |
| `third_party/` | Vendored BLAKE3, Dear ImGui and stb_image sources |

### Make targets

| Target | Description |
|---|---|
| `make` | Build everything and link `build/kretro` |
| `make runtime` | Build both runtime images (slow) |
| `make test` | Unit tests and bootstrap tests. Needs no games and no runtime image |
| `make test-runtime` | Check both runtime images and start their Wine |
| `make lint` | Run clang-tidy over `src/` |
| `make format` | Format the files listed in `FORMAT_CLEAN` with clang-format |
| `make check-generic` | Check that no tracked file refers to games in your local collection |
| `make clean` | Remove `build/` |
| `make help` | List all targets |

### Tests

`make test` runs the unit tests and needs no game data. The integration tests
in `tests/integration/` install real games from your own disc images, build
players and run them on several distributions. They read their configuration
from `tests/local.env`, which is not tracked, and skip when it is absent. See
[docs/testing.md](docs/testing.md) for setup.

### Documentation

- [docs/architecture.md](docs/architecture.md): the programs, the process
  model, the environment contract, the module map, threads and the GUI.
- [docs/file-format.md](docs/file-format.md): the specification of every
  format kretrogame writes.
- [docs/building.md](docs/building.md): the images, the make targets and
  the development loops.
- [docs/testing.md](docs/testing.md): the unit and integration tests.

### Environment variables for development

| Variable | Effect |
|---|---|
| `KRETRO_GUI` | Run kretro with a freshly built application instead of the embedded one |
| `KRETRO_PLAYER_BASE` | Build players from the given player base instead of the embedded one |
| `KRETRO_DEBUG` | Print the bootstrap's decisions to standard error |

See [CONTRIBUTING.md](CONTRIBUTING.md) for coding conventions and how to submit
changes.

## License

kretrogame is released under the [MIT License](LICENSE). Bundled third-party
components are distributed under their own licences, listed in
[licenses/](licenses/).
