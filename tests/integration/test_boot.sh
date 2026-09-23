#!/usr/bin/env bash
# The bootstrap, run for real against files built here.
#
# The bootstrap is C with no library to unit-test, so this builds whole files
# around it - the real bootstrap, the real DwarFS tool, a runtime image holding
# a stand-in loader, a stand-in app that reports what it was handed - and runs
# them. It runs inside kretro-builder without /dev/fuse, so every run here takes
# the extraction path; tests/integration/portability.sh is where FUSE is exercised.
#
# `make test` runs it with a noexec tmpfs at /nx, which is what the memfd
# fallback checks need. Without one, those checks are skipped, not failed.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

BUILD="${BUILD:-build}"
BOOT="$ROOT/$BUILD/bootstrap"
STUBS="$ROOT/$BUILD/boot-test"
DWARFS="${DWARFS_TOOL:-/usr/local/bin/dwarfs-universal}"
for f in "$BOOT" "$STUBS/stub_loader" "$STUBS/stub_app" "$STUBS/b3sum" "$DWARFS"; do
  [ -x "$f" ] || { echo "test_boot: missing $f - run: make boot-test" >&2; exit 2; }
done

CHECKS=0; FAILS=0
ok()  { CHECKS=$((CHECKS+1)); }
bad() { CHECKS=$((CHECKS+1)); FAILS=$((FAILS+1)); printf '  FAIL %s\n' "$1" >&2; }
# check LABEL COMMAND... - passes when the command succeeds
check() { local l="$1"; shift; if "$@"; then ok; else bad "$l"; fi; }
has()  { printf '%s\n' "$OUT" | grep -qF -- "$1"; }
hasnt() { ! has "$1"; }

T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT

# --- the parts ---------------------------------------------------------------
mkdir -p "$T/rt/lib"
cp "$STUBS/stub_loader" "$T/rt/lib/ld-linux-x86-64.so.2"
"$DWARFS" --tool=mkdwarfs -i "$T/rt" -o "$T/runtime.dwarfs" --log-level=error --no-progress >/dev/null 2>&1 ||
  { echo "test_boot: mkdwarfs failed" >&2; exit 2; }
printf 'format 1, but only a test would believe it\n' >"$T/bundle.meta"
head -c 20000 /dev/urandom >"$T/game.kgpack"
head -c 9000 /dev/urandom >"$T/player-base"

mkv4() { python3 tests/fixtures/boot/mkv4.py --b3 "$STUBS/b3sum" --bootstrap "$BOOT" "$@"; }
KRETRO_PARTS=(--part "tools=$DWARFS" --part "runtime=$T/runtime.dwarfs" --part "app=$STUBS/stub_app")

# Each run gets fresh XDG directories and a clean environment, so one run's
# extraction cannot make the next one pass.
fresh() {
  rm -rf "$T/run" "$T/cache"
  mkdir -p "$T/run" "$T/cache"
  chmod 700 "$T/run"
}
# go FILE ARGS... - runs FILE with the test's environment; output in OUT, status in RC
go() {
  OUT="$(env -i PATH="$PATH" HOME="$T/home" XDG_RUNTIME_DIR="${RUNDIR:-$T/run}" \
             XDG_CACHE_HOME="${CACHEDIR:-$T/cache}" ${EXTRA_ENV:-} "$@" 2>&1)"
  RC=$?
}
nothing_unpacked() {
  [ -z "$(find "$T/run" "$T/cache" -name 'rt-*' -o -name 'app-*' -o -name 'dwarfs-universal' 2>/dev/null)" ]
}

echo "bootstrap"

