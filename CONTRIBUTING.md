# Contributing to kretrogame

## Building

Nothing is compiled on the host: the Makefile runs every compiler inside
Docker images it builds from `runtime/`. You need Docker, GNU make and
Python 3.

```sh
make runtime   # once, and again only when runtime/ changes; slow
make           # build/kretro, with build/player-base inside it
make help      # every target
```

[docs/building.md](docs/building.md) describes the images, the targets, the
variables and the development loops.

## Testing

```sh
make test          # unit tests, the Bundles page, the bootstrap
make test-runtime  # both runtimes: what they carry, and that Wine starts
```

`make test` needs neither game media nor the runtime, and is what CI runs. It
must stay green, with the same number of checks or more, on every change.
[docs/testing.md](docs/testing.md) describes the unit tests and their support
headers, the boot tests, the integration scripts and their local
configuration, and the scratch-space and FUSE rules.

Game media and anything describing a game collection are never committed:
no manifests, disc lists, image names, titles or ids in code, comments, tests
or docs. Unit tests use stand-ins (`example-game`, `DEMO_DISC`, `game.exe`).
`make check-generic` searches every tracked file for the names in your own
local data (`games/`, `db/`, `iso/`, `tests/local/`) and fails on any hit; run
it before sending a change. CI cannot, having no collection.

## Formatting and linting

```sh
make format            # rewrite the files in FORMAT_CLEAN to .clang-format
make format-check      # fail if any of them differs; CI runs this
make format-check-all  # list every place in src/, boot/ and tests/ out of style
make lint              # clang-tidy over all of src/ with .clang-tidy; must pass
```

`.clang-format` follows the existing style as closely as clang-format can.
The older code deliberately puts short related statements on one line, which
clang-format cannot keep, so the tree as a whole is not formatted by it. The
rule is:

- A new file, or one split out as new code, is formatted in full and added to
  `FORMAT_CLEAN` in `mk/sources.mk`, which `make format-check` holds to it.
  Where clang-format would break up a deliberate multi-statement line, wrap
  that block in `// clang-format off` and `// clang-format on`, with a
  one-line reason.
- An edit to any other file is formatted on the lines it changes only:
  `git clang-format` does that for the uncommitted changes. The
  `kretro-guibuilder` image has the script but not git, so run it on the host
  with clang-format 21, or match the surrounding style by hand. Code moved
  verbatim from one file to another is not reformatted.

`make format-all` rewrites the whole tree and is not for routine use.
`.clang-tidy` explains
each check it turns off. `make lint` covers every `.cpp` file under `src/`,
the GUI and the three programs included, and must pass on every change.

`.editorconfig` covers whitespace for every file type: two spaces for C, C++
and shell, four for Python, tabs in makefiles.

## Where things live

| Path | What |
|---|---|
| `src/apps/` | the `main` of kretro, the player and `kgpack` |
| `src/` | one directory per module, from `util/` up to `gui/` and `cli/`; [docs/architecture.md](docs/architecture.md#module-map) has the map and the rule for which may include which |
| `src/bundle/builder/` | the Bundles page's decisions, without the page |
| `src/gui/` | both windows: shared pieces at the top, kretro's `shelf/`, `wizard/`, `bundles/` and `stage/`, and the player's `launcher/` |
| `boot/` | the static bootstrap every kretro file and player starts from: `main.c` and one unit per concern, sharing `boot.h` |
| `scripts/` | the Python linker and its BLAKE3 helper, and `check-generic.sh` |
| `runtime/` | the toolchain and runtime Dockerfiles, and the scripts that assemble the runtimes |
| `docs/` | architecture, the file formats, building and testing |
| `tests/unit/` | one C++ test program per subsystem |
| `tests/unit/support/` | the checks and helpers the unit tests share |
| `tests/integration/` | shell tests against real builds, runtimes and discs |
| `tests/fixtures/` | stand-ins and generators the tests build from |
| `tests/local.env`, `tests/local/` | your own integration test settings and expectations; gitignored |
| `mk/` | the pieces of the Makefile |
| `licenses/` | the licences of every third-party component, and where its source is |
| `third_party/` | vendored BLAKE3, Dear ImGui and stb_image, in their upstream style |

A format change is a change to [docs/file-format.md](docs/file-format.md) in
the same commit, with its golden tests updated.

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
