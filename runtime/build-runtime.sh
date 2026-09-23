#!/usr/bin/env bash
# Turns the runtime image into the runtimes that get packed into binaries:
#
#   build/runtime.dwarfs         kretro's, with the authoring tools
#   build/player-runtime.dwarfs  the player's, without them
#
#   build-runtime.sh [kretro] [player]     both when none is named
#
# Two containers: the builder supplies the DwarFS tool, the runtime image
# supplies its own filesystem. Neither leaves anything on the host but the
# finished images.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
BUILD="${BUILD:-build}"
RUNTIME="${RUNTIME:-kretro-runtime:latest}"
UID_GID="$(id -u):$(id -g)"
profiles=("$@")
[ ${#profiles[@]} -gt 0 ] || profiles=(kretro player)

mkdir -p "$BUILD"
echo "==> staging the DwarFS tool"
docker run --rm -v "$ROOT/$BUILD:/out" kretro-builder:latest \
  bash -c "cp /usr/local/bin/dwarfs-universal /out/ && chown $UID_GID /out/dwarfs-universal"

for profile in "${profiles[@]}"; do
  case "$profile" in
    kretro)
      [ -f "$BUILD/kretro-gui" ] || { echo "build $BUILD/kretro-gui first (make)" >&2; exit 1; }
      image=runtime.dwarfs
      # kretro's runtime also carries what the wizard knows, when this machine
      # has any: the manifests in games/ and the list of known discs in
      # db/discs.txt. Both are local, gitignored data; a checkout without them
      # builds a runtime without them, and kretro works the same minus the
      # presets and the disc names.
      extra='mkdir -p /tmp/rt/usr/share/kretro
             if [ -d /local/games ]; then
               cp -r /local/games /tmp/rt/usr/share/kretro/games
               echo "    $(ls /tmp/rt/usr/share/kretro/games | wc -l) manifests"
             else
               echo "    no games/ here: no manifests"
             fi
             if [ -f /local/db/discs.txt ]; then
               cp /local/db/discs.txt /tmp/rt/usr/share/kretro/discs.txt
             else
               echo "    no db/discs.txt here: no known discs"
             fi'
      mkflags=""
      ;;
    player)
      image=player-runtime.dwarfs
      extra=':'
      # --categorize sorts the input by what it is before choosing how to
      # compress it: already-compressed data (fonts, PNGs, the odd .cab) is
      # stored rather than squeezed a second time, and PCM gets FLAC. It costs
      # nothing to read, and the player's runtime is the one that ships in
      # every bundle.
      mkflags="--categorize"
      ;;
    *) echo "build-runtime: unknown profile '$profile' (kretro or player)" >&2; exit 2 ;;
  esac

  # Mounted only when present: a bind mount of a missing directory would have
  # Docker create it on the host, owned by root.
  local_mounts=()
  [ -d "$ROOT/games" ] && local_mounts+=(-v "$ROOT/games:/local/games:ro")
  [ -d "$ROOT/db" ] && local_mounts+=(-v "$ROOT/db:/local/db:ro")

  echo "==> assembling and packing the $profile runtime"
  docker run --rm \
    -v "$ROOT/$BUILD:/out" "${local_mounts[@]}" \
    -v "$ROOT/licenses:/licenses:ro" -v "$ROOT/runtime:/rtsrc:ro" \
    -e PLAYER_DLL_WHITELIST="${PLAYER_DLL_WHITELIST:-0}" \
    "$RUNTIME" bash -c "
    set -euo pipefail
    assemble-runtime /tmp/rt $profile
    $extra
    echo '==> packing'
    /out/dwarfs-universal --tool=mkdwarfs -i /tmp/rt -o /out/$image $mkflags \
        --log-level=warn --no-progress -f
    chown $UID_GID /out/$image
    ls -la /out/$image
  "
done
echo "==> done"
for profile in "${profiles[@]}"; do
  case "$profile" in
    kretro) ls -lh "$BUILD/runtime.dwarfs" ;;
    player) ls -lh "$BUILD/player-runtime.dwarfs" ;;
  esac
done
