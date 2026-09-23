# Working on kretrogame

Rules for Claude and any other agent working in this repository. They override
default behaviour.

## Git: the user commits, agents never do

- **Never run `git commit`**, and never do anything else that creates or
  rewrites commits or refs: `git commit --amend`, `rebase`, `reset`, `stash`,
  `merge`, `cherry-pick`, `tag`, `push`, `filter-repo`, branch deletion.
- Leave every change as an uncommitted modification in the working tree. The
  user reviews and commits it.
- `git mv` and `git rm` are fine for moves and deletions. Read-only commands
  (`status`, `diff`, `log`, `show`, `blame`) are always fine.
- Do not create git worktrees.

## No game information in the repository

The game collection used for testing belongs to the user and stays on their
machine. Nothing tracked by git may name or describe it:

- no disc images, installers, archives, installed game trees, packs or built
  players (`.gitignore` covers these);
- no game manifests (`games/`), disc fingerprint lists, collection listings,
  serials, or install paths taken from real games;
- no game titles, game ids, disc labels or image file names in source code,
  comments, tests, fixtures, docs or the README. Use neutral stand-ins
  (`example-game`, `DEMO_DISC`, `game.exe`).

Integration tests that need real games read them from local, gitignored
configuration (see `tests/README.md`) and skip cleanly when it is absent.

## Building and testing

- Nothing compiles on the host. Use the Makefile, which runs the toolchains in
  Docker (`kretro-guibuilder` for C++, `kretro-builder` for the static musl
  bootstrap). `make help` lists the targets.
- `make test` runs the unit and bootstrap tests and must stay green.
- `make runtime` rebuilds the runtime images and takes a long time. Only run it
  when `runtime/` changes.
- **Never use `/tmp` for scratch space.** It has a quota, and filling it breaks
  every shell command. Use `export TMPDIR=$PWD/build/scratch/tmp` and keep
  scratch files under `build/scratch/`.
- Unmount every FUSE mount you create (`fusermount3 -u`). Never touch mounts you
  did not create.

## Style

- C++20. Format new and changed code with `.clang-format`. The existing tree is
  not reformatted wholesale: the style keeps some deliberate multi-statement
  lines that clang-format would break up. Comments explain *why*, in plain
  prose.
- No behaviour change inside a refactor. Keep the test counts the same or
  higher.