# --- a v4 kretro: tools, runtime, app, player base ----------------------------
mkdir -p "$T/k"
read -r TOC_OFF TOC_LEN < <(mkv4 --out "$T/k/kretro" "${KRETRO_PARTS[@]}" --part "player-base=$T/player-base")
fresh
go "$T/k/kretro" hello "two words"
check "v4 kretro runs its app" [ "$RC" -eq 0 ]
check "v4 kretro reaches the app through the loader" has "stub-app ran"
check "arguments pass through untouched" has "arg2=two words"
check "KRETRO_TOC is the table the trailer names" has "KRETRO_TOC=$TOC_OFF:$TOC_LEN"
check "KRETRO_SELF is the file that was run" has "KRETRO_SELF=$T/k/kretro"
check "the DwarFS tool runs from a memfd" has "KRETRO_DWARFS=/proc/self/fd/"
check "the app can run the tool it was handed" has "dwarfs=ok"
check "without FUSE the runtime is extracted" has "KRETRO_MOUNT_MODE=extract"
check "the extracted runtime is in the cache" has "KRETRO_RUNTIME=$T/cache/kretro/rt-"
check "KRETRO_APP names the unpacked app" has "KRETRO_APP=$T/run/kretro/app-"
check "a first extraction says so" has "unpacking the runtime"
check "the tool is never written to disk" \
  [ -z "$(find "$T/run" "$T/cache" -name dwarfs-universal)" ]
check "no state dir is chosen without kretro-data" has "KRETRO_STATE unset"

go "$T/k/kretro"
check "a second run reuses the extraction" hasnt "unpacking the runtime"
check "a second run still reaches the app" has "stub-app ran"

# An unpack is made whole beside its place and renamed in, marked finished,
# and nothing of the making is left beside it.
RT="$(find "$T/cache/kretro" -maxdepth 1 -name 'rt-*' -type d | head -n1)"
check "a finished unpack is marked so" [ -e "$RT/.kretro-unpacked" ]
check "nothing is left beside a finished unpack" \
  [ -z "$(find "$T/cache/kretro" -maxdepth 1 -name 'rt-*.*')" ]
# One that was cut short - Ctrl-C, a full disk - after its loader was
# written has no mark, and is not taken for a runtime: it is done again. So
# are what a run that died mid-unpack left beside it.
rm -f "$RT/.kretro-unpacked"
mkdir -p "$RT.999999999.partial/lib" && touch "$RT.999999999.image" "$RT.999999999.image.999999999.tmp"
go "$T/k/kretro"
check "an unpack without its mark is done again" has "unpacking the runtime"
check "and the app runs from the new one" has "stub-app ran"
check "which is marked finished" [ -e "$RT/.kretro-unpacked" ]
check "what a dead unpack left is removed" \
  [ -z "$(find "$T/cache/kretro" -maxdepth 1 -name 'rt-*.*')" ]

# Two first runs at once: each unpacks on its own, one rename wins, and both
# run from a whole runtime.
fresh
for i in 1 2; do
  env -i PATH="$PATH" HOME="$T/home" XDG_RUNTIME_DIR="$T/run" XDG_CACHE_HOME="$T/cache" \
    "$T/k/kretro" >"$T/together-$i.log" 2>&1 &
done
wait
both_ran() { grep -q "stub-app ran" "$T/together-1.log" && grep -q "stub-app ran" "$T/together-2.log"; }
check "two first runs at once both reach the app" both_ran
check "and leave one whole runtime and nothing beside it" \
  [ -z "$(find "$T/cache/kretro" -maxdepth 1 -name 'rt-*.*')" ]

EXTRA_ENV="KRETRO_BUNDLED_GAME=1:2" go "$T/k/kretro"
check "an inherited KRETRO_BUNDLED_GAME is dropped for v4" has "KRETRO_BUNDLED_GAME unset"

# The development override: kretro runs the app it names, and nothing it
# starts inherits the variable.
cp "$STUBS/stub_app" "$T/dev-app"
EXTRA_ENV="KRETRO_GUI=$T/dev-app" go "$T/k/kretro"
check "KRETRO_GUI swaps kretro's app" has "KRETRO_APP=$T/dev-app"
check "KRETRO_GUI is not handed down" has "KRETRO_GUI unset"

