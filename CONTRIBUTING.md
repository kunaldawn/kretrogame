# Contributing to kretrogame

## Building

Nothing is compiled on the host. The Makefile runs every compiler inside one
of two Docker images, which it builds from `runtime/` the first time:

- **kretro-guibuilder** (Ubuntu 26.04, the runtime's own base) builds the C++,
  and runs clang-format and clang-tidy.
- **kretro-builder** (musl, and the static DwarFS tool) builds the bootstrap and
  links the final files.

You need Docker, GNU make and Python 3.

```sh
make runtime   # once, and again only when runtime/ changes; slow
make           # build/kretro, with build/player-base inside it
make help      # every target
```

`make` without a runtime still builds the programs; linking `build/kretro`
needs `build/runtime.dwarfs`, and `build/player-base` needs
`build/player-runtime.dwarfs`.

## Testing

```sh
make test          # unit tests, the Bundles page, the bootstrap
make test-runtime  # both runtimes: what they carry, and that Wine starts
```

`make test` needs neither game media nor the runtime, and is what CI runs. It
must stay green, with the same number of checks or more, on every change.

The scripts in `tests/integration/` exercise real installs and built players.
They need `build/kretro`, and the ones that install games read which games,
and where their discs are, from `tests/local.env`, which is gitignored; each
skips cleanly when what it needs is missing. `tests/README.md` has the details.

Game media and anything describing a game collection are never committed:
no manifests, disc lists, image names, titles or ids in code, comments, tests
or docs. Unit tests use stand-ins (`example-game`, `DEMO_DISC`, `game.exe`).
`make check-generic` searches every tracked file for the names in your own
local data (`games/`, `db/`, `iso/`, `tests/local/`) and fails on any hit; run
it before sending a change. CI cannot, having no collection.

Scratch space: point `TMPDIR` into the build tree (for example
`export TMPDIR=$PWD/build/scratch/tmp`) rather than `/tmp`, and unmount any
FUSE mount a test leaves behind with `fusermount3 -u`.

## Formatting and linting

```sh
make format        # rewrite C and C++ in src/, boot/ and tests/ to .clang-format
make format-check  # fail if anything differs
make lint          # clang-tidy over src/ with .clang-tidy; must pass
```

`.clang-format` follows the existing style as closely as clang-format can.
The code deliberately puts short related statements on one line, which
clang-format cannot keep, so the tree as a whole is not formatted by it:
format what you write, and keep the surrounding style. `.clang-tidy` explains
each check it turns off.

`.editorconfig` covers whitespace for every file type: two spaces for C, C++
and shell, four for Python, tabs in makefiles.

## Where things live

| Path | What |
|---|---|
| `src/` | kretro, the player and `kgpack`: one directory per subsystem (`bundle/`, `disc/`, `gui/`, `install/`, `pack/`, `player/`, `session/`, ...) |
| `boot/` | the static bootstrap every kretro file and player starts from |
| `scripts/` | the Python linker and its BLAKE3 helper, and `check-generic.sh` |
| `runtime/` | the toolchain and runtime Dockerfiles, and the scripts that assemble the runtimes |
| `tests/unit/` | one C++ test program per subsystem |
| `tests/integration/` | shell tests against real builds, runtimes and discs |
| `tests/fixtures/` | stand-ins and generators the tests build from |
| `tests/local.env`, `tests/local/` | your own integration test settings and expectations; gitignored |
| `mk/` | the pieces of the Makefile |
| `licenses/` | the licences of every third-party component, and where its source is |
| `third_party/` | vendored BLAKE3, Dear ImGui and stb_image, in their upstream style |

## Commits

- One logical change per commit, with the tests passing at each.
- The subject is a short lowercase sentence saying what is now true, not what
  was done: "a player keeps its state only where it is the owner", "make
  test-runtime looks in the same BUILD make runtime wrote to". No full stop.
- The body, when there is one, says why.
- A refactor changes no behaviour.

## Licence

kretrogame's own code is under the MIT licence in `LICENSE`. By contributing
you agree your contribution is under it too. Third-party code keeps its own
licence; add the licence text to `licenses/` with anything new you vendor or
ship.
