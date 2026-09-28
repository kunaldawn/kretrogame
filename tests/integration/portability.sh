#!/usr/bin/env bash
# Does the single binary actually run anywhere?
#
# This is the test the whole v2 architecture exists to pass. It runs the built
# binary on distributions with different libcs and with and without FUSE, and
# asserts it reaches the bundled runtime every time. Alpine is the one that
# matters: it is musl, so a binary that borrows the host libc cannot work there.
#
#   tests/integration/portability.sh                 kretro, on every distro
#   tests/integration/portability.sh alpine          kretro, on one
#   tests/integration/portability.sh player          a player, on every distro
#   tests/integration/portability.sh player alpine   a player, on one
#
# A player is what gets shipped, so it is the one a stranger's machine meets.
# KRETRO_PLAYER names one to run; without it, one is built here from a game
# made of three small files, with `kretro bundle build`, so the test needs no
# disc and no shelf. Each run asserts the player reaches its runtime and
# answers --licenses and --doctor, and that its game mounts - or, without
# FUSE, unpacks - with `play <game> --dry-run`; which way the runtime was
# reached is reported beside it.
set -uo pipefail
# Only the binaries go into the containers, and the commands run there are
# inline: nothing in them sources this library.
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"
# KRETRO_PLAYER and KRETRO_PLAYER_GAME may be kept in tests/local.env.
load_local_env

MODE=kretro
if [ "${1:-}" = player ]; then MODE=player; shift; fi
only="${1:-}"
run_one() { [ -z "$only" ] || [ "$only" = "$1" ]; }

BIN="${KRETRO_BIN:-build/kretro}"
[ -x "$BIN" ] || { echo "no binary at $BIN - run: make kretro" >&2; exit 2; }

# An image that is not here and cannot be pulled is skipped, not failed: this
# is a test of the binary, not of the network.
have_image() {
  docker image inspect "$1" >/dev/null 2>&1 || docker pull -q "$1" >/dev/null 2>&1 ||
    { note "$1: no image, and it cannot be pulled; skipped"; return 1; }
}

# Runs the binary in `image` and asserts it got as far as the bundled runtime.
# Extra docker arguments after the image enable the FUSE path.
try() {
  local label="$1" image="$2"; shift 2
  have_image "$image" || return 0
  local out
  out="$(docker run --rm "$@" -v "$ROOT/$BIN:/mnt/kretro:ro" "$image" \
        sh -c 'cp /mnt/kretro /usr/local/bin/kretro 2>/dev/null || cp /mnt/kretro /tmp/kretro
               k=$(command -v kretro || echo /tmp/kretro)
               HOME=/root "$k" info 2>&1
               echo "WINE: $(HOME=/root "$k" wine --version 2>&1 | tail -1)"' 2>&1)"
  # The runtime is reached, and the bundled Wine inside it actually runs. On a
  # musl host that second half is the whole argument for shipping our own libc.
  if printf '%s' "$out" | grep -q 'WINE: wine-'; then
    ok "$label  ($(printf '%s' "$out" | sed -n 's/^WINE: //p' | head -1))"
  else
    bad "$label"
    printf '%s\n' "$out" | sed 's/^/        /' | head -8
  fi
}