mkdir -p "$T/k/kretro-data"
go "$T/k/kretro"
check "kretro-data beside kretro is portable mode" has "KRETRO_STATE=$T/k/kretro-data"
rm -rf "$T/k/kretro-data"

# Started with stdin closed, the memfd must not become fd 0.
OUT="$(env -i PATH="$PATH" HOME="$T/home" XDG_RUNTIME_DIR="$T/run" XDG_CACHE_HOME="$T/cache" \
       "$T/k/kretro" 2>&1 <&-)"
check "the memfd stays clear of stdin" hasnt "KRETRO_DWARFS=/proc/self/fd/0"
check "the tool still runs with stdin closed" has "dwarfs=ok"

# --- a player: the same, plus bundle.meta and a pack --------------------------
mkdir -p "$T/p/game-data" "$T/p/kretro-data"
read -r P_OFF P_LEN < <(mkv4 --out "$T/p/game.run" "${KRETRO_PARTS[@]}" \
  --part "meta=$T/bundle.meta" --part "pack=$T/game.kgpack:example-game")
fresh
EXTRA_ENV="KRETRO_STATE=/somewhere/the/builder/keeps/its/own" go "$T/p/game.run" play example-game
check "a player runs" [ "$RC" -eq 0 ]
check "a player gets its table" has "KRETRO_TOC=$P_OFF:$P_LEN"
check "a player is not given a state dir" has "KRETRO_STATE unset"
check "a player's app is unpacked as the player" has "/player"

# A player takes no development override: a KRETRO_GUI left in a shell, or
# handed down by the kretro previewing it, would swap its app for kretro's.
EXTRA_ENV="KRETRO_GUI=$T/dev-app" go "$T/p/game.run" play example-game
check "a player ignores KRETRO_GUI" hasnt "KRETRO_APP=$T/dev-app"
check "a player still runs its own app" has "/player"
check "a player does not pass KRETRO_GUI on" has "KRETRO_GUI unset"

# --- damage ----------------------------------------------------------------------
SIZE=$(stat -c %s "$T/k/kretro")

head -c $((SIZE - 1000)) "$T/k/kretro" >"$T/cut"; chmod +x "$T/cut"
fresh; go "$T/cut"
check "a truncated file stops" [ "$RC" -ne 0 ]
check "a truncated file says how big it is" \
  has "This file is damaged or incomplete (it is $((SIZE - 1000)) bytes"
check "a truncated file says to download it again" has "Download it again."
check "a truncated file mounts and unpacks nothing" nothing_unpacked

