#!/usr/bin/env bash
# Records which DLLs a game loaded, for the player's DLL whitelist.
#
#   loaddll-whitelist.sh <name> <wine-log>...
#
# Play the game with WINEDEBUG=+loaddll (through `kretro play`, so the log is
# the session's own), then hand the log here. Each line Wine writes for a load
# looks like
#
#   0024:trace:loaddll:build_module Loaded L"C:\\windows\\system32\\dsound.dll" at ...: builtin
#
# and only the file name matters: the same DLL is kept in both architectures,
# because a game that loads it as 32-bit today may have a 64-bit sibling
# tomorrow and the saving is not worth that surprise. The names are merged into
# runtime/loaddll/<name>.txt, so playing more of the game only adds to it. A
# DLL the game shipped itself is listed too and does no harm: nothing in Wine's
# tree carries its name.
#
# The list takes effect only with `make runtime PLAYER_DLL_WHITELIST=1`; see
# runtime/player-keep-dlls.txt. A list named after a game stays on this
# machine (.gitignore); only wine-core.txt, Wine's own, is in the repository.
set -euo pipefail
name="${1:?usage: loaddll-whitelist.sh <name> <wine-log>...}"
shift
[ $# -gt 0 ] || { echo "usage: loaddll-whitelist.sh <name> <wine-log>..." >&2; exit 2; }
case "$name" in
  */*|.*|'') echo "loaddll-whitelist: '$name' is not a plain name" >&2; exit 2 ;;
esac

dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/loaddll"
mkdir -p "$dir"
out="$dir/$name.txt"
tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT

{
  [ -f "$out" ] && grep -v '^#' "$out" || true
  cat "$@" \
    | grep -o 'loaddll:[^ ]* Loaded L"[^"]*"' \
    | sed 's/.*Loaded L"//; s/"$//; s/.*\\//' \
    | tr 'A-Z' 'a-z' \
    | grep '\.dll$' || true
} | sort -u > "$tmp"

n="$(wc -l < "$tmp")"
[ "$n" -gt 0 ] || { echo "loaddll-whitelist: no loads in those logs - was WINEDEBUG=+loaddll set?" >&2; exit 1; }
{
  echo "# DLLs $name loaded, recorded by runtime/loaddll-whitelist.sh."
  cat "$tmp"
} > "$out"
echo "$out: $n DLLs"
