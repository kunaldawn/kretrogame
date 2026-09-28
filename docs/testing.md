# Testing

There are two kinds of test, kept apart by what they need. `make test` needs
no disc, no game, no GPU and no runtime, and it is what CI runs. The scripts
in `tests/integration/` need real builds, runtimes or your own discs, and are
run by hand.

- [Unit tests: make test](#unit-tests-make-test)
- [Writing unit tests](#writing-unit-tests)
- [The boot tests](#the-boot-tests)
- [Fixtures](#fixtures)
- [Integration tests](#integration-tests)
- [Local configuration](#local-configuration)
- [Keeping the repository generic](#keeping-the-repository-generic)
- [Scratch space and FUSE](#scratch-space-and-fuse)

## Unit tests: make test

```sh
make test
```

`tests/unit/` holds one C++ program per subsystem. `make test` builds them
in `kretro-guibuilder` and runs them in the order of `UNIT_TESTS` in
`mk/sources.mk`: `test_pack`, `test_disc`, `test_install`, `test_config`,
`test_wizard`, `test_stage`, `test_bundle`, `test_policy`, `test_player` and
`test_builder`. Then it runs `test_bundles_page`, which draws every step of
the Bundles page into no window, and finally
`tests/integration/test_boot.sh` in `kretro-builder`, which checks the real
bootstrap against stand-ins.

Each `test_*` program prints the heading of each group of checks and any
failure, then ends with the summary line `N checks, M failed`, and exits
non-zero when M is not 0. `test_boot.sh` ends the same way.
`test_bundles_page` ends by saying how many frames it drew, and exits non-zero
on a failure.

M must be 0. The counts must stay the same or grow with every change. A
refactor keeps them exactly the same.

### Baseline

| Suite | Checks |
|---|---:|
| `test_pack` | 247 |
| `test_disc` | 117 |
| `test_install` | 354 |
| `test_config` | 150 |
| `test_wizard` | 370 |
| `test_stage` | 71 |
| `test_bundle` | 322 |
| `test_policy` | 408 |
| `test_player` | 390 |
| `test_builder` | 302 |
| `test_bundles_page` | 52 frames |
| `test_boot.sh` | 79 |

Update this table in the same change as anything that adds checks.

A few checks need the real DwarFS tool, `build/dwarfs-universal` (which `make`
builds) or `KRETRO_DWARFS`: `test_install`'s "a second game from the same disc
joins its set", and `test_builder`'s trimming, player-of-part-of-a-set, pack
read and licence checks. Without it they print a `skip:` line and the counts
are lower; the table is the count with the tool.

## Writing unit tests

Fixtures and unit tests use stand-ins only: `example-game`, `Example Game`,
`DEMO_DISC`, `game.exe`, `Example.zip`. Never use a real title, id, volume
label or file name from anyone's collection.

### The support headers

`tests/unit/support/` holds three header-only files, all in namespace
`kgtest`. A test includes them as `"support/check.h"` and so on.

`check.h` provides the checks:

| Name | What it does |
|---|---|
| `CHECK(cond)` | one check: passes when `cond` is true |
| `CHECK_EQ(a, b)` | one check: passes when `a == b`. On failure it prints both expressions and, when they can be streamed, both values. |
| `CHECK_THROWS(expr)` | one check: passes when `expr` throws a `std::exception` |
| `CHECK_THROWS_WITH(expr, needle)` | one check: passes when `expr` throws and the message contains `needle` |
| `kgtest::section(name)` | prints a group heading |
| `kgtest::record(ok, file, line, what)` | one check that is already decided, for a test's own macros |
| `kgtest::fail(file, line, what)` | a failure that does not count a check |
| `kgtest::unexpected(e)` | an exception that escaped every check: a failure, not a check |
| `kgtest::finish()` | prints `N checks, M failed` and returns the exit status |
| `kgtest::show(v)` | a value as a failure prints it, or `<?>` |

Every macro counts exactly one check, before it evaluates anything, whether it
passes, fails or throws. `make test`'s totals are compared against fixed
numbers, so a check must never count twice or not at all.

`files.h` provides file helpers: `write_file(path, content)`, which creates
the directories and replaces the file; `slurp(path)`, which returns `""` when
the file cannot be read; `contains(s, what)`; `to_hex(bytes)`, which the
golden tests compare encoders' output in; and `flip_byte(path, offset)`, which
damages one byte in place.

`bundle_fixtures.h` provides the fixture packs and player base that
`test_bundle` and `test_player` both use: `pack_meta(tmp, id)`,
`make_pack(tmp, id, body_size)`, which writes a capsule at `tmp/shelf/<id>.kgpack`
whose body is not a real DwarFS image, and `make_base(tmp)`, a player base
built from small files at `tmp/player-base`.

### Adding a suite

1. Write `tests/unit/test_<name>.cpp` with a `main` that calls its groups in
   order and ends with `return kgtest::finish();`.
2. Add `<name>` to `UNIT_TESTS` in `mk/sources.mk`. The rules in
   `mk/rules.mk` link it with the library and BLAKE3, and `make test` runs it
   in list order.
3. Add its count to the [baseline](#baseline).

Tests are called explicitly from `main`. There is no self-registering test
table, so the order is visible and state set up by one group (for example
`KRETRO_STATE`, `HOME` or `XDG_*`, which must be set before `kg::state_dir`
first resolves) cannot be reordered by the linker.

A suite that needs a window links the GUI, as `test_bundles_page` does. It
has its own rule in `mk/rules.mk`, and `make test` runs it inside the image,
because the host need not have SDL.

## The boot tests

`tests/integration/test_boot.sh` runs the real bootstrap against whole files
built around it:

- the real `dwarfs-universal` from `kretro-builder`;
- a runtime image containing only `tests/fixtures/boot/stub_loader.c` as
  `lib/ld-linux-x86-64.so.2`, which checks that it was called the way the real
  loader is and then runs the app;
- `tests/fixtures/boot/stub_app.c` as the app, which prints its arguments and
  every `KRETRO_*` variable, one per line, and runs the DwarFS tool it was
  given;
- files written by `mkv4.py` and `mkv3.py` (see [Fixtures](#fixtures)).

It runs inside `kretro-builder` without `/dev/fuse`, so every run takes the
extraction path. FUSE is exercised by `tests/integration/portability.sh`.

`make test` starts the container with a noexec tmpfs at `/nx`
(`--tmpfs /nx:rw,noexec,mode=1777`). The checks that need a noexec cache or
runtime directory use it, and are skipped rather than failed without it.
`KRETRO_BOOT_NO_MEMFD=1` makes the bootstrap write the DwarFS tool to a file
rather than run it from memory, which is how the file fallback is tested on
kernels that allow executable memfds.

Every run gets fresh XDG directories and a clean environment through `env
-i`, so one run's extraction cannot make the next one pass. `make boot-test`
runs it on its own inside the builder image.

## Fixtures

`tests/fixtures/boot/`:

| File | What it is |
|---|---|
| `mkv4.py` | a v4 file writer for `test_boot.sh`. It prints `toc_off toc_len`. |
| `mkv3.py` | the v3 linker exactly as it shipped before v4, so the test can build the binaries people already have |
| `stub_loader.c`, `stub_app.c` | stand-ins for the runtime's loader and the app |
| `b3sum.c` | BLAKE3 of stdin, because the builder image's Python has no BLAKE3 |

`mkv4.py` is deliberately independent of kretro's own writers. A bootstrap
that is checked only against the linker written beside it agrees with that
linker, which is not the same as agreeing with the format. `mkv4.py` follows
[docs/file-format.md](file-format.md) on its own, and it can also write files
no real linker would, such as an entry past the end of the file or a table
without a runtime (`--raw-entry`, `--trailer-version`), which most of the
damage checks need. The C++ writer and the Python linker are held to each
other separately, by `test_bundle` (see
[file-format.md](file-format.md#cross-checks)).

## Integration tests

Run by hand, against real builds, runtimes and discs:

| Script | What it needs | What it checks |
|---|---|---|
| `scan.sh` | `build/kretro`, your images, a listing of what they hold | `kretro scan` finds the right number of discs in each download |
| `install.sh` | `build/kretro`, `build/kgpack`, one game's manifest and disc | a headless install, and what the pack says about itself |
| `bundle.sh` | the same, for one or more games | a player built from real installs and run as a stranger, and damage to one game sparing the others |
| `stage.sh` | `build/kretro` | headless Weston gives a readable Xwayland root window |
| `runtime.sh` | `make runtime` | what each runtime carries, and that its Wine starts (`make test-runtime`) |
| `portability.sh` | `build/kretro`, Docker | kretro, or a player, on several distributions |
| `test_boot.sh` | nothing extra; run by `make test` | the bootstrap |

The scripts share their helpers through `tests/integration/lib.sh`, which each
one sources:

- sourcing it moves to the top of the repository and sets `ROOT`;
- `load_local_env` reads `tests/local.env` when it exists;
- `ok`, `bad`, `is`, `check`, `has` and `hasnt` count and report checks
  (`QUIET_OK=1` counts passes without printing them, and `CHECK_QUIET=1`
  discards a checked command's output);
- `skip MSG` prints `skip: MSG` and exits 0;
- `scratch NAME` makes `$SCRATCH` under `TMPDIR` and removes it on exit,
  after running the script's own `scratch_cleanup` if it defines one;
- `our_mounts` and `unmount_ours` find and unmount only the FUSE mounts
  under `$SCRATCH`, innermost first;
- `finish` prints `N checks, M failed` and fails when M is not 0.

Each script prints `ok` or `FAIL` per check (`test_boot.sh` prints only its
failures), ends with the same summary line as the unit tests, and exits
non-zero when M is not 0. When a disc, a game or a build it needs is missing,
a script prints a `skip:` line and exits 0 instead.

`KRETRO_BIN` points the scripts at a kretro other than `build/kretro`, and
`KRETRO_B3` points `bundle.sh` and `test_bundle` at a `kretro-b3` other than
`build/kretro-b3`.

## Local configuration

The games the integration tests use are yours, so which ones is never
committed. The scripts read them from `tests/local.env`, which is gitignored:

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
| `KRETRO_TEST_COLLECTION` | scan | the listing scan.sh checks against (default `tests/local/expected-collection.txt`) |
| `KRETRO_PLAYER`, `KRETRO_PLAYER_GAME` | portability | a player to run instead of building a tiny one, and its game |

`tests/local/` is gitignored too, for expectation files like the scan listing:
one line per image or archive, with its file name and how many discs are in
it, separated by a tab.

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

`scripts/check-generic.sh` builds a list of names from your local data:
manifest ids and titles, disc labels and fingerprints, image and archive file
names, and the first column of `tests/local/`. It searches every file git
tracks for them, case-insensitively, and fails with each hit. Upstream
licence texts in `licenses/` and vendored code in `third_party/` are skipped.
Run it before sending a change. CI cannot run it, having no collection to
look for.

## Scratch space and FUSE

- Never use `/tmp` for scratch space. It has a quota, and filling it breaks
  every shell command. Point `TMPDIR` into the build tree instead:
  `export TMPDIR=$PWD/build/scratch/tmp`. `lib.sh`'s `scratch` honours
  `TMPDIR`.
- Unmount every FUSE mount a test leaves behind with `fusermount3 -u` (or
  `fusermount3 -u -z` for one whose daemon has died). Never touch a mount you
  did not create. `unmount_ours` only touches mounts under the script's own
  `$SCRATCH`.
