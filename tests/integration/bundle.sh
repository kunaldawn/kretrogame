#!/usr/bin/env bash
# Tier 2: build a real player out of real installs, and run it as a stranger.
#
# tests/integration/install.sh proves one pack comes off one disc. This goes the
# rest of the way to what gets shipped: the games in KRETRO_TEST_GAMES - the
# first always, each other one when its disc is here - installed headless into
# a scratch kretro, a player built from them with `kretro bundle build`, the
# player's table of contents read back and each pack in it held against the
# shelf it came off, and then the player run
# under a HOME that has never seen kretro: --doctor, --licenses, and
# `play <id> --dry-run` for each game, which mounts the game, makes its prefix
# and stops before starting it. Where the overlay cannot be put up on this
# host, the dry run falls back to --extract-to and says which path it took.
# Last, one byte of one pack is flipped and only that game may say it is
# damaged.
#
# Everything is under one scratch directory - kretro's state, the player's
# state, the caches, and XDG_RUNTIME_DIR, so every FUSE mount made here is
# under it too - and all of it is unmounted and removed on the way out. It
# wants a few gigabytes of scratch space; TMPDIR moves it. The discs come
# from iso/, or KRETRO_ISO_DIR.
#
# The games are local configuration, never committed, usually set in
# tests/local.env (see tests/README.md):
#
#   KRETRO_TEST_GAMES     ids whose manifests install unattended (method "copy"
#                         or "unzip", one disc named by `iso`); two or more
#                         also test that damage to one game spares the others
#   KRETRO_TEST_NO_DISCS  ids to pack without their disc, for a disc that is
#                         mostly other games
set -uo pipefail
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"
load_local_env

BIN="$(realpath -m "${KRETRO_BIN:-build/kretro}")"
PACKER="$(realpath -m "${KGPACK_BIN:-build/kgpack}")"
B3="$(realpath -m "${KRETRO_B3:-build/kretro-b3}")"
ISO_DIR="$(realpath -m "${KRETRO_ISO_DIR:-iso}")"

[ -n "${KRETRO_TEST_GAMES:-}" ] || skip "no KRETRO_TEST_GAMES - set it in tests/local.env (see tests/README.md)"
[ -d "$ISO_DIR" ] || skip "no $ISO_DIR - this one needs your own discs"
for t in "$BIN" "$PACKER" "$B3"; do
  [ -x "$t" ] || skip "no $t - run: make"
done
command -v python3 >/dev/null || skip "no python3 to read the table with"

MANIFESTS="${KRETRO_MANIFESTS:-games}"
iso_of() { sed -n 's/^iso *= *"\(.*\)"/\1/p' "$MANIFESTS/$1.toml" 2>/dev/null | head -n 1; }
has_disc() { local iso; iso="$(iso_of "$1")"; [ -n "$iso" ] && [ -f "$ISO_DIR/$iso" ]; }
GAMES=()
read -r -a WANT <<<"$KRETRO_TEST_GAMES"
if has_disc "${WANT[0]}"; then GAMES+=("${WANT[0]}"); else
  skip "the disc of ${WANT[0]} ($MANIFESTS/${WANT[0]}.toml) is not in $ISO_DIR"; fi
for id in "${WANT[@]:1}"; do has_disc "$id" && GAMES+=("$id"); done

show() { sed 's/^/        /' "$1" | tail -n "${2:-15}"; }

# Every mount made here is under $WORK: our_mounts finds them, and they are
# all taken down before it is removed.
scratch bundle
WORK="$SCRATCH"
mkdir -m 700 "$WORK/run"
scratch_cleanup() { unmount_ours; }

export XDG_RUNTIME_DIR="$WORK/run"
export KRETRO_ISO_DIR="$ISO_DIR"
unset KRETRO_GUI KRETRO_PLAYER_BASE KRETRO_STATE KRETRO_DEBUG

# kretro, as the author: a home and a shelf of its own.
author() {
  env HOME="$WORK/author" XDG_DATA_HOME="$WORK/author/data" XDG_CACHE_HOME="$WORK/author/cache" \
      KRETRO_STATE="$WORK/shelf" "$BIN" "$@"
}
# The player, as a stranger: nothing of kretro's, nothing from a run before.
stranger() {
  local home="$1"; shift
  env -u XDG_DATA_HOME HOME="$home" XDG_CACHE_HOME="$home/.cache" XDG_CONFIG_HOME="$home/.config" "$@"
}

echo "bundle: ${GAMES[*]}"

