# File formats

This is the specification of every format kretrogame writes to disk. Each
copy of a format's constants in the code points here: `boot/boot.h`,
`src/bundle/toc.h`, `src/pack/header.h`, `scripts/kretro-link.py` and
`tests/fixtures/boot/mkv4.py`. When the code and this page disagree, one of
them is wrong. Change the code only together with this page and with the
golden tests listed under [Cross-checks](#cross-checks).

All integers are little-endian and unsigned unless the text says otherwise.
"BLAKE3" means the 32-byte default BLAKE3 digest. Offsets are in bytes.

- [kretro and player files (v4)](#kretro-and-player-files-v4)
- [Legacy v2 and v3 trailers](#legacy-v2-and-v3-trailers)
- [bundle.meta](#bundlemeta)
- [kgpack](#kgpack)
- [Remembered bundles](#remembered-bundles)
- [The CBOR subset and decoder limits](#the-cbor-subset-and-decoder-limits)
- [State, saves and cache layout](#state-saves-and-cache-layout)
- [Cross-checks](#cross-checks)

## kretro and player files (v4)

kretro, the player base and every player are one ELF file with payloads
appended to it:

```
[bootstrap ELF][payload][payload]...[table of contents][trailer]
```

The bootstrap is an ordinary static executable, so the kernel runs the file
as it is. Everything after it is data that the bootstrap and the app find
through the trailer.

A **player** is the player base (bootstrap, tools, runtime, app) copied up to
the end of its last payload, followed by `bundle.meta` and one kgpack per game,
then a new table and trailer. `bundle::build_bundle` writes it. kretro and the
player base are written by `scripts/kretro-link.py`, and `bundle::link_file`
writes the same bytes for the tests.

### Trailer

The last 64 bytes of the file.

| Offset | Size | Field | Value |
|---:|---:|---|---|
| 0 | 8 | magic | `KRETROv4` |
| 8 | 4 | version | 4 |
| 12 | 4 | reserved | written 0, not read |
| 16 | 8 | toc_off | where the table starts, from the start of the file |
| 24 | 8 | toc_len | the table's length in bytes |
| 32 | 32 | toc_hash | BLAKE3 of the table's `toc_len` bytes |

A reader looks for a v4 trailer first. Only when the last 64 bytes do not
start with `KRETROv4` does it read the last 176 bytes as a
[v2 or v3 trailer](#legacy-v2-and-v3-trailers).

A trailer with the v4 magic and any version other than 4 is refused. The
bootstrap says the file was made by a newer kretro.

### Table of contents

A 16-byte header followed by one 128-byte record per payload, in the order
the writer chose. Writers list payloads in file order.

Header:

| Offset | Size | Field | Value |
|---:|---:|---|---|
| 0 | 4 | magic | `KTOC` |
| 4 | 4 | version | 1 |
| 8 | 4 | count | number of records |
| 12 | 4 | reserved | written 0, not read |

Record:

| Offset | Size | Field | Value |
|---:|---:|---|---|
| 0 | 4 | kind | see [Kinds](#kinds) |
| 4 | 4 | flags | written 0, not interpreted |
| 8 | 8 | off | where the payload starts, from the start of the file |
| 16 | 8 | len | the payload's length |
| 24 | 32 | blake3 | BLAKE3 of the payload's `len` bytes |
| 56 | 64 | name | UTF-8, NUL-padded: the game id for a pack, empty otherwise |
| 120 | 8 | reserved | written 0, not read |

`toc_len` is always `16 + 128 * count`.

### Kinds

| Kind | Name | What it is | How many |
|---:|---|---|---|
| 1 | tools | the static `dwarfs-universal` | one |
| 2 | runtime | the runtime, a DwarFS image mounted in place | one |
| 3 | app | the program the runtime's loader runs: kretro, or the player | one |
| 4 | meta | [`bundle.meta`](#bundlemeta). Its presence makes the file a player. | at most one |
| 5 | pack | a [kgpack](#kgpack), byte for byte as it was on the author's shelf | any number, each with a distinct name |
| 6 | player base | a whole player base file, carried inside kretro | at most one |

kretro carries kinds 1, 2, 3 and, when one was linked, 6. A player base
carries 1, 2 and 3. A player carries 1, 2, 3, 4 and one 5 per game, and never
6.

The offsets inside a carried player base (kind 6) are relative to that
payload's own first byte, so the embedded base reads exactly as the stand-alone
`build/player-base` does. `bundle::Toc` records this as its `base`.

### Invariants

Writers always produce files with these properties. Readers check them as
described, and refuse a file that breaks one with a message that ends in
"Download it again" (the bootstrap) or a `bundle::FormatError` (the C++
reader).

- **The table immediately precedes the trailer:**
  `toc_off + toc_len == file size - 64`. The bootstrap requires this exactly,
  so a file that lost bytes from its middle or gained bytes at its end is
  refused with both sizes. The C++ reader (`bundle::read_toc`) requires only
  that the table lies before the trailer.
- **The table's hash matches** `toc_hash`. Both readers check this before they
  believe any field of the table.
- **Payloads are page-aligned:** every `off` is a multiple of 4096, so DwarFS
  can mount the runtime and each pack body directly from the file. Writers pad
  with zeros.
- **Payloads lie before the table:** `off <= toc_off` and
  `len <= toc_off - off`. Both readers compute this by subtraction so that the
  sum cannot wrap. The bootstrap also refuses `off == 0`, where the bootstrap
  ELF itself is.
- **Singleton kinds appear once.** Kinds 1, 2, 3, 4 and 6 may each appear at
  most once. Both readers refuse a duplicate.
- **Unknown kinds are bounds-checked and ignored.** A kind above 6 must
  satisfy the alignment and bounds rules, and the C++ reader also includes it
  in the overlap check. Otherwise it is skipped, so a newer builder can add
  payloads that an older player does not need.
- **A meta entry makes the file a player.** The bootstrap uses this to decide
  who chooses the state directory. The app uses it to decide whether it is a
  player.

The C++ reader checks more than the bootstrap does:

- A kind of 0 is refused. The bootstrap ignores it as an unknown kind.
- An empty payload (`len == 0`) is refused.
- No two payloads may overlap.
- A pack's name must be a safe id (`kg::id_is_safe`), and no two packs may
  share a name.
- A name must be valid UTF-8 and padded with NULs only, with no bytes after
  its first NUL.

The bootstrap additionally requires kinds 1, 2 and 3, and says which one is
missing.

### Limits

| Limit | Bootstrap | C++ reader and writer |
|---|---|---|
| Table length | `toc_len <= 16 MiB` (`TOC_MAX`), about 131,000 records | `toc_len <= 16 + 128 * 4096`, checked before anything is allocated |
| Record count | whatever fits in 16 MiB | `count <= 4096` (`kMaxEntries`); `encode_toc` throws above it |
| Name | not read | at most 64 bytes; `encode_toc` throws for a longer one |

The bootstrap's limit is looser because it only has to protect its own
allocation. A player never carries more than 4096 entries, because its builder
cannot write more.

### What is hashed, and who checks it

The table records a BLAKE3 for every payload, and the trailer records the
BLAKE3 of the table. Checking happens at different times:

- **The bootstrap** checks the table's hash, every entry's bounds and the
  singleton rule on every start. It does not hash the payloads: hashing a
  multi-gigabyte player on every start would take a minute.
- **The player** checks the `bundle.meta` entry's hash every time it opens
  its file (`player::Bundle::open`). It checks a pack's hash
  (`player::Player::verify`) the first time a given version of the bundle
  plays that game, and remembers the result in `<state>/verified`
  (see [Player state](#player-state)).
- **The builder** re-reads a finished player from disk before renaming it into
  place. `bundle::verify_bundle` checks every entry's BLAKE3, `bundle.meta`,
  and for each pack its Merkle root, its body hash and that it is the game its
  entry names.
- **Building from the carried player base** checks the kind 6 entry's BLAKE3
  while copying it.
- The runtime and app hashes also name cache directories (see
  [Bootstrap cache](#bootstrap-cache-and-runtime)).

### Environment handed to the app

The bootstrap tells the app about the file through `KRETRO_SELF` (the path
that was run) and, for v4 files only, `KRETRO_TOC=<toc_off>:<toc_len>` in
decimal. The player refuses to start if `KRETRO_TOC` disagrees with the table
it reads itself, because that means the file changed while it was starting.
[docs/architecture.md](architecture.md#the-kretro_-environment) lists the
whole environment contract.

## Legacy v2 and v3 trailers

Files linked before v4 end in a 176-byte trailer with fixed slots and no
table. The bootstrap and `bundle::read_toc` still read them, so existing
binaries keep running. Nothing writes them any more, except the test fixture
`tests/fixtures/boot/mkv3.py`.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | magic: `KRETROv2` or `KRETROv3` |
| 8 | 4 | version: 2 or 3 |
| 12 | 4 | flags, written 0 |
| 16 | 8 | tools off |
| 24 | 8 | tools len |
| 32 | 32 | tools hash |
| 64 | 8 | runtime off |
| 72 | 8 | runtime len |
| 80 | 32 | runtime hash |
| 112 | 8 | app off |
| 120 | 8 | app len |
| 128 | 32 | app hash |
| 160 | 8 | v3 only: carried game off (0 when there is none) |
| 168 | 8 | v3 only: carried game len (0 when there is none) |

The slot hashes are SHA-256, not BLAKE3. They were only ever used to name
cache directories, and the carried game had no hash at all. `bundle::read_toc`
returns the slots as entries of kinds 1, 2 and 3 with `hashed = false`, and a
non-empty v3 game slot as a kind 5 entry with an empty name. The C++ reader
requires the version field to match the magic, and applies the entry checks
with the start of the trailer as the limit and without the alignment rule.
The bootstrap accepts version 2 or 3 under either magic, requires a non-empty
runtime and app, and no longer reads the v3 game slot.

## bundle.meta

The kind 4 payload: what a player says about itself and its games. It is a
CBOR map with text keys, encoded by `bundle::BundleMeta::encode` and decoded by
`bundle::BundleMeta::decode`, using the [CBOR subset](#the-cbor-subset-and-decoder-limits).

Decoding rules:

- `format` is read first and must equal `kMetaFormat`, which is 1. Any other
  value is refused before any other key is read.
- Unknown keys are skipped, so a newer builder can add keys.
- A known key holding the wrong CBOR type is refused with a message that names
  the key and the place, for example `'year' of game 2 (example-game) should
  be a whole number, found text`. It is never read as a default.
- A missing optional key takes the default shown below.
- After decoding, `BundleMeta::validate` runs. It checks that the id is a safe
  directory name, the title is not empty, there is at least one game, each
  game id is safe, at most 64 bytes and unique, every game has a name, every
  `backend` and `display` is a known value, every extra file name is safe, an
  embedded key is not empty, and a key's `view` is empty, `"32"` or `"64"`.

Top-level keys, in the order the encoder writes them:

| Key | Type | Required | Meaning |
|---|---|---|---|
| `format` | uint | yes | 1 |
| `id` | text | yes | bundle id; names the player's state directory, so it stays the same across versions |
| `title` | text | yes | shown in the launcher |
| `version` | text | no | the author's version string |
| `built_at` | text | no | when it was built |
| `kretro_version` | text | no | the building kretro's version; `dev` today (see [building.md](building.md#kretro_version)) |
| `banner` | bytes | no | PNG. Written only when not empty. |
| `icon` | bytes | no | PNG. Written only when not empty. |
| `rights_acknowledged` | bool | no | the author confirmed the right to distribute |
| `licenses` | array of text | no | |
| `games` | array of maps | yes | one per game, in launcher order |

Keys of each game map:

| Key | Type | Required | Default | Meaning |
|---|---|---|---|---|
| `id` | text | yes | | game id; also the name of its kind 5 entry |
| `name` | text | yes | | |
| `year` | uint | no | 0 | must fit in 32 bits |
| `cover` | bytes | no | none | PNG. Written only when not empty. |
| `backend` | text | no | `auto` | `auto`, `dxvk`, `wined3d-vk`, `wined3d-gl` or `cnc-ddraw` |
| `needs_gpu` | bool | no | false | |
| `display` | text | no | `integer` | `integer`, `fit` or `native` |
| `fullscreen` | bool | no | false | |
| `gamepad` | text | no | kretro's own map | a map as `bundle::format_gamepad` writes it. Written only when not empty. |
| `extra_dlls` | array of maps | no | none | each `{name: text, data: bytes}`, both required |
| `key` | map | no | none | written only when the author chose to embed a serial |

Keys of `key`: `value` (text, required), `registry_path` (text),
`registry_value` (text), and `view` (text, `"32"` or `"64"`, written only when
set). An empty `view` means the registry path is used exactly as written.

The golden tests in `test_bundle` pin the encoder's exact bytes, with every
optional key and with none.

## kgpack

One installed game, a recipe for rebuilding one, or a save export. The same
envelope is used in `<state>/games/<id>.kgpack`, `<id>.recipe.kgpack`, save
exports, and byte for byte as a kind 5 payload inside a player.

```
[96-byte header][zstd-compressed metadata][zero padding][body]
```

Every offset in the header is relative to the pack's own first byte, so a pack
reads the same on its own and inside a player (`kg::Pack::open` takes the
pack's range inside a larger file).

### Header

`kg::Header::serialize` and `kg::Header::parse`.

| Offset | Size | Field | Value |
|---:|---:|---|---|
| 0 | 7 | magic | `KGPACK\0` |
| 7 | 1 | revision | container revision: written as 2, read if 1 or 2 |
| 8 | 2 | format_version | written 1 (`kPackFormatVersion`), not checked |
| 10 | 2 | flags | bit 0 has body, bit 1 signed, bit 2 body is squashfs (clear: DwarFS) |
| 12 | 2 | kind | `PackKind`: 1 game, 2 runtime, 3 save export |
| 14 | 2 | reserved | written 0 |
| 16 | 8 | meta_off | written 96 |
| 24 | 8 | meta_len | length of the compressed metadata |
| 32 | 8 | body_off | 0 without a body |
| 40 | 8 | body_len | 0 without a body |
| 48 | 8 | sig_off | written 0 |
| 56 | 8 | sig_len | written 0 |
| 64 | 32 | blake3_root | the Merkle root of the game tree (see [Merkle root](#merkle-root)) |

Revisions:

- **1, flat:** the DwarFS body's root is the game tree.
- **2, rooted:** the body's root holds `game/`, `system/`, `discs/<n>/` and
  `registry.reg` (see [Body](#body)).

This build writes revision 2 for every pack. The metadata's `layout` key, not
the revision, says which layout a body has. A missing `layout` means `flat`,
which is true of every revision 1 pack.

`Header::parse` refuses a header that is shorter than 96 bytes, has the wrong
magic, a revision outside 1 to 2, a kind outside 1 to 3, a `meta_len` over
64 MiB, a `body_len` over 1 TiB, or a has-body flag that disagrees with
`body_len` being non-zero. `Pack::open` then requires the metadata and the body
to lie inside the pack's own length, checked by subtraction.

Nothing in this build writes a signature. No pack has the signed flag, and
`sig_off` and `sig_len` are zero. The repacker refuses a signed pack, and also
a squashfs body.

### Metadata

`kg::Meta::encode` produces a CBOR map, and `write_pack` compresses it with
zstd at level 19 into a single frame that records its content size. The
reader refuses a frame without a content size, or one that declares more than
64 MiB decompressed.

The map has 16 keys, written in this order:

| Key | Type | Contents |
|---|---|---|
| `id` | text | game id |
| `name` | text | |
| `year` | uint | |
| `layout` | text | `flat` or `rooted` |
| `who` | map | `developer` text, `publisher` text |
| `recipe` | map | `method` text (`unzip`, `copy`, `wine_setup` or `installer_exe`), `member` text, `subdir` text, `setup` text, `setup_ref` text, `discs` array of text, `verify` array of text, `fingerprints` array of fingerprint maps |
| `run` | map | `exe` text, `args` text, `windows_version` text, `width` uint, `height` uint |
| `runtime` | map | `id` text, `blake3` bytes(32), `dlloverrides` text, `winetricks` array of text, `dgvoodoo` bool |
| `present` | map | `dar` text (default `4:3`), `pause_on_blur` bool (default true) |
| `install` | map | `install_dir` text: where under `C:` the installer put the game |
| `registry` | map | `fragment` text: the REGEDIT4 text the installer created |
| `system` | map | `files` uint, `bytes` uint: what is under `system/` in the body |
| `input` | map | text to text: gamepad bindings over the default map |
| `discs2` | array of maps | `label` text, `serial` uint, `ref` text (`archive#LABEL`), `source` text (absolute path it was opened from), `embedded` bool (its tree is in the body at `discs/<n>/`) |
| `body` | map | `length` uint, `blake3` bytes(32), and `packing` text only when not empty |
| `tree` | text | the tree's canonical text (see [Merkle root](#merkle-root)) |

A fingerprint map has `filename` text, `size` uint, `blake3` bytes(32),
`volume_id` text, `created` text and `anchors`, an array of
`{path: text, size: uint, blake3: bytes(32)}`. The fingerprint's `blake3` is
of the whole image, or of its first `iso::kPrefixBytes` when only that was
read.

The disc list is called `discs2` because the recipe map already has a
`discs` key.

`body.packing` is `kBodyPacking`, `categorize,S22`, for a body made the way
install packs one now, which is `kg::mkdwarfs_body`:

```
<tool> --tool=mkdwarfs -i <in> -o <out> --categorize -S 22 --log-level=error --no-progress -f
```

The string and the flags are a pair and change together. An empty `packing`
means the body was packed earlier with mkdwarfs's defaults. The Bundles page
offers to repack such a body. The packing never affects the Merkle root.

**Lenient decoding.** `Meta::decode` is deliberately forgiving, because a
pack written by a newer kretro must open in an older one, and packs written
before a key existed must still open:

- An absent key leaves the default.
- A key of the wrong type takes the accessor's fallback (`text_or`,
  `uint_or`, `bool_or`), not an error.
- A hash that is not 32 bytes decodes as all zeros.
- Non-text items in a string array are skipped.
- An `input` key that is not text decodes as `""`. A golden test in
  `test_pack` pins this.
- Unknown keys are ignored. Older packs may carry a `filter` key under
  `present` and more fields under `install`, which are ignored.

Only three things are refused:

- metadata that is not a map;
- an `id` that is not a safe id (`kg::id_is_safe`);
- an `install_dir` that is not a relative path under `C:`
  (`kg::install_dir_is_safe`).

A `tree` text that does not parse throws from `Tree::from_canonical`.

### Body

A DwarFS image (or squashfs with flag bit 2), starting at
`align_up(96 + meta_len, 4096)` so that it can be mounted in place. It stays
aligned inside a player because the pack itself starts on a page there.

A rooted body holds:

| Path | Contents |
|---|---|
| `game/` | the game directory; the Merkle root covers exactly this |
| `system/` | files the installer wrote outside the game directory, relative to `drive_c`; present only when there were any |
| `discs/<n>/` | disc *n*, numbered from 1, with `.windows-label` and `.windows-serial`, and its CD audio under `audio/` |
| `registry.reg` | the same text as `registry.fragment`; present only when not empty |

`body.blake3` is the BLAKE3 of the whole body, and `body.length` its size.
`Pack::verify` checks the root against the tree and the body against these
two.

### Merkle root

`Tree::canonical` writes one line per entry, sorted by path:

```
<mode:08x> <size:016x> <hash:64 hex> <path>\n
```

`mode` is the `st_mode`. For a regular file `size` is its length and `hash`
the BLAKE3 of its contents. For a symlink they are the length and BLAKE3 of
its target string. For a directory they are 0 and the BLAKE3 of the empty
string. Paths are relative, `/`-separated, and have no leading `./`. The root
(`Tree::root`, and the header's `blake3_root`) is the BLAKE3 of that text.

The root covers the game tree only: all of a flat body, `game/` of a rooted
one. So a capsule and a recipe of the same install, or a pack with its discs
and one without them, have the same root.

### Kinds of pack

| Use | PackKind | Body |
|---|---|---|
| installed game (capsule) | 1 game | yes |
| recipe | 1 game | no: identity, fingerprints and root only |
| save export | 3 save export | the saves |
| pinned runtime capsule | 2 runtime | yes |

## Remembered bundles

The Bundles page remembers each bundle as `<state>/bundles/<id>.cbor`, one
file per bundle, written to `<id>.cbor.new` and renamed into place. It is a
CBOR map (`bundle::encode_draft`), format `kDraftFormat`, which is 1. The key
order, and which keys are left out when empty, are part of the format. A
golden test in `test_builder` pins both.

Top-level keys, in order: `format` uint, `id` text, `id_typed` bool,
`published` bool, `title` text, `version` text (default `1.0`), `banner` bytes
(only when not empty), `icon` bytes (only when not empty), `banner_from` text,
`icon_from` text, `games` array, `rights` bool, `acknowledged` array of text,
`out_dir` text, `last_built` text, `last_size` uint, `last_built_at` text.

Keys of each game, in order: `id`, `name` text, `year` uint, `cover` bytes
(only when not empty), `cover_from`, `backend` (default `auto`), `needs_gpu`
bool, `display` (default `integer`), `fullscreen` bool, `gamepad` text,
`extra_dlls` array of `{name, data}`, `embed_key` bool, `key_path` text,
`key_value` text. The key itself is never stored here. It is read from the
keys file at build time.

Decoding (`bundle::decode_draft`) refuses a `format` other than 1 and a known
key of the wrong type, and skips unknown keys. A `year` above 32 bits reads as
0.

## The CBOR subset and decoder limits

`kg::cbor` implements the part of RFC 8949 these formats use:

- unsigned and negative integers, byte strings, text strings, arrays and maps
  of definite length, `false`, `true` and `null`;
- no tags, no floating point, no indefinite lengths, no other simple values;
- the encoder writes the shortest head for every length and value.

The decoder treats its input as hostile. It refuses:

- nesting deeper than 32 (`Limits::max_depth`);
- more than `1 << 20` items in one document (`Limits::max_items`), counted
  before anything is reserved for an array or map;
- a length longer than the remaining input;
- trailing bytes after the one value.

Text is not checked for UTF-8. Map keys may be of any type; lookups match text
keys only.

Other limits on the same inputs:

| What | Limit | Where |
|---|---|---|
| kgpack metadata, compressed and decompressed | 64 MiB | `kMaxMetaLen` |
| kgpack body | 1 TiB | `kMaxBodyLen` in `header.cpp` |
| table of contents entries | 4096 | `bundle::kMaxEntries` |

## State, saves and cache layout

`kg::state_dir` is resolved once per process: `KRETRO_STATE`, else
`$XDG_DATA_HOME/kretro`, else `~/.local/share/kretro`, else `/tmp/kretro`.
kretro keeps its state by kind of thing. A player keeps it by game, once
`kg::use_bundle_layout` has been called.

### kretro

| Path | Contents |
|---|---|
| `<state>/config.toml` | settings |
| `<state>/games/<id>.kgpack` | installed games |
| `<state>/runtimes/` | pinned runtime capsules |
| `<state>/saves/<id>/` | the game's saves (see [Saves](#saves)), and its mount points `image/` and `merged/` |
| `<state>/prefixes/<id>/` | Wine prefixes; disposable |
| `<state>/home/<id>/` | the game's own `HOME` |
| `<state>/extracted/<id>/` | the game unpacked, where FUSE is unavailable |
| `<state>/gl/` | links to host driver libraries |
| `<state>/cache/` | scratch: `repack/`, `bundle-exe/`, `stage-probe/`, the GStreamer registry |
| `<state>/bundles/<id>.cbor` | [remembered bundles](#remembered-bundles) |
| `<state>/keys.txt` | serials stored with `kretro key` |
| `<state>/manifests/` | local manifests |
| `<state>/discs.txt` | local known-disc list |
| `<state>/iso/` | the default disc image folder |
| `<state>/compare/<id>/` | `kretro compare` output |

### Player state

`<state>` is `<exe-dir>/<exe-name>-data/` when that exists, is writable, holds
symbolic links and belongs to the user. Otherwise it is
`$XDG_DATA_HOME/<bundle-id>/` when that variable is an absolute path, else
`~/.local/share/<bundle-id>/`, else `/tmp/kretro-<uid>/<bundle-id>/` under a
private directory that must belong to the user (`player::choose_state`).

| Path | Contents |
|---|---|
| `<state>/launcher.toml` | window state, first-run choices, warnings already shown |
| `<state>/verified` | one line per pack already hashed: `<game> "<version>" <blake3 hex>`, at most 256 lines |
| `<state>/report.txt` | the launcher's saved doctor report |
| `<state>/exports/` | save exports made from the launcher |
| `<state>/cache/` | scratch |
| `<state>/<id>/settings.toml` | the game's display and controller settings |
| `<state>/<id>/saves/` | see [Saves](#saves) |
| `<state>/<id>/prefix/` | the game's Wine prefix |
| `<state>/<id>/home/` | the game's `HOME` |
| `<state>/<id>/unpacked-at` | where `--extract-to` put the game, one line |

The mount points and the unpacked copy stay out of the state, because the
state may be on a USB stick:

| Path | Contents |
|---|---|
| `$XDG_RUNTIME_DIR/kretro/<bundle>-<key>/<id>/{image,merged}` | mount points |
| `$XDG_CACHE_HOME/kretro/<bundle>-<key>/<id>/` | the unpacked game, with `<dir>.stamp` beside it |

`<key>` is `kg::state_key(<state>)`: the first 8 hex digits of the FNV-1a hash
of the normalised state path.

### Saves

Under the game's saves directory (`kg::game_saves_dir`):

| Path | Contents |
|---|---|
| `live/` | the writable layer over the pack |
| `work/` | fuse-overlayfs's work directory; must share a filesystem with `live/` |
| `gen/NNNN/` | snapshots, numbered from `0001` |
| `journal/<started>.json` | one per session; `<started>` is the Unix time, zero-padded to 10 digits |
| `journal/` frames | `last.png`, `title.png` and `title.provisional` |
| `lock` | held while a copy of kretro or the player plays the game |
| `restoring`, `restoring.old` | transient, during a restore |

A journal record is JSON with the fields `started`, `ended` and `seconds`
(integers), `runtime` (text), `files_written` and `status` (integers), and
`generation`, `screenshot` and `note` (text). `screenshot` is relative to the
journal directory.

### Bootstrap cache and runtime

`<rtdir>` is `$XDG_RUNTIME_DIR`, else a private `/tmp/.kretro-<uid>` that the
bootstrap creates and checks is the user's own. `<cache>` is
`$XDG_CACHE_HOME`, else `~/.cache`, else `<rtdir>`. `<key>` is the first
8 bytes, in hex, of the payload's hash from the table (for v2 and v3, the
trailer's SHA-256).

| Path | Contents |
|---|---|
| `<rtdir>/kretro/rt-<runtime key>-<runtime off, hex>/` | the runtime's mount point, or its unpacked copy when the cache is noexec |
| `<cache>/kretro/rt-<runtime key>-<runtime off, hex>/` | the runtime unpacked, where FUSE is unavailable |
| `.../rt-.../.kretro-unpacked` | written last into an unpacked runtime; one without it is incomplete |
| `rt-...<pid>.partial`, `.image`, `.stale` | an unpack in progress, removed once its process is gone |
| `<rtdir or cache>/kretro/app-<app key>/kretro` or `/player` | the app, unpacked from the file |
| `<cache or rtdir>/kretro/tools-<tools key>/dwarfs-universal` | the DwarFS tool, when it cannot run from memory |

The runtime root must contain `lib/ld-linux-x86-64.so.2`. That is the only
path the bootstrap relies on.

## Cross-checks

Several writers and readers of the same bytes are held to each other by
tests:

- `test_bundle`'s section "scripts/kretro-link.py writes what link_file
  writes" links a player base with the Python linker and with
  `bundle::link_file` and requires identical bytes. It needs `python3` and
  `build/kretro-b3` (or `KRETRO_B3`), and says it was skipped otherwise.
- `tests/integration/test_boot.sh` runs the real bootstrap against files
  written by `tests/fixtures/boot/mkv4.py`, a writer that is deliberately
  independent of kretro's code, so the bootstrap is checked against this
  specification rather than against the linker written beside it. mkv4.py can
  also write files no real linker would, which the damage checks need.
  `tests/fixtures/boot/mkv3.py` is the v3 linker as it shipped, and proves
  that v3 and v2 binaries still run.
- The golden tests pin the encoders' exact bytes: `test_pack` ("golden: the
  pack header", "golden: pack metadata, with and without body.packing",
  "golden: an input key that is not text decodes as \"\""), `test_bundle`
  ("golden: bundle.meta, with every optional part and with none", "golden:
  the table of contents and the trailer") and `test_builder` ("golden: a
  remembered bundle, with every optional key and with none").
