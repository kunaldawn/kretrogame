#!/usr/bin/env bash
# Tier 2: can a headless Weston give us an Xwayland root window we can read?
#
# This is the one question Milestone 4 exists to answer. It needs the bundled
# runtime - weston, Xwayland and wine - so it needs the linked binary and not
# just the objects; skipped when that is missing, the way scan.sh is.
#
# It needs no display of its own. That is the whole point: nothing is presented
# to the host, and the picture is read back out of the X server.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"; cd "$ROOT"

BIN="${KRETRO_BIN:-build/kretro}"
[ -x "$BIN" ] || { echo "skip: no $BIN - run: make"; exit 0; }

LOG="$(mktemp)"
OUT="$("$BIN" stage-probe "${1:-20}" 2>"$LOG")"
RC=$?

if [ "$RC" = 77 ]; then
  echo "skip: $(grep '^skip:' "$LOG" | tail -n 1)"
  rm -f "$LOG"
  exit 0
fi

printf '%s\n' "$OUT"
if [ "$RC" != 0 ]; then
  printf '  FAIL  headless Weston did not give a readable Xwayland root window\n'
  printf '        %s\n' "$(grep '^FAIL:' "$LOG" | tail -n 1)"
  printf '        the full log is at %s\n' "$LOG"
  printf '        there is no fallback: the installer is drawn as a panel in\n'
  printf '        our own window or it is not drawn at all - src/gui/stage.h\n'
  exit 1
fi

rm -f "$LOG"
printf '  ok    the stage can be built on this runtime\n'
