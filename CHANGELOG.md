# Changelog

All notable changes to kretrogame are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- **Players.** kretro is now the builder; what it builds and what gets
  distributed is a *player*: one Linux executable carrying a slim runtime and
  one or more installed games, with a launcher and nothing else. It plays,
  keeps snapshots of saves, exports and imports them, and remembers display
  and gamepad settings per game.
- A **Bundles** page on kretro's shelf that builds a player: identity, games,
  per-game settings, a check list, the size with warnings at 2 GiB and 4 GiB,
  a rights acknowledgement, the build, and a preview run.
- `kretro bundle build`, `bundle list` and `bundle rebuild`, to build players
  without a window.
- File format v4: a fixed table of contents and trailer, `bundle.meta`
  describing the bundle, and packs opened where they sit inside the file.
  The table records a BLAKE3 for every payload; the bootstrap checks the
  table's own hash on every start, and the player verifies each game's pack
  the first time it plays it. v2 and v3 binaries still run.
- `build/player-base`, linked by `make` and carried inside kretro, so building
  a player needs no network and no source tree.
- A player runtime cut from the same image as kretro's, without the authoring
  tools; both run Wine 11 in new WoW64.
- A graphics and backend policy table, NVIDIA host library matching, and
  `--doctor` diagnostics shared by kretro and the player.
- Player state beside the executable when that directory is writable, else
  under XDG, keyed by bundle id; per-game locks; `--extract-to` for machines
  without FUSE; an optional desktop menu entry.
- `make format`, `make format-check` and `make lint` (clang-format and
  clang-tidy in the GUI builder image), `.editorconfig`, a GitHub Actions
  workflow running `make test`, `make lint`, `make format-check` and
  `make programs`, `CONTRIBUTING.md`, and this
  changelog.
- The MIT licence for kretrogame's own code (`LICENSE`); third-party
  components keep their own licences, in `licenses/`.

### Changed

- The bootstrap reads a table of contents, runs DwarFS from memory rather
  than from disk, and runs on a noexec `/tmp` or cache.
- The source tree is laid out for maintenance: build scripts live in
  `scripts/`, the tests are split into `tests/unit/`, `tests/integration/`
  and `tests/fixtures/`, and the Makefile is split into `mk/*.mk`. Every make
  target keeps its name.
- The source layout is refactored: the programs' `main`s are in `src/apps/`,
  kretro's commands in `src/cli/`, the backend policy in `src/backend/`,
  Wine's prefix files in `src/wine/`, the session, install, Bundles page
  logic and GUI are split into a file per concern, the bootstrap into a unit
  per concern, and the unit tests share `tests/unit/support/`. `docs/`
  describes the architecture, the file formats, building and testing. No
  behaviour, message, exit status or on-disk format changes.

### Removed

- `kretro export --standalone`. A one-game player replaces it.
- Every trace of a particular game collection from the repository: the
  manifests in `games/`, the disc list in `db/`, and game names in code,
  comments, tests and docs. kretro reads `games/` and `db/discs.txt` when they
  are present locally and works without them; the integration tests take
  their games from a gitignored `tests/local.env`, and `make check-generic`
  checks that nothing tracked names the local collection.
