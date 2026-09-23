# Tests

Two kinds, kept apart by what they need.

## Unit tests: `make test`

```sh
make test
```

`tests/unit/` holds one C++ program per subsystem (`test_pack`, `test_disc`,
`test_install`, `test_wizard`, `test_bundle`, `test_player`, ...), and
`test_bundles_page` draws the Bundles page into no window. `make test` builds
and runs all of them, then runs `tests/integration/test_boot.sh`, which checks
the real bootstrap against stand-ins built from `tests/fixtures/boot/`.

None of it needs a disc, a game, a GPU or the runtime, and it is what CI runs.
Each program prints its checks and ends with `failed 0`; the counts must stay
the same or grow with every change.

Fixtures and unit tests use stand-ins only: `example-game`, `Example Game`,
`DEMO_DISC`, `game.exe`, `Example.zip`. Never a real title, id, volume label or
file name from anyone's collection.

## Integration tests: `tests/integration/*.sh`

Run by hand, against real builds, runtimes and discs:

| Script | What it needs | What it checks |
|---|---|---|
| `scan.sh` | `build/kretro`, your images, a listing of what they hold | `kretro scan` finds the right number of discs in each download |
| `install.sh` | `build/kretro`, `build/kgpack`, one game's manifest and disc | a headless install, and what the pack says about itself |
| `bundle.sh` | the same, for one or more games | a player built from real installs and run as a stranger, and damage to one game sparing the others |
| `stage.sh` | `build/kretro` | headless Weston gives a readable Xwayland root window |
| `runtime.sh` | `make runtime` | what each runtime carries, and that its Wine starts (`make test-runtime`) |
| `portability.sh` | `build/kretro`, docker | kretro, or a player, on several distributions |
| `test_boot.sh` | nothing extra; run by `make test` | the bootstrap |

Each script ends with `failed N`, and exits cleanly with a `skip:` line when
something it needs is missing.

## Local configuration

The games the integration tests use are yours, so which ones is never
committed. The scripts read it from `tests/local.env`, which is gitignored:

```sh
cp tests/local.env.example tests/local.env
$EDITOR tests/local.env
```

| Variable | Used by | Meaning |
|---|---|---|
| `KRETRO_ISO_DIR` | scan, install, bundle | where the disc images and archives are (default `iso/`) |
| `KRETRO_MANIFESTS` | install, bundle | where the manifests are (default `games/`) |
| `KRETRO_TEST_GAMES` | install, bundle | ids of games that install unattended (`copy` or `unzip`, disc named by `iso`) |
| `KRETRO_TEST_INSTALL_GAME` | install | install.sh's game, if not the first of `KRETRO_TEST_GAMES` |
| `KRETRO_TEST_NO_DISCS` | bundle | ids packed without their disc |
| `KRETRO_TEST_COLLECTION` | scan | the listing scan.sh checks against (default `tests/local/expected-collection.txt`) |
| `KRETRO_PLAYER`, `KRETRO_PLAYER_GAME` | portability | a player to run instead of building a tiny one, and its game |

`tests/local/` is gitignored too, for expectation files like the scan listing:
one line per image or archive, its file name and how many discs are in it,
separated by a tab.

```
Example.zip	2
Another Game (USA).iso	1
```

Manifests (`games/*.toml`) and the known-disc list (`db/discs.txt`) are local
data in the same way. kretro reads them when they are there and works without
them.

## Keeping the repository generic

```sh
make check-generic
```

`scripts/check-generic.sh` builds a list of names from your local data -
manifest ids and titles, disc labels and fingerprints, image and archive file
names, the first column of `tests/local/` - and searches every file git tracks
for them, case-insensitively. It fails with each hit. Run it before sending a
change; CI cannot, having no collection to look for.

## Scratch space

Point `TMPDIR` into the build tree (`export TMPDIR=$PWD/build/scratch/tmp`)
rather than `/tmp`, and unmount any FUSE mount a test leaves behind with
`fusermount3 -u`.
