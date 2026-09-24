# Building

Nothing is compiled on the host. The Makefile runs every compiler, formatter
and linter inside Docker images that it builds from `runtime/`. The host needs
GNU make, Docker and Python 3.

```sh
make runtime   # once, and again only when runtime/ changes; slow
make           # build/kretro, with build/player-base inside it
make help      # every target
```

The Makefile is split into pieces under `mk/`:

| File | Contents |
|---|---|
| `mk/config.mk` | tools, images and flags |
| `mk/sources.mk` | what goes into each program, and the list of unit tests |
| `mk/rules.mk` | compiling and linking |
| `mk/tests.mk` | `make test`, the boot tests, `make test-runtime` |
| `mk/runtime.mk` | the images, the runtimes, `link` and `player-base` |
| `mk/quality.mk` | `make format`, `format-check`, their `-all` forms, `lint` and `check-generic` |

## Images

| Image | Built from | Contents | Used for |
|---|---|---|---|
| `kretro-guibuilder` | `runtime/Dockerfile.guibuilder` | Ubuntu 26.04 (the runtime's own base, so SDL and glibc headers match what ships), build-essential, pkg-config, Python 3, SDL2, zstd, zlib, X11 and XTest headers, clang-format and clang-tidy | all C++, the unit tests, `test_bundles_page`, format and lint |
| `kretro-builder` | `runtime/Dockerfile.builder` | Debian bookworm, musl-tools, CMake, FUSE 3 headers, Python 3, and the static `dwarfs-universal` release at `/usr/local/bin` | the bootstrap, the boot tests, `link` and `player-base`, and staging the DwarFS tool for the runtimes |
| `kretro-runtime` | `runtime/Dockerfile.runtime` | Ubuntu 26.04 with Wine 11, DXVK, cnc-ddraw, Mesa, Weston, Xwayland, SDL, GStreamer, winetricks and a prefix template | the source of both runtimes; never run as a toolchain |

`make images` builds the two toolchain images when they are missing, and
every target that compiles depends on it. `make lint` and `make format`
rebuild `kretro-guibuilder` if an older image lacks the clang tools.
`make runtime` always rebuilds `kretro-runtime`.

## Targets

```
all ─┬─ images
     ├─ app          (in kretro-guibuilder)  build/kretro-gui, build/kgpack, build/kretro-player
     ├─ boot         (in kretro-builder)     build/bootstrap
     ├─ player-base  (in kretro-builder)     only when build/player-runtime.dwarfs exists
     └─ link         (in kretro-builder)     build/kretro
```

- **`make`** (`all`, also `make kretro`) runs `app`, `boot`, `player-base`
  when the player runtime exists (otherwise it warns), and `link`, each inside
  its image. `app`, `boot`, `player-base` and `link` are the steps themselves:
  they run their commands directly and are meant to run inside the image, as
  `make` runs them.
- **`make programs`** runs `app` and `boot` only. It needs no runtime, which
  is what CI builds.
- **`make link`** runs `scripts/kretro-link.py` on the bootstrap, the builder
  image's `dwarfs-universal`, `build/runtime.dwarfs` and `build/kretro-gui`,
  and adds `--player-base build/player-base` when that file exists. Without
  it, kretro links with a warning and cannot build players. It checks that its
  inputs exist rather than naming them as prerequisites, because it runs in
  the musl image and must not try to rebuild the GUI there.
- **`make player-base`** links `build/player-base` the same way from
  `PLAYER_BOOTSTRAP` (default `build/bootstrap`),
  `build/player-runtime.dwarfs` and `build/kretro-player`.
- **`make test`** builds the unit tests, `test_bundles_page` and
  `build/kretro-b3` in `kretro-guibuilder`, runs each unit test, runs
  `test_bundles_page` in that image, then runs `make boot-test` in
  `kretro-builder` with a noexec tmpfs at `/nx`. See
  [testing.md](testing.md).
- **`make test-runtime`** runs `tests/integration/runtime.sh` against both
  runtimes. It needs `make runtime` first.
- **`make lint`** runs clang-tidy over every `.cpp` under `src/`
  (`LINT_SRC`), one file per process, as many in parallel as there are cores,
  with `.clang-tidy`'s checks. It must pass.
- **`make format`** rewrites the files listed in `FORMAT_CLEAN`
  (`mk/sources.mk`) to `.clang-format`, and **`make format-check`** fails if
  any of them differs; CI runs it. Those are the files written as new code,
  kept formatted in full. Every other file is formatted only on the lines an
  edit changes (`git clang-format`, run on the host: the image has no git).
  See `CONTRIBUTING.md` for the rule.
- **`make format-all`** rewrites every `.c`, `.cpp` and `.h` under `src/`,
  `boot/` and `tests/` (`FORMAT_SRC`). It breaks up the older code's
  deliberate multi-statement lines, so it is not for routine use.
  **`make format-check-all`** lists every place in that tree out of style and
  never fails.
- **`make check-generic`** runs `scripts/check-generic.sh` on the host. It
  fails if a tracked file names anything from your local game collection.
- **`make runtime`** builds `kretro-runtime` and runs
  `runtime/build-runtime.sh`, which writes both `build/runtime.dwarfs` and
  `build/player-runtime.dwarfs`. **`make player-runtime`** writes only the
  player's.
- **`make shell`** opens a shell in `kretro-guibuilder`. **`make clean`**
  removes `build/`.

## What build/ holds

| Path | What |
|---|---|
| `build/src/...`, `build/tests/...`, `build/third_party/...` | objects and their `.d` files, at the path of their source |
| `build/musl/...` | the bootstrap's objects and its own BLAKE3 objects, compiled with musl |
| `build/kretro-gui` | kretro's app |
| `build/kretro-player` | the player's app |
| `build/kgpack` | the kgpack tool |
| `build/libkg.a` | the library as an archive, for the player |
| `build/bootstrap` | the static bootstrap; the build fails if it is not static |
| `build/kretro-b3` | the linker's static BLAKE3 tool |
| `build/dwarfs-universal` | the DwarFS tool, staged by `make runtime` |
| `build/runtime.dwarfs`, `build/player-runtime.dwarfs` | the two runtimes |
| `build/player-base` | a player without games |
| `build/kretro` | kretro, the file that is run and distributed |
| `build/test_*`, `build/boot-test/` | the unit tests and the boot test fixtures |
| `build/scratch/` | scratch space by convention (see [testing.md](testing.md#scratch-space-and-fuse)) |

Linking writes `<out>.partial` and renames it into place only when it is
complete, so a failed link never leaves a file that looks finished.

## Variables

Every variable in `mk/config.mk` can be overridden on the command line, for
example `make BUILD=out` or `make CXXFLAGS=...`:

| Variable | Default |
|---|---|
| `BUILD` | `build` |
| `BUILDER`, `GUIBUILDER`, `RUNTIME` | `kretro-builder:latest`, `kretro-guibuilder:latest`, `kretro-runtime:latest` |
| `CXX`, `CXXFLAGS` | `g++`, `-std=c++20 -O2 -g -Wall -Wextra` |
| `CC`, `CFLAGS` | `gcc`, `-O3 -Wall` |
| `MUSL_CC`, `MUSL_FLAGS` | `musl-gcc`, `-static -O2 -Wall -Wextra -std=c11` |
| `PLAYER_DLL_WHITELIST` | `0`. With `1`, `make runtime` keeps only the Wine DLLs in `runtime/player-keep-dlls.txt` in the player runtime. |
| `PLAYER_BOOTSTRAP` | `build/bootstrap` |
| `LINT_CHECKS` | empty. When set, it is passed to clang-tidy as `--checks` on top of `.clang-tidy`, to measure one check without editing the file: `make lint LINT_CHECKS='-*,performance-unnecessary-value-param'` |

**`DEPFLAGS`** is `-MMD -MP`. Every compile writes a `.d` file listing the
headers it included, and `mk/rules.mk` includes them all, so editing a header
rebuilds everything that uses it. It is kept separate from `CXXFLAGS` so that
overriding the flags cannot turn it off. Without it, objects compiled against
an old struct layout would be linked with objects using the new one.

Sources are listed explicitly in `mk/sources.mk`, and a new `.cpp` file must be
added to the right list there (`LIB_SRC` and its parts, `GUI_SRC`,
`PLAYER_GUI_SRC`, or one of the app lists). The bootstrap is the exception:
every `boot/*.c` is compiled.

## Development loops

Relinking kretro means repacking nothing, but it still copies a runtime of
hundreds of megabytes. Two variables avoid it while working:

- **`KRETRO_GUI`**: `make programs`, then
  `KRETRO_GUI=$PWD/build/kretro-gui build/kretro ...` runs the freshly built
  app instead of the embedded one. It still runs through the runtime's loader
  against the runtime's libraries, so the real arrangement is what is tested.
  Only kretro honours it; a player ignores it. The bootstrap removes it from
  the environment either way, so nothing it starts sees it.
- **`KRETRO_PLAYER_BASE`**: `make` (which links `build/player-base` once
  `make runtime` has run), then `KRETRO_PLAYER_BASE=$PWD/build/player-base build/kretro bundle build ...`
  builds players from that player base instead of the one kretro carries.

`KRETRO_DEBUG=1` makes the bootstrap print what it decided.

## KRETRO_VERSION

`bundle::meta_from_draft` writes `kretro_version` into `bundle.meta` from the
preprocessor macro `KRETRO_VERSION`, or `dev` when it is not defined. The
build never defines it, so every bundle records `dev`. This is deliberate for
now: defining it would change the bytes of every `bundle.meta` built, and it
is left for a change of its own.

## Disk and network

- The toolchain images download Debian and Ubuntu packages and the DwarFS
  release the first time. After that, `make` and `make test` need no network.
- `make runtime` downloads Wine, DXVK, cnc-ddraw and winetricks, and needs
  roughly 10 GB of free disk space the first time. It is slow.
- `build/kretro` is about 780 MB, of which the player base is about 380 MB.
- Neither kretro nor a player needs the network to build a player: the player
  base is inside kretro.
