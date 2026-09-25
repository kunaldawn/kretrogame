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
- UI scaling in kretro and the player: text and layout follow the window's
  size and the screen's density, Ctrl +, Ctrl - and Ctrl 0 (and Ctrl with the
  mouse wheel) zoom, and `KRETRO_UI_SCALE` sets the scale outright.

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
- Keyboard and gamepad navigation work on every screen of kretro and the
  player: game tiles and the installer's stage take the focus, the arrows move
  through every column, list and step without Enter to go in first, each
  screen opens with its main action focused and comes back to where it was
  left, and panels of text scroll with the arrows, PageUp, PageDown, Home and
  End. Escape and the pad's B never act from under a dialog or out of a field
  being typed in; every dialog answers them with its safe choice; B goes back
  in kretro too, but never quits it from the shelf. The amber focus is shown
  only for the keyboard and the pad.
- The game pages of kretro and the player are laid out as a console's
  library shows a game: the game's picture across the top, blurred behind its
  name, then a bar with a large Play (or Install) button, how much it has
  been played, and its other actions, with the sessions, snapshots and pack
  below; the whole page scrolls as one. Every button, dialog and key does
  what it did.
- kretro's shelf is laid out as a console's library: a sidebar of the other
  screens (a click or the pad reaches what the letters do, and the letters
  are unchanged), a row of the games played most lately where you left off,
  and the grid of every game below it, in one region that scrolls. A view
  (all, installed, not installed, blocked) and a sort (the scan's order by
  default, or recent, name, play time) change only how the shelf looks this
  session and are never saved. The typed filter shows in a field over the
  grid with how many games it leaves. Tab and the pad's shoulder buttons move
  between the sidebar, the recent row and the grid; at a narrow width the
  sidebar is a rail of glyphs and keys. Right from a sidebar row with no game
  level with it now goes to the nearest one.
- A player's launcher opens on the game last played as a band across the top
  of the window: its cover blurred behind it and framed at the right, its
  year and name, a large Play and how much it has been played, under the
  bundle's banner, with the settings at the top right. The bundle's games are
  a row of covers under it (a grid past twelve games); the band follows the
  focus along them, cross-fading between games, and the focused game is named
  under the row. Enter on a cover still opens the game's page; Play in the band
  is a quicker way to the same Play. Tab and the pad's shoulder buttons move
  between the band and the row, and the pad's Start opens the settings.
- kretro's wizard and the Bundles editor are laid out as flows: the steps
  as a row of numbered nodes across the top (ticked when done, the Bundles
  editor's open checks and unticked rights marked on theirs), the step under
  it in a column a form reads well at, and a footer with Back and the step's
  own button. Back does what Escape does on that step, the wizard's first
  step calls it Cancel, and a node is a button only where Escape (in the
  wizard) or the old step list (in the editor) already went; each step's
  buttons, labels and dialogs are unchanged. The Bundles editor's Build is
  in its footer with the reason it cannot be pressed beside it, and its
  other steps have "Next". The list of bundles is a grid of cards with the
  bundle's banner or first cover, New bundle first; Open and Build again
  show on a card while it has the focus or the pointer. A file list chosen
  from in the editor now heads the step rather than following it. Tab and the
  pad's shoulder buttons move between the steps, the step and the footer.
  Steps open on their first control that is not a text field (the wizard's
  name step on the name while it is empty).
- kretro's and the player's other pages follow: a one-line trail for a
  header, and the page's title at the head of a column a form or a report
  reads well at. A game's timeline and a player's saves show each snapshot
  as a card on a rail, with the last picture of the game on the newest and
  Restore at the right of each. Settings (kretro's, and a player's display,
  controls and bundle settings) are rows with the name and its help at the
  left and the control at the right, and Left and Right step a choice while
  it has the keyboard or pad focus, as picking it from its list does. The
  Library's discs are rows the arrows walk, Enter on a row doing what its
  Install does; the Import page shows the pack on a card over a large Import
  (or Rebuild) button; the doctor's sections say their verdict at the right
  of their headings. A player's first check and the screen for a machine
  that cannot play are a card over the bundle's banner, blurred. Restore in
  its dialog is in the colour of a button that replaces something. Every
  button, label, dialog and key does what it did.
- The mouse wheel no longer falls behind while the mouse moves, and pages no
  longer read the disk on every frame (the journal, snapshots, settings,
  folder listings and the uninstall sizes).
- Motion is short and the same at any frame rate: a page fades in as it
  comes up (the status bar stays as it is), the focus glides between items,
  a region scrolls smoothly to where the wheel, the keys or the focus send
  it, the recent row and the launcher's covers ease the focused game to the
  row's left edge, and the launcher's band cross-fades between games.
  `KRETRO_REDUCE_MOTION=1` turns all of it off: everything snaps, and the
  blinking cursors are solid.
- On kretro's shelf, Home and End go to the first and the last game, and
  PageUp and PageDown scroll the shelf by most of its height and move the
  focus to the game nearest where it was.
- On a very wide window the launcher's band text, Play and its row of
  covers keep to a centred column, as the shelf's grid does; the band's
  picture still spans the window. On a small, short window (800x480) the
  game pages' band is held to under a third of the height.
- Space no longer starts the shelf's typed filter, since it is also the key
  that opens the focused game; it still goes into a filter already being
  typed.

### Removed

- `kretro export --standalone`. A one-game player replaces it.
- Every trace of a particular game collection from the repository: the
  manifests in `games/`, the disc list in `db/`, and game names in code,
  comments, tests and docs. kretro reads `games/` and `db/discs.txt` when they
  are present locally and works without them; the integration tests take
  their games from a gitignored `tests/local.env`, and `make check-generic`
  checks that nothing tracked names the local collection.

### Fixed

- A windowed game on a Wayland desktop with a fractional scale came up at
  twice the size the screen could show, squeezed by the desktop into its work
  area. The panel is now measured in the units the nested Weston sizes its
  window in (the desktop's logical size, not the scaled-up Xwayland root),
  and a windowed game's whole-number scale fits the work area, the desktop
  less its top bar and dock. Fullscreen still uses the whole panel.
  `kretro display` shows the work area when it is smaller.
- A game started from kretro's shelf was always shown at 1x in a window: the
  shelf never told the session the panel's size. It now measures it as the
  launcher does, and the Settings page's "would run at" line uses the same
  measure.
- Getting a game's Wine prefix ready drew on the real desktop: the "Wine
  configuration is being updated" dialog on a game's first play and during an
  install, and a full-screen "Wine Desktop" that flashed up for a second on
  every launch. Every Wine command run before the game's own compositor
  (wineboot, winecfg, the registry imports and `reg add`s, the CD-ROM drive,
  the player's prefix upgrade and author's key) now runs with no display, and
  the game starts on a fresh wineserver.