# The same for a player: its runtime is reached, it lists its licences, its
# doctor reports on it, and its game's pack mounts or unpacks - the part of
# the design's pass that a container without a display can show. --doctor
# exits 1 when something on the machine is blocking - a container has no GPU
# and no display - and that is an answer, not a failure; not reaching the
# doctor at all is.
#
# `play --dry-run` mounts the game, makes its prefix and stops before Wine.
# With /dev/fuse it has to be ready in place: a mount that falls back to
# "has to be unpacked" there is the FUSE path broken. Without, it has to say
# so, and then --extract-to has to unpack the game and the dry run be ready
# from the copy.
try_player() {
  local label="$1" image="$2"; shift 2
  have_image "$image" || return 0
  local want=unpacked
  case " $* " in *" /dev/fuse "*) want=mounted ;; esac
  local out
  out="$(docker run --rm "$@" -v "$PLAYER:/mnt/game.run:ro" "$image" \
        sh -c 'cp /mnt/game.run /usr/local/bin/game.run 2>/dev/null || cp /mnt/game.run /tmp/game.run
               p=$(command -v game.run || echo /tmp/game.run)
               HOME=/root "$p" --licenses >/tmp/lic.txt 2>&1; echo "LICENSES-RC: $?"
               grep -q "SOURCES.txt" /tmp/lic.txt && echo "LICENSES: listed"
               HOME=/root "$p" --doctor >/tmp/doc.txt 2>&1; echo "DOCTOR-RC: $?"
               grep -q "this player" /tmp/doc.txt && echo "DOCTOR: reported"
               sed -n "s/^ *used  */RUNTIME: /p" /tmp/doc.txt | head -1
               HOME=/root "$p" play '"$GAME"' --dry-run >/tmp/dry.txt 2>&1; echo "DRY-RC: $?"
               grep -m1 -o "is ready to play\|has to be unpacked\|cannot mount" /tmp/dry.txt | sed "s/^/DRY: /"
               if ! grep -q "is ready to play" /tmp/dry.txt; then
                 HOME=/root "$p" --extract-to /root/unpacked '"$GAME"' >/tmp/ext.txt 2>&1; echo "EXTRACT-RC: $?"
                 HOME=/root "$p" play '"$GAME"' --dry-run >/tmp/dry2.txt 2>&1; echo "DRY2-RC: $?"
                 grep -m1 -o "is ready to play" /tmp/dry2.txt | sed "s/^/DRY2: /"
                 tail -n 4 /tmp/dry.txt /tmp/ext.txt /tmp/dry2.txt | sed "s/^/  | /"
               fi
               tail -n 3 /tmp/doc.txt /tmp/lic.txt | sed "s/^/  | /"' 2>&1)"
  said() { printf '%s' "$out" | sed -n "s/^$1: //p" | head -1; }
  local dry; dry="$(said DRY)"; [ -n "$dry" ] || dry="exit $(said DRY-RC)"
  local played=""
  if [ "$(said DRY)" = "is ready to play" ] && [ "$(said DRY-RC)" = 0 ]; then
    played="ready in place"
  elif [ "$want" = unpacked ] && [ -n "$(said DRY)" ] && [ "$(said EXTRACT-RC)" = 0 ] &&
       [ "$(said DRY2)" = "is ready to play" ] && [ "$(said DRY2-RC)" = 0 ]; then
    played="$dry, then ready from --extract-to"
  fi
  if [ "$(said LICENSES)" = listed ] && [ "$(said DOCTOR)" = reported ] && [ -n "$played" ]; then
    ok "$label  (runtime by $(said RUNTIME), doctor exit $(said DOCTOR-RC), dry run: $played)"
  else
    bad "$label  (dry run: $dry; wanted the game $want)"
    printf '%s\n' "$out" | sed 's/^/        /' | tail -16
  fi
}

FUSE_ARGS=(--device /dev/fuse --cap-add SYS_ADMIN --security-opt apparmor=unconfined)

if [ "$MODE" = kretro ]; then
  echo "single binary portability"
  # musl, and with no glibc loader anywhere on the system: the case that decides
  # whether shipping our own libc was necessary.
  run_one alpine && try "alpine   musl, extract fallback" alpine:latest
  run_one alpine && try "alpine   musl, FUSE mount      " alpine:latest "${FUSE_ARGS[@]}"
  # A noexec /tmp would stop the mount and the extraction together if both ran
  # the DwarFS tool from a file under it. The tool runs from memory; what still
  # has to be a file moves to wherever programs may run.
  run_one alpine && try "alpine   FUSE, noexec /tmp     " alpine:latest "${FUSE_ARGS[@]}" \
    --tmpfs /tmp:rw,noexec,mode=1777
  run_one alpine && try "alpine   extract, noexec /tmp  " alpine:latest --tmpfs /tmp:rw,noexec,mode=1777
  run_one debian && try "debian   glibc, extract        " debian:trixie
  run_one fedora && try "fedora   glibc, extract        " fedora:latest
  run_one arch   && try "arch     glibc, extract        " archlinux:latest
  finish
  exit
fi

# --- player mode -----------------------------------------------------------------
scratch portability
WORK="$SCRATCH"
mkdir -m 700 "$WORK/run"
scratch_cleanup() {
  awk -v w="$WORK" 'index($2, w "/") == 1 { print $2 }' /proc/self/mounts | sort -r |
    while read -r m; do fusermount3 -u "$m" 2>/dev/null || fusermount3 -u -z "$m" 2>/dev/null; done
}