# A file missing a stretch from its middle keeps its trailer, which remembers
# how long the file was: both numbers are known.
APP_OFF=$(python3 -c "
import sys; d=open('$T/k/kretro','rb').read(); t=d[-64:]; o=int.from_bytes(t[16:24],'little')
for i in range(int.from_bytes(d[o+8:o+12],'little')):
    r=d[o+16+128*i:o+16+128*(i+1)]
    if int.from_bytes(r[0:4],'little')==3: print(int.from_bytes(r[8:16],'little'))")
{ head -c $((APP_OFF + 4096)) "$T/k/kretro"; tail -c +$((APP_OFF + 8192 + 1)) "$T/k/kretro"; } >"$T/holed"
chmod +x "$T/holed"
fresh; go "$T/holed"
check "a file missing its middle gives both sizes" \
  has "This file is damaged or incomplete (it is $((SIZE - 4096)) bytes, it should be $SIZE). Download it again."
check "a file missing its middle unpacks nothing" nothing_unpacked

# One flipped bit in the table: the trailer's hash of it no longer matches.
cp "$T/k/kretro" "$T/flip"
python3 -c "
f=open('$T/flip','r+b'); f.seek($TOC_OFF + 16 + 9); b=f.read(1); f.seek(-1,1); f.write(bytes([b[0]^1]))"
fresh; go "$T/flip"
check "a damaged table stops" [ "$RC" -ne 0 ]
check "a damaged table is named" has "its table of contents does not match its checksum"
check "a damaged table unpacks nothing" nothing_unpacked

cp "$T/k/kretro" "$T/flip2"
python3 -c "
f=open('$T/flip2','r+b'); f.seek(-1,2); b=f.read(1); f.seek(-1,2); f.write(bytes([b[0]^0x80]))"
fresh; go "$T/flip2"
check "a damaged trailer hash is caught" has "does not match its checksum"

mkv4 --out "$T/oob" "${KRETRO_PARTS[@]}" --raw-entry "5:$((SIZE * 2 / 4096 * 4096)):4096" >/dev/null
fresh; go "$T/oob"
check "an entry past the end stops" [ "$RC" -ne 0 ]
check "an entry past the end is named" has "its game (entry 3"
check "an entry past the end unpacks nothing" nothing_unpacked

mkv4 --out "$T/unaligned" "${KRETRO_PARTS[@]}" --raw-entry "5:4097:10" >/dev/null
fresh; go "$T/unaligned"
check "an unaligned entry is refused" has "lies outside it"

mkv4 --out "$T/noruntime" --part "tools=$DWARFS" --part "app=$STUBS/stub_app" >/dev/null
fresh; go "$T/noruntime"
check "a table with no runtime is refused" has "it carries no runtime"

mkv4 --out "$T/two" "${KRETRO_PARTS[@]}" --part "runtime=$T/runtime.dwarfs" >/dev/null
fresh; go "$T/two"
check "a table with two runtimes is refused" has "it lists its runtime twice"

mkv4 --out "$T/newer" "${KRETRO_PARTS[@]}" --trailer-version 5 >/dev/null
fresh; go "$T/newer"
check "a newer layout says so" has "made by a newer kretro"
check "a newer layout unpacks nothing" nothing_unpacked

# --- v3 and v2 still run ---------------------------------------------------------
python3 tests/fixtures/boot/mkv3.py --bootstrap "$BOOT" --tools "$DWARFS" --image "$T/runtime.dwarfs" \
  --app "$STUBS/stub_app" --out "$T/v3" >/dev/null
fresh
EXTRA_ENV="KRETRO_TOC=1:2" go "$T/v3" old
check "a v3 binary still runs" has "arg1=old"
check "a v3 binary has no table to hand down" has "KRETRO_TOC unset"
check "a v3 binary carries no game unless it says so" has "KRETRO_BUNDLED_GAME unset"
check "a v3 binary's tool runs from memory too" has "KRETRO_DWARFS=/proc/self/fd/"
check "a v3 binary's app can run that tool" has "dwarfs=ok"

cp "$T/v3" "$T/v3game"
python3 -c "
f=open('$T/v3game','r+b'); f.seek(-16,2)
f.write((12288).to_bytes(8,'little')); f.write((777).to_bytes(8,'little'))"
fresh; go "$T/v3game"
check "a v3 binary carrying a game still runs" has "stub-app ran"
check "a v3 game slot is no longer handed down" has "KRETRO_BUNDLED_GAME unset"

cp "$T/v3" "$T/v2"
python3 -c "
f=open('$T/v2','r+b'); f.seek(-176,2); f.write(b'KRETROv2'); f.write((2).to_bytes(4,'little'))"
fresh; go "$T/v2"
check "a v2 binary still runs" has "stub-app ran"

# --- when memory will not run programs --------------------------------------------
fresh
EXTRA_ENV="KRETRO_BOOT_NO_MEMFD=1" go "$T/k/kretro"
check "without a memfd the tool goes to the cache" has "KRETRO_DWARFS=$T/cache/kretro/tools-"
check "the app can run the tool from the cache" has "dwarfs=ok"

# No HOME and no XDG_CACHE_HOME: what has to be a file goes to the runtime
# directory, which is checked to be ours, and never to /tmp/.cache, which
# anyone could have made first with a program at the path the key predicts.
fresh
rm -rf /tmp/.cache/kretro 2>/dev/null
OUT="$(env -i PATH="$PATH" XDG_RUNTIME_DIR="$T/run" KRETRO_BOOT_NO_MEMFD=1 "$T/k/kretro" 2>&1)"
check "with no home the tool goes to the runtime dir" has "KRETRO_DWARFS=$T/run/kretro/tools-"
check "with no home the runtime goes to the runtime dir" has "KRETRO_RUNTIME=$T/run/kretro/rt-"
check "and the app still runs" has "dwarfs=ok"
check "nothing is put in a shared /tmp/.cache" [ ! -e /tmp/.cache/kretro ]

# A tool the kernel will not execute from memory must move to a file. No
# kernel setting can be flipped from here, but a tool that is not a program at
# all fails the same exec the same way. Run once normally, and once with stdin
# and stdout closed: a desktop launcher can start a program like that, and the
# pipe that carries the exec error must not land on the descriptors the child
# points at /dev/null.
head -c 5000 /dev/urandom >"$T/not-a-program"
mkv4 --out "$T/badtool" --part "tools=$T/not-a-program" --part "runtime=$T/runtime.dwarfs" \
  --part "app=$STUBS/stub_app" >/dev/null
tool_on_disk() { [ -n "$(find "$T/cache" -path '*/tools-*/dwarfs-universal' 2>/dev/null)" ]; }
fresh; go "$T/badtool"
check "a tool memory will not run moves to a file" tool_on_disk
fresh
OUT="$(env -i PATH="$PATH" HOME="$T/home" XDG_RUNTIME_DIR="$T/run" XDG_CACHE_HOME="$T/cache" \
       "$T/badtool" 2>&1 <&- >&-)"
check "and does so with stdin and stdout closed" tool_on_disk

if [ -d /nx ] && [ -w /nx ] && grep -qE '^[^ ]+ /nx [^ ]+ [^ ]*noexec' /proc/mounts; then
  rm -rf /nx/*
  fresh
  CACHEDIR=/nx/cache EXTRA_ENV="KRETRO_BOOT_NO_MEMFD=1" go "$T/k/kretro"
  check "a noexec cache sends the tool to the runtime dir" has "KRETRO_DWARFS=$T/run/kretro/tools-"
  check "a noexec cache sends the runtime to the runtime dir" has "KRETRO_RUNTIME=$T/run/kretro/rt-"
  check "and the app still runs" has "dwarfs=ok"

  # The next run finds that extraction where a mount would be, and must still
  # call it an extraction: the app decides from this whether packs can mount.
  CACHEDIR=/nx/cache EXTRA_ENV="KRETRO_BOOT_NO_MEMFD=1" go "$T/k/kretro"
  check "a runtime extracted to the runtime dir is reused" hasnt "unpacking the runtime"
  check "and is still called an extraction" has "KRETRO_MOUNT_MODE=extract"

  rm -rf /nx/*
  fresh
  CACHEDIR=/nx/cache RUNDIR=/nx/run EXTRA_ENV="KRETRO_BOOT_NO_MEMFD=1" go "$T/k/kretro"
  check "with nowhere to run the tool, it stops" [ "$RC" -ne 0 ]
  check "and names the noexec mount" has "/nx is mounted noexec"

  # With a memfd, a noexec cache and runtime dir stop the tool no longer; they
  # stop only what has to be a file.
  rm -rf /nx/*
  fresh
  CACHEDIR=/nx/cache go "$T/k/kretro"
  check "with a memfd a noexec cache does not stop the tool" has "KRETRO_DWARFS=/proc/self/fd/"
  check "and the runtime still runs" has "dwarfs=ok"
else
  echo "  (no noexec /nx: skipping the noexec checks)"
fi

printf '\n%d checks, %d failed\n' "$CHECKS" "$FAILS"
[ "$FAILS" -eq 0 ]
