# Helpers shared by the scripts in tests/integration/. Sourced, never run:
#
#   . "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"
#
# Sourcing it moves to the top of the repository and sets ROOT. Every script
# counts its checks the same way and ends with the same summary line as the
# unit tests, "N checks, M failed", through finish.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2

# The games a script uses are local configuration, never committed.
load_local_env() { [ -f tests/local.env ] && . tests/local.env; }

# Colour only on a terminal, so a log or a pipe gets plain text.
if [ -t 1 ]; then _GREEN=$'\033[32m' _RED=$'\033[31m' _PLAIN=$'\033[0m'
else _GREEN='' _RED='' _PLAIN=''; fi

CHECKS=0; FAILS=0
# ok LABEL and bad LABEL each count one check. QUIET_OK=1 counts a pass
# without printing it, for a script with more checks than anyone reads.
ok()   { CHECKS=$((CHECKS+1)); [ -n "${QUIET_OK:-}" ] || printf '  %sok%s    %s\n' "$_GREEN" "$_PLAIN" "$1"; }
bad()  { CHECKS=$((CHECKS+1)); FAILS=$((FAILS+1)); printf '  %sFAIL%s  %s\n' "$_RED" "$_PLAIN" "$1"; }
note() { printf '  --    %s\n' "$1"; }
# is LABEL GOT WANT
is()   { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 - wanted '$3', got '$2'"; fi; }
# check LABEL COMMAND... - passes when the command succeeds. CHECK_QUIET=1
# throws away what the command prints.
check() {
  local l="$1"; shift
  if [ -n "${CHECK_QUIET:-}" ]; then
    if "$@" >/dev/null 2>&1; then ok "$l"; else bad "$l"; fi
  else
    if "$@"; then ok "$l"; else bad "$l"; fi
  fi
}
# has TEXT and hasnt TEXT look in $OUT, the output of the last run.
has()   { printf '%s\n' "$OUT" | grep -qF -- "$1"; }
hasnt() { ! has "$1"; }

# skip MSG - nothing to test here, which is not a failure.
skip() { echo "skip: $1"; exit 0; }

# scratch NAME - makes $SCRATCH, a fresh directory under TMPDIR, and removes
# it on the way out. A script that has to do something first - take its
# mounts down, make read-only trees writable - defines scratch_cleanup, which
# runs before the removal.
scratch() {
  SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/kretro-$1.XXXXXX")"
  trap _scratch_exit EXIT
}
_scratch_exit() {
  if declare -F scratch_cleanup >/dev/null; then scratch_cleanup; fi
  rm -rf "$SCRATCH"
}

# Our mounts are the ones under $SCRATCH, and only those are touched.
# Innermost first, so an overlay goes before the image under it.
our_mounts() { awk -v w="$SCRATCH" 'index($2, w "/") == 1 { print $2 }' /proc/self/mounts | awk '{ print length, $0 }' |
               sort -rn | cut -d' ' -f2-; }
unmount_ours() {
  our_mounts | while read -r m; do fusermount3 -u "$m" 2>/dev/null || fusermount3 -u -z "$m" 2>/dev/null; done
}

# The summary line, and the exit status that goes with it.
finish() {
  printf '\n%d checks, %d failed\n' "$CHECKS" "$FAILS"
  [ "$FAILS" -eq 0 ]
}