# --- the installs -------------------------------------------------------------
for id in "${GAMES[@]}"; do
  # A disc that is mostly other games - a compilation - would make the pack
  # gigabytes of games it is not: KRETRO_TEST_NO_DISCS packs those without.
  extra=()
  case " ${KRETRO_TEST_NO_DISCS:-} " in *" $id "*) extra=(--no-discs) ;; esac
  if author install "$id" --headless "${extra[@]}" >"$WORK/install-$id.log" 2>&1; then
    ok "$id installs headless ($(du -h "$WORK/shelf/games/$id.kgpack" | cut -f1))"
  else
    bad "$id did not install"; show "$WORK/install-$id.log"
    finish; exit 1
  fi
done

# --- the build -------------------------------------------------------------------
PLAYER="$WORK/out/classics.run"
mkdir -p "$WORK/out"
if author bundle build -o "$WORK/out" --id kretro-bundle-test --title "kretro bundle test" --version 1 \
     "${GAMES[@]}" >"$WORK/refused.log" 2>&1; then
  bad "a build without --rights was not refused"
else
  grep -q -- "--rights" "$WORK/refused.log" && ok "a build without --rights is refused, and says so" \
                                           || { bad "the refusal did not name --rights"; show "$WORK/refused.log"; }
fi
if author bundle build -o "$PLAYER" --id kretro-bundle-test --title "kretro bundle test" --version 1 --rights \
     "${GAMES[@]}" >"$WORK/build.log" 2>&1; then
  ok "kretro bundle build wrote $(basename "$PLAYER") ($(du -h "$PLAYER" | cut -f1))"
else
  bad "kretro bundle build failed"; show "$WORK/build.log"
  finish; exit 1
fi
# -o names the file, so the build is written as that name's .partial; and
# none of any name, from either build, is left.
partials="$(find "$WORK/out" -maxdepth 1 -name '*.partial' | xargs -r -n1 basename)"
[ ! -e "$PLAYER.partial" ] && [ -z "$partials" ] && ok "no .partial is left beside it" \
                                                  || bad "a .partial was left behind: $partials"
# Remembered, as a build from the page is: the README's build, list and
# rebuild run as written.
if author bundle list >"$WORK/list.log" 2>&1 && grep -q "^kretro-bundle-test " "$WORK/list.log"; then
  ok "kretro bundle list remembers it, for kretro bundle rebuild"
else
  bad "kretro bundle list does not remember the build"; show "$WORK/list.log"
fi

# --- the table ---------------------------------------------------------------------
# One line per entry: kind, name, offset, length, BLAKE3. The layout is the
# one src/bundle/toc.h describes: a 64-byte trailer naming the table, and
# fixed 128-byte records.
python3 - "$PLAYER" >"$WORK/toc.txt" 2>"$WORK/toc.err" <<'EOF'
import sys
f = open(sys.argv[1], "rb"); f.seek(-64, 2); t = f.read(64)
assert t[:8] == b"KRETROv4", "no v4 trailer"
off, ln = int.from_bytes(t[16:24], "little"), int.from_bytes(t[24:32], "little")
f.seek(off); toc = f.read(ln)
assert toc[:4] == b"KTOC", "no KTOC"
kinds = {1: "tools", 2: "runtime", 3: "app", 4: "meta", 5: "pack", 6: "player-base"}
for i in range(int.from_bytes(toc[8:12], "little")):
    r = toc[16 + 128 * i:16 + 128 * (i + 1)]
    k = int.from_bytes(r[0:4], "little")
    name = r[56:120].rstrip(b"\0").decode() or "-"
    print(kinds.get(k, "kind%d" % k), name, int.from_bytes(r[8:16], "little"),
          int.from_bytes(r[16:24], "little"), r[24:56].hex())
EOF
[ -s "$WORK/toc.txt" ] || { bad "the table could not be read: $(cat "$WORK/toc.err")"; finish; exit 1; }

is_kinds="$(cut -d' ' -f1 "$WORK/toc.txt" | paste -sd' ')"
want_kinds="tools runtime app meta$(printf ' pack%.0s' "${GAMES[@]}")"
[ "$is_kinds" = "$want_kinds" ] && ok "the table is $is_kinds" || bad "the table is '$is_kinds', not '$want_kinds'"
packs="$(awk '$1 == "pack" { print $2 }' "$WORK/toc.txt" | paste -sd' ')"
[ "$packs" = "${GAMES[*]}" ] && ok "its packs are the games asked for, in order: $packs" \
                               || bad "its packs are '$packs', not '${GAMES[*]}'"

