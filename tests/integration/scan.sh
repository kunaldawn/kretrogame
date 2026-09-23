#!/usr/bin/env bash
# Tier 2: the disc layer against your own collection. `kretro scan` over the
# image folder, held against a listing of what it must find: one line per
# download, its file name and how many discs are in it, tab-separated. Lines
# starting with # are comments.
#
# The folder and the listing are local and never committed: KRETRO_ISO_DIR
# (default iso/) and KRETRO_TEST_COLLECTION (default
# tests/local/expected-collection.txt), both usually set in tests/local.env.
# Skipped when either is missing, or when there is no built binary.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"; cd "$ROOT"
[ -f tests/local.env ] && . tests/local.env

ISO_DIR="${KRETRO_ISO_DIR:-iso}"
EXPECT="${KRETRO_TEST_COLLECTION:-tests/local/expected-collection.txt}"
[ -x build/kretro ] || { echo "skip: no build/kretro - run: make"; exit 0; }
[ -d "$ISO_DIR" ] || { echo "skip: no $ISO_DIR - set KRETRO_ISO_DIR (see tests/README.md)"; exit 0; }
[ -f "$EXPECT" ] || { echo "skip: no $EXPECT - nothing to hold the scan against (see tests/README.md)"; exit 0; }

FAIL=0
OUT="$(./build/kretro scan "$ISO_DIR")"
while IFS=$'\t' read -r name want; do
  case "$name" in ''|'#'*) continue ;; esac
  got="$(printf '%s\n' "$OUT" | awk -v n="$name" '$0==n{f=1;next} /^[^ ]/{f=0} f&&/^    disc /{c++} END{print c+0}')"
  if [ "$got" = "$want" ]; then
    printf '  ok    %s (%s discs)\n' "$name" "$got"
  else
    printf '  FAIL  %s: expected %s discs, got %s\n' "$name" "$want" "$got"
    FAIL=$((FAIL+1))
  fi
done < "$EXPECT"
printf 'failed %d\n' "$FAIL"
[ "$FAIL" -eq 0 ]