GAME=tiny
if [ -n "${KRETRO_PLAYER:-}" ]; then
  PLAYER="$(realpath "$KRETRO_PLAYER")"
  GAME="${KRETRO_PLAYER_GAME:-$(python3 -c "
import sys
f=open(sys.argv[1],'rb'); f.seek(-64,2); t=f.read(64); o=int.from_bytes(t[16:24],'little')
f.seek(o); toc=f.read(int.from_bytes(t[24:32],'little'))
for i in range(int.from_bytes(toc[8:12],'little')):
    r=toc[16+128*i:16+128*(i+1)]
    if int.from_bytes(r[0:4],'little')==5: print(r[56:120].rstrip(b'\0').decode()); break" "$PLAYER")}"
else
  # A game of three files, packed the way install packs one, on a shelf of
  # its own, and a player built from it by the kretro under test.
  PACKER="${KGPACK_BIN:-build/kgpack}"
  DW="${KRETRO_DWARFS:-build/dwarfs-universal}"
  [ -x "$PACKER" ] && [ -x "$DW" ] || { echo "no $PACKER or $DW to make a player with - run: make, or set KRETRO_PLAYER" >&2; exit 2; }
  mkdir -p "$WORK/bin" "$WORK/tiny" "$WORK/shelf/games" "$WORK/shelf/packs"
  ln -s "$(realpath "$DW")" "$WORK/bin/mkdwarfs"
  printf 'MZ not really a program\n' >"$WORK/tiny/TINY.EXE"
  printf 'the whole of the data\n' >"$WORK/tiny/data.txt"
  printf 'read me\n' >"$WORK/tiny/readme.txt"
  PATH="$WORK/bin:$PATH" "$PACKER" create --from "$WORK/tiny" --out "$WORK/tiny.kgpack" --id tiny \
    --name "Tiny" --year 1999 --exe TINY.EXE --dwarfs >"$WORK/pack.log" 2>&1 ||
    { cat "$WORK/pack.log" >&2; echo "could not pack the tiny game" >&2; exit 2; }
  # On the shelf as an install puts a game there: the set under packs/, and
  # the game's index naming it.
  SET="$("$PACKER" info "$WORK/tiny.kgpack" | sed -n 's/^set  *//p')"
  mv "$WORK/tiny.kgpack" "$WORK/shelf/packs/$SET.kgpack"
  printf '%s\n' "$SET" >"$WORK/shelf/games/tiny.set"
  PLAYER="$WORK/tiny.run"
  env HOME="$WORK/author" XDG_RUNTIME_DIR="$WORK/run" XDG_CACHE_HOME="$WORK/author/cache" KRETRO_STATE="$WORK/shelf" \
    "$BIN" bundle build -o "$PLAYER" --id portability --title "Portability" --rights tiny >"$WORK/build.log" 2>&1 ||
    { cat "$WORK/build.log" >&2; echo "could not build a player" >&2; exit 2; }
fi

echo "player portability: $(basename "$PLAYER") ($(du -h "$PLAYER" | cut -f1)), game $GAME"
run_one alpine && try_player "alpine   musl, extract fallback" alpine:latest
run_one alpine && try_player "alpine   musl, FUSE mount      " alpine:latest "${FUSE_ARGS[@]}"
run_one alpine && try_player "alpine   FUSE, noexec /tmp     " alpine:latest "${FUSE_ARGS[@]}" \
  --tmpfs /tmp:rw,noexec,mode=1777
run_one alpine && try_player "alpine   extract, noexec /tmp  " alpine:latest --tmpfs /tmp:rw,noexec,mode=1777
run_one debian && try_player "debian   glibc, extract        " debian:trixie
run_one debian && try_player "debian   glibc, FUSE mount     " debian:trixie "${FUSE_ARGS[@]}"
run_one fedora && try_player "fedora   glibc, extract        " fedora:latest
run_one arch   && try_player "arch     glibc, extract        " archlinux:latest
run_one ubuntu && try_player "ubuntu   26.04, extract        " ubuntu:26.04
run_one ubuntu && try_player "ubuntu   26.04, FUSE mount     " ubuntu:26.04 "${FUSE_ARGS[@]}"
run_one ubuntu && try_player "ubuntu   24.04, extract        " ubuntu:24.04

finish