# Each pack: the shelf's bytes exactly, the BLAKE3 its entry says, and the
# tree root the shelf's pack has, read back out of the copy.
while read -r kind name off len hash; do
  [ "$kind" = pack ] || continue
  shelf="$WORK/shelf/games/$name.kgpack"
  [ "$len" = "$(stat -c %s "$shelf")" ] || { bad "$name: the entry is $len bytes, the shelf's pack $(stat -c %s "$shelf")"; continue; }
  if cmp -s -n "$len" -i "$off:0" "$PLAYER" "$shelf"; then
    ok "$name: the pack inside is the shelf's, byte for byte ($len bytes at +$off)"
  else
    bad "$name: the pack inside differs from the shelf's"
  fi
  got="$(tail -c +"$((off + 1))" "$PLAYER" | head -c "$len" | "$B3" - | cut -d' ' -f1)"
  [ "$got" = "$hash" ] && ok "$name: its BLAKE3 is the one the table records" \
                       || bad "$name: BLAKE3 $got, the table says $hash"
  tail -c +"$((off + 1))" "$PLAYER" | head -c "$len" >"$WORK/copy-$name.kgpack"
  root_in="$("$PACKER" info "$WORK/copy-$name.kgpack" 2>/dev/null | sed -n 's/^merkle root  *//p' | head -n 1)"
  root_shelf="$("$PACKER" info "$shelf" 2>/dev/null | sed -n 's/^merkle root  *//p' | head -n 1)"
  if [ -n "$root_in" ] && [ "$root_in" = "$root_shelf" ]; then
    ok "$name: its Merkle root is the shelf's ($root_in)"
  else
    bad "$name: Merkle root '$root_in' inside, '$root_shelf' on the shelf"
  fi
  "$PACKER" verify "$WORK/copy-$name.kgpack" >"$WORK/verify-$name.log" 2>&1 \
    && ok "$name: kgpack verify passes on the copy out of the player" \
    || { bad "$name: kgpack verify fails on the copy"; show "$WORK/verify-$name.log" 5; }
  rm -f "$WORK/copy-$name.kgpack"
done <"$WORK/toc.txt"

# --- the player, as a stranger runs it ---------------------------------------------------
S1="$WORK/stranger"
mkdir -p "$S1"
stranger "$S1" "$PLAYER" --licenses >"$WORK/licenses.txt" 2>&1
rc=$?
if [ "$rc" -eq 0 ] && grep -q "SOURCES.txt" "$WORK/licenses.txt" && grep -qx "  wine/" "$WORK/licenses.txt"; then
  ok "--licenses lists the runtime's notices and where the source is"
else
  bad "--licenses (exit $rc) did not list wine/ and SOURCES.txt"; show "$WORK/licenses.txt"
fi

stranger "$S1" "$PLAYER" --doctor >"$WORK/doctor.txt" 2>&1
rc=$?
if grep -q "this player" "$WORK/doctor.txt" && grep -q "$(printf '%s' "${GAMES[*]}" | sed 's/ /, /g')" "$WORK/doctor.txt"; then
  ok "--doctor reports on this player and its games (exit $rc: $([ "$rc" -eq 0 ] && echo "nothing blocking" || echo "something blocking on this host"))"
else
  bad "--doctor (exit $rc) did not report on this player"; show "$WORK/doctor.txt" 30
fi
note "doctor: FUSE $(sed -n 's/^ *fuse  *//p' "$WORK/doctor.txt" | head -n 1), the runtime reached by $(sed -n 's/^ *used  *//p' "$WORK/doctor.txt" | head -n 1)"
[ -d "$S1/.local/share/kretro-bundle-test" ] && ok "its state is ~/.local/share/kretro-bundle-test, keyed by the bundle id" \
                                           || bad "no state at ~/.local/share/kretro-bundle-test"
[ ! -e "$S1/.local/share/kretro" ] && ok "and nothing of kretro's is made there" || bad "the player made a kretro state"

