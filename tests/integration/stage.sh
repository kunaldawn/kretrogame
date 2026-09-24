#!/usr/bin/env bash
# Tier 2: can a headless Weston give us an Xwayland root window we can read?
#
# Installing inside the stage depends on the answer being yes. It needs the
# bundled runtime - weston, Xwayland and wine - so it needs the linked binary
# and not just the objects; skipped when that is missing, the way scan.sh is.
#
# It needs no display of its own. That is the whole point: nothing is presented
# to the host, and the picture is read back out of the X server.
set -uo pipefail
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

BIN="${KRETRO_BIN:-build/kretro}"
[ -x "$BIN" ] || skip "no $BIN - run: make"

LOG="$(mktemp)"
OUT="$("$BIN" stage-probe "${1:-20}" 2>"$LOG")"
RC=$?

if [ "$RC" = 77 ]; then
  why="$(grep '^skip:' "$LOG" | tail -n 1)"
  rm -f "$LOG"
  skip "$why"
fi

printf '%s\n' "$OUT"
if [ "$RC" != 0 ]; then
  bad "headless Weston did not give a readable Xwayland root window"
  printf '        %s\n' "$(grep '^FAIL:' "$LOG" | tail -n 1)"
  printf '        the full log is at %s\n' "$LOG"
  printf '        there is no fallback: the installer is drawn as a panel in\n'
  printf '        our own window or it is not drawn at all - src/gui/stage/stage.h\n'
  finish
  exit 1
fi

rm -f "$LOG"
ok "the stage can be built on this runtime"
finish
