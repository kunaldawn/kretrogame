#!/usr/bin/env bash
# Tier 2: install a real game off a real disc, then read the pack back.
#
# Everything else we know about install is known from fixtures. This is the one
# test that consults reality: it opens a disc image out of your image folder,
# takes the game tree off it, packs it with its disc, and then asks the pack
# what it carries.
#
# Which game is local configuration, never committed: KRETRO_TEST_INSTALL_GAME,
# or else the first id in KRETRO_TEST_GAMES, usually set in tests/local.env
# (see tests/README.md). It has to be a game whose manifest installs unattended
# - method "copy" or "unzip", one disc named by `iso` - and the cheapest such
# game makes the fastest run. Its disc rides along inside the body, which is
# half of what there is to look at. The images come from KRETRO_ISO_DIR, or
# iso/.
#
# It installs into a scratch KRETRO_STATE and throws it away afterwards, so the
# user's library is never touched and two runs in a row both pass. It wants
# about a gigabyte of scratch space; set TMPDIR to move that somewhere roomier.
set -uo pipefail
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"
load_local_env

BIN="${KRETRO_BIN:-build/kretro}"
PACKER="${KGPACK_BIN:-build/kgpack}"
ISO_DIR="$(realpath -m "${KRETRO_ISO_DIR:-iso}")"
read -r first_game _ <<<"${KRETRO_TEST_GAMES:-}"
ID="${KRETRO_TEST_INSTALL_GAME:-${first_game:-}}"
[ -n "$ID" ] || skip "no game to install - set KRETRO_TEST_GAMES in tests/local.env (see tests/README.md)"
MANIFEST="${KRETRO_MANIFESTS:-games}/$ID.toml"

[ -d "$ISO_DIR" ] || skip "no $ISO_DIR - this one needs your own discs"
[ -x "$BIN" ] || skip "no $BIN - run: make"
[ -x "$PACKER" ] || skip "no $PACKER - run: make app"
[ -f "$MANIFEST" ] || skip "no $MANIFEST"

# Everything the pack is checked against comes out of the manifest, so what is
# being asserted is that the pack says what the manifest said - not that it
# says some constant a person once typed here.
key() { sed -n "s/^$1 *= *\"\(.*\)\"/\1/p" "$MANIFEST" | head -n 1; }
ISO="$(key iso)"
METHOD="$(key method)"
SUBDIR="$(key subdir)"
VERIFY="$(sed -n 's/^verify *= *\[\(.*\)\]/\1/p' "$MANIFEST" | head -n 1 |
          tr ',' '\n' | tr -d ' "' | grep .)"

[ -n "$ISO" ] || skip "$MANIFEST names no iso - pick a copy or unzip game"
[ -f "$ISO_DIR/$ISO" ] || skip "$ISO is not in $ISO_DIR - nothing to install from"
export KRETRO_ISO_DIR="$ISO_DIR"
[ -n "$METHOD" ] && [ -n "$VERIFY" ] ||
  skip "could not read method and verify out of $MANIFEST"

scratch install
WORK="$SCRATCH"
export KRETRO_STATE="$WORK/state"
LOG="$WORK/install.log"

# --- the install itself ----------------------------------------------------
if "$BIN" install "$ID" --headless >"$LOG" 2>&1; then
  ok "$ID installs unattended from $ISO"
else
  bad "$ID did not install (exit $?)"
  sed 's/^/        /' "$LOG" | tail -n 20
  finish
  exit 1
fi

PACK="$KRETRO_STATE/games/$ID.kgpack"
[ -s "$PACK" ] && ok "the pack is where the library keeps it" \
               || { bad "no pack at $PACK"; finish; exit 1; }

# A second install must refuse rather than quietly rebuild over the first.
if "$BIN" install "$ID" --headless >"$WORK/again.log" 2>&1; then
  bad "a second install overwrote the pack instead of refusing"
else
  grep -q -- '--force' "$WORK/again.log" \
    && ok "installing it again refuses, and says --force" \
    || bad "the second install failed without pointing at --force"
fi

# --- what the pack says about itself ---------------------------------------
"$BIN" verify "$ID" >"$WORK/verify.txt" 2>&1 \
  && ok "kretro verify: $(tr -s ' \n' ' ' <"$WORK/verify.txt")" \
  || { bad "kretro verify failed"; sed 's/^/        /' "$WORK/verify.txt"; }

INFO="$WORK/info.txt"
"$PACKER" info "$PACK" >"$INFO" 2>&1 || bad "kgpack info could not read the pack"
field() { sed -n "s/^$1  *//p" "$INFO" | head -n 1; }

case "$(field layout)" in
  rooted*) ok "the body is rooted - game/ beside discs/, not one flat tree" ;;
  *)       bad "layout is '$(field layout)', not rooted" ;;
esac

RECIPE="$(field recipe)"
case "$RECIPE" in
  *"$METHOD"*) ok "the recipe kept the method: $RECIPE" ;;
  *)           bad "the recipe says '$RECIPE', not '$METHOD'" ;;
esac
if [ -n "$SUBDIR" ]; then
  case "$RECIPE" in
    *"$SUBDIR"*) ok "the recipe kept the subdirectory the game lives in ($SUBDIR)" ;;
    *)           bad "the recipe lost subdir '$SUBDIR': $RECIPE" ;;
  esac