# play --dry-run: mounted and overlaid when the host allows it, and unpacked
# with --extract-to when it does not.
dry_run() {  # home id log
  stranger "$1" "$PLAYER" play "$2" --dry-run >"$3" 2>&1
}
path_of() { sed -n 's/^ *game directory  *//p' "$1" | head -n 1; }
for id in "${GAMES[@]}"; do
  log="$WORK/dry-$id.log"
  if dry_run "$S1" "$id" "$log" && grep -q "is ready to play" "$log"; then
    ok "$id: play --dry-run is ready to play ($(path_of "$log"))"
  elif grep -q "unpacked\|cannot mount\|--extract-to" "$log"; then
    note "$id: not playable in place here: $(grep -m1 -i "unpack\|mount" "$log" | sed 's/^ *//')"
    if stranger "$S1" "$PLAYER" --extract-to "$WORK/unpacked" "$id" >"$WORK/extract-$id.log" 2>&1; then
      ok "$id: --extract-to unpacked it into $WORK/unpacked/$id"
      if dry_run "$S1" "$id" "$log" && grep -q "is ready to play" "$log"; then
        ok "$id: play --dry-run is ready to play, from the unpacked copy ($(path_of "$log"))"
      else
        bad "$id: play --dry-run failed even unpacked"; show "$log"
      fi
    else
      bad "$id: --extract-to failed"; show "$WORK/extract-$id.log"
    fi
  else
    bad "$id: play --dry-run failed"; show "$log"
  fi
  [ -s "$S1/.local/share/kretro-bundle-test/$id/prefix/system.reg" ] &&
    [ -s "$S1/.local/share/kretro-bundle-test/$id/prefix/.kretro-wine-version" ] &&
    ok "$id: its prefix is the runtime's template, made its own in its state" ||
    bad "$id: no prefix made from the template in its state"
done
left="$(our_mounts | grep -v '/kretro/rt-' || true)"
[ -z "$left" ] && ok "the dry runs left no game mounted" || { bad "mounts left behind by the dry runs:"; printf '        %s\n' $left; }

# --- damage ----------------------------------------------------------------------------------
# One byte in the middle of the first game's pack. The table's own hash does
# not cover the payloads, so the file still starts; the first play of that
# game hashes its pack and must say it is damaged, and only that game.
DAMAGED="$WORK/out/damaged.run"
cp --reflink=auto "$PLAYER" "$DAMAGED"
read -r _ first off len _ < <(awk '$1 == "pack"' "$WORK/toc.txt" | head -n 1)
python3 -c "
f = open('$DAMAGED', 'r+b'); f.seek($off + $len // 2); b = f.read(1); f.seek(-1, 1); f.write(bytes([b[0] ^ 0x40]))"
S2="$WORK/stranger2"
mkdir -p "$S2"
PLAYER_SAVED="$PLAYER"; PLAYER="$DAMAGED"
dry_run "$S2" "$first" "$WORK/damaged-$first.log"; rc=$?
if [ "$rc" -ne 0 ] && grep -q "is damaged inside this file" "$WORK/damaged-$first.log"; then
  ok "$first, one byte flipped: \"$(grep -m1 'is damaged inside' "$WORK/damaged-$first.log")\""
else
  bad "$first with a flipped byte was not called damaged (exit $rc)"; show "$WORK/damaged-$first.log"
fi
# The others still play: each gets the verdict it got from the whole file -
# ready, or the same explained way round a host that cannot mount it - and a
# crash, a refusal of the whole file or silence is not "not called damaged".
[ "${#GAMES[@]}" -gt 1 ] || note "one game only (one id in KRETRO_TEST_GAMES, or only one disc here): that the others still play is not tested here"
for id in "${GAMES[@]}"; do
  [ "$id" = "$first" ] && continue
  dry_run "$S2" "$id" "$WORK/damaged-$id.log"; rc=$?
  verdict="$(grep -m1 -o 'is ready to play\|has to be unpacked\|cannot mount' "$WORK/damaged-$id.log" || true)"
  if grep -q "is damaged" "$WORK/damaged-$id.log"; then
    bad "$id was called damaged when only $first was"; show "$WORK/damaged-$id.log"
  elif [ -z "$verdict" ]; then
    bad "$id in the damaged file got no verdict at all (exit $rc)"; show "$WORK/damaged-$id.log"
  elif [ "$verdict" = "is ready to play" ] && [ "$rc" -ne 0 ]; then
    bad "$id said it is ready to play and exited $rc"; show "$WORK/damaged-$id.log"
  else
    ok "$id in the same file is not called damaged, and still plays ($verdict)"
  fi
done
PLAYER="$PLAYER_SAVED"

left="$(our_mounts | grep -v '/kretro/rt-' || true)"
[ -z "$left" ] && ok "nothing of a game is left mounted" || bad "left mounted: $left"
note "runtime mounts to take down on the way out: $(our_mounts | wc -l)"

finish