fi
is "the verify list came along" \
   "$(field verify | tr -d ' ')" "$(printf '%s' "$VERIFY" | paste -sd,)"

# The disc: label, volume serial, and whether its bytes are in the body.
DISC="$(sed -n 's/^disc 1  *//p' "$INFO" | head -n 1)"
LABEL="$(printf '%s' "$DISC" | sed -n 's/^\(.*\)  serial .*/\1/p')"
SERIAL="$(printf '%s' "$DISC" | sed -n 's/.*serial \([0-9a-f]*\).*/\1/p')"
[ -n "$LABEL" ] && [ -n "$SERIAL" ] \
  && ok "disc 1 is named and numbered: $LABEL, serial $SERIAL" \
  || bad "disc 1 has no label or no volume serial: '$DISC'"
case "$DISC" in
  *carried*) ok "disc 1 is carried in the pack, not merely named" ;;
  *)         bad "disc 1 was not carried: '$DISC'" ;;
esac

# The fingerprint is what a recipe rebuild would match the disc against. An
# empty one would make the pack unrebuildable and nothing else would notice.
FP="$(grep -A1 '^disc 1' "$INFO" | sed -n '2s/^ *//p')"
FPFILE="$(printf '%s' "$FP" | sed -n 's/^\(.*\)  [0-9.]* [KMGT]*i*B  .*/\1/p')"
FPHASH="$(printf '%s' "$FP" | awk '{print $NF}')"
is "the fingerprint names the disc it was taken from" "$FPFILE" "$ISO"
case "$FPHASH" in
  0000000000000000|"") bad "the fingerprint hash is empty: '$FP'" ;;
  *[!0-9a-f]*)         bad "the fingerprint hash is not a hash: '$FPHASH'" ;;
  *)                   ok  "the fingerprint carries a hash of the disc ($FPHASH)" ;;
esac

# --- what is actually inside the body --------------------------------------
# The body is a DwarFS image at an offset inside the pack. The tool that reads
# it is the one the bootstrap unpacked for install's own use.
tool=""
for c in "${KRETRO_DWARFS:-}" \
         "$(ls -1d "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/kretro/tools-*/dwarfs-universal 2>/dev/null | tail -n 1)" \
         "build/dwarfs-universal" "$(command -v dwarfs-universal 2>/dev/null)"; do
  [ -n "$c" ] && [ -x "$c" ] && { tool="$c"; break; }
done
OFF="$(sed -n 's/^body .* at +\([0-9]*\)$/\1/p' "$INFO")"

if [ -z "$tool" ] || [ -z "$OFF" ]; then
  bad "cannot read the body: no dwarfs tool, or no body offset in kgpack info"
else
  "$tool" --tool=dwarfsck -i "$PACK" -O "$OFF" -l >"$WORK/body.txt" 2>"$WORK/body.err" \
    || bad "dwarfsck could not list the body: $(tail -n 1 "$WORK/body.err")"

  grep -q '^game/' "$WORK/body.txt" && ok "the body holds game/" || bad "the body has no game/"
  grep -q '^discs/1/' "$WORK/body.txt" \
    && ok "the body holds discs/1/ - the disc came with the game" \
    || bad "the body has no discs/1/"

  # The tree in the metadata is the game and only the game: it is what verify
  # hashes and what a session diffs against, so it must be the game/ subtree
  # entry for entry, with nothing from discs/ in it and no game/ prefix on it.
  "$PACKER" tree "$PACK" | cut -d' ' -f4- | LC_ALL=C sort >"$WORK/tree.txt"
  grep '^game/' "$WORK/body.txt" | sed 's|^game/||' | LC_ALL=C sort >"$WORK/bodygame.txt"
  N="$(wc -l <"$WORK/tree.txt")"
  D="$(comm -3 "$WORK/tree.txt" "$WORK/bodygame.txt" | wc -l)"
  if [ "$N" -lt 2 ]; then D="an empty tree"; fi
  is "the pack's tree is exactly the body's game/ ($N entries)" "$D" "0"

  # Every name the manifest promised, present where the game was put. Wine is
  # case-insensitive and a real disc is not consistent: GAME.EXE in the
  # manifest may be Game.exe on the disc, so this looks the way the loader looks.
  missing=""
  for v in $VERIFY; do
    grep -qixF "$v" "$WORK/bodygame.txt" || missing="$missing $v"
  done
  [ -z "$missing" ] \
    && ok "every file the manifest verifies is in game/ ($(printf '%s' "$VERIFY" | paste -sd,))" \
    || bad "the verify list names files the body does not have:$missing"

  # The drive metadata is what makes discs/1/ a CD-ROM at play time rather than
  # a folder: without a label and a volume serial the game sees a blank drive.
  rm -rf "$WORK/meta"; mkdir -p "$WORK/meta"
  "$tool" --tool=dwarfsextract -i "$PACK" -O "$OFF" -o "$WORK/meta" \
          --pattern 'discs/1/.windows-*' --log-level=error >/dev/null 2>&1
  is "discs/1/.windows-label is the disc's label" \
     "$(cat "$WORK/meta/discs/1/.windows-label" 2>/dev/null)" "$LABEL"
  is "discs/1/.windows-serial is the volume serial the metadata records" \
     "$(cat "$WORK/meta/discs/1/.windows-serial" 2>/dev/null)" "$SERIAL"
fi

finish
