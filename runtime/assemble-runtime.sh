#!/bin/bash
# Turns this image's filesystem into the relocatable runtime tree that gets
# packed into the binary.
#
#   assemble-runtime <outdir> [kretro|player]
#
# One image, two cuts of it. `kretro` is the builder's runtime: everything,
# including the tools that read discs and run installers. `player` is what a
# bundle carries: the same Wine, graphics and sound, without any of the
# authoring tools (runtime/player-prune.txt says exactly which).
#
# The Debian layout is kept intact rather than flattened, because Wine, Mesa
# and Weston all locate their own parts relative to paths they were built with.
# What the bootstrap needs is only a fixed contract:
#
#   <rt>/lib/ld-linux-x86-64.so.2     the loader, as a real file
#
# Everything else is found through environment variables set by the app. Some
# of those are not optional, because a compiled-in path is the host's once the
# tree has moved, and following it loads host code into a process running our
# libc:
#
#   ALSA_CONFIG_PATH        <rt>/usr/share/kretro/alsa/asound.conf
#   GST_PLUGIN_SYSTEM_PATH_1_0  <rt>/usr/lib/x86_64-linux-gnu/gstreamer-1.0
#   GST_PLUGIN_SCANNER_1_0  <rt>/usr/lib/x86_64-linux-gnu/gstreamer1.0/
#                           gstreamer-1.0/gst-plugin-scanner (RUNTIME records
#                           where this image put it)
#   GST_REGISTRY_1_0        a file in the cache, not ~/.cache/gstreamer-1.0,
#                           which the host's own GStreamer also writes
#   WINEUSERNAME, USER      player - the name the prefix template was built for
#
# Inputs the caller provides, both optional:
#   KRETRO_LICENSES  the repository's licenses/ directory
#   KRETRO_RTSRC     the repository's runtime/ directory (prune lists,
#                    recorded loaddll lists)
#   PLAYER_DLL_WHITELIST=1  enforce player-keep-dlls.txt; off by default
set -euo pipefail
out="${1:?usage: assemble-runtime <outdir> [kretro|player]}"
profile="${2:-kretro}"
case "$profile" in
  kretro|player) ;;
  *) echo "assemble-runtime: unknown profile '$profile' (kretro or player)" >&2; exit 2 ;;
esac
licenses_src="${KRETRO_LICENSES:-/licenses}"
rtsrc="${KRETRO_RTSRC:-/rtsrc}"
wine=/opt/wine-staging
rm -rf "$out"
mkdir -p "$out"

echo "==> copying the runtime tree ($profile)"
for d in bin sbin lib lib64 usr opt etc/fonts etc/alternatives etc/ld.so.conf.d; do
  [ -e "/$d" ] || continue
  mkdir -p "$out/$(dirname "$d")"
  cp -a "/$d" "$out/$d"
done

# Before the pruning, because the pruning takes /usr/share/doc, which is where
# Debian keeps every package's copyright file.
echo "==> licences"
lic="$out/usr/share/kretro/licenses"
mkdir -p "$lic"
if [ -d "$licenses_src" ]; then
  cp -r "$licenses_src"/. "$lic"/
  rm -f "$lic/SOURCES.in"
else
  echo "    no $licenses_src - the notices for Wine, DXVK, cnc-ddraw and DwarFS are missing" >&2
fi
# The components the design names, each under a name a person would look for,
# from the package that actually carries it in this image. A package that is
# not installed is skipped rather than failing the build: the full set below
# still has whatever is there.
named() {
  local name="$1"; shift
  local pkg
  for pkg in "$@"; do
    if [ -f "/usr/share/doc/$pkg/copyright" ]; then
      mkdir -p "$lic/$name"
      cp "/usr/share/doc/$pkg/copyright" "$lic/$name/copyright.$pkg"
    fi
  done
  [ -d "$lic/$name" ] || echo "    no copyright file for $name ($*)" >&2
}
named glibc          libc6
named gstreamer      libgstreamer1.0-0 gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-libav
named mesa           libgl1-mesa-dri libglx-mesa0 libegl-mesa0 mesa-vulkan-drivers libgbm1
named weston         weston libweston-14-0
named xwayland       xwayland
named fuse           libfuse3-4 libfuse3-3 fuse3
named fuse-overlayfs fuse-overlayfs
named sdl2           libsdl2-2.0-0 libsdl3-0
named alsa-lib       libasound2t64 libasound2
named pulseaudio     libpulse0
# And every package in the image, because the named ones link against the
# rest. Their copyright files refer to /usr/share/common-licenses for the full
# texts, so that comes along too.
mkdir -p "$lic/packages"
for f in /usr/share/doc/*/copyright; do
  p="$(basename "$(dirname "$f")")"
  cp "$f" "$lic/packages/$p"
done
[ -d /usr/share/common-licenses ] && cp -rL /usr/share/common-licenses "$lic/common-licenses"
{
  if [ -f "$licenses_src/SOURCES.in" ]; then cat "$licenses_src/SOURCES.in"; echo; fi
  echo "# Ubuntu packages, from dpkg: package, version, source package, source"
  echo "# version, and where that source lives."
  dpkg-query -W -f='${db:Status-Abbrev} ${Package} ${Version} ${source:Package} ${source:Version}\n' \
    | awk '$1 == "ii" { printf "%-40s %-32s %s %s https://launchpad.net/ubuntu/+source/%s/%s\n", $2, $3, $4, $5, $4, $5 }'
} > "$lic/SOURCES.txt"

echo "==> pruning"
# Documentation, locales, headers, static archives and apt's bookkeeping are
# all dead weight in something that ships inside a binary.
rm -rf "$out"/usr/share/doc "$out"/usr/share/man "$out"/usr/share/info \
       "$out"/usr/share/locale "$out"/usr/share/lintian "$out"/usr/share/bug \
       "$out"/usr/share/common-licenses \
       "$out"/var/lib/apt "$out"/var/cache "$out"/var/log \
       "$out"/usr/include "$out"/usr/share/gtk-doc 2>/dev/null || true
find "$out" -name '*.a' -delete 2>/dev/null || true
find "$out" -name '*.la' -delete 2>/dev/null || true
# Wine's own tests, its C headers (for winegcc, which nobody here runs), and
# its desktop integration are never used by anything that plays a game. The
# import libraries went with the *.a above: in Wine's tree they are libfoo.a
# beside every foo.dll, a third of the PE tree's file count.
rm -rf "$out"/usr/lib/wine/*/winetest* "$out"/opt/*/lib/wine/*/winetest* 2>/dev/null || true
rm -rf "$out"/opt/*/share/man "$out"/opt/*/share/doc "$out"/opt/*/share/applications 2>/dev/null || true
rm -rf "$out$wine/include" "$out$wine"/share/wine/gecko "$out$wine"/share/wine/mono

if [ "$profile" = player ]; then
  echo "==> pruning for the player"
  list="$rtsrc/player-prune.txt"
  [ -f "$list" ] || { echo "assemble-runtime: no $list" >&2; exit 1; }
  # Globs expand against the tree; one that matches nothing is not an error,
  # since the list names what might be there across Ubuntu releases.
  shopt -s nullglob
  removed=0
  while read -r pat; do
    pat="${pat%%#*}"; pat="${pat%"${pat##*[![:space:]]}"}"
    [ -n "$pat" ] || continue
    for m in "$out"/$pat; do
      rm -rf "$m"
      removed=$((removed + 1))
    done
  done < "$list"
  shopt -u nullglob
  echo "    $removed paths removed"

  if [ "${PLAYER_DLL_WHITELIST:-0}" = 1 ]; then
    echo "==> enforcing the DLL whitelist"
    keep="$(mktemp)"
    # No game recorded yet is the ordinary case, so the glob may match nothing.
    shopt -s nullglob
    lists=("$rtsrc/player-keep-dlls.txt" "$rtsrc"/loaddll/*.txt)
    shopt -u nullglob
    cat "${lists[@]}" | sed 's/#.*//; s/[[:space:]]//g' | tr 'A-Z' 'a-z' \
      | { grep -v '^$' || true; } | sort -u > "$keep"
    echo "    $(wc -l < "$keep") names kept"
    dropped=0
    for d in "$out$wine"/lib/wine/i386-windows "$out$wine"/lib/wine/x86_64-windows \
             "$out"/opt/kretro/prefix-template/drive_c/windows/system32 \
             "$out"/opt/kretro/prefix-template/drive_c/windows/syswow64; do
      [ -d "$d" ] || continue
      while IFS= read -r -d '' f; do
        b="$(basename "$f" | tr 'A-Z' 'a-z')"
        hit=0
        while read -r g; do
          # shellcheck disable=SC2254 - the list holds globs on purpose
          case "$b" in $g) hit=1; break ;; esac
        done < "$keep"
        [ "$hit" = 1 ] || { rm -f "$f"; dropped=$((dropped + 1)); }
      done < <(find "$d" -maxdepth 1 -iname '*.dll' -print0)
    done
    rm -f "$keep"
    echo "    $dropped DLLs removed"
  fi
fi

# Wine's built-in DLLs are PE files carrying DWARF debug sections, which is
# most of their size and of no use to anyone playing a game. objcopy
# understands PE, so the sections can come out without touching the code. The
# prefix template's copies of those DLLs go the same way. Nothing else under
# /opt is touched: DXVK and cnc-ddraw ship without debug sections, and objcopy
# would only rewrite their headers - cnc-ddraw is an MSVC build it has no
# business re-linking - so what ships would stop being the bytes the pins in
# Dockerfile.runtime verified.
echo "==> stripping debug sections from Wine's PE tree"
before=$(du -sm "$out"/opt 2>/dev/null | cut -f1 || echo 0)
find "$out$wine" "$out"/opt/kretro \( -name '*.dll' -o -name '*.exe' -o -name '*.drv' -o -name '*.ocx' \) \
     -type f 2>/dev/null \
  | while read -r f; do
      objcopy --strip-debug "$f" "$f.stripped" 2>/dev/null && mv "$f.stripped" "$f" \
        || rm -f "$f.stripped"
    done
after=$(du -sm "$out"/opt 2>/dev/null | cut -f1 || echo 0)
echo "    /opt: ${before} MB -> ${after} MB"

echo "==> establishing the bootstrap contract"
mkdir -p "$out/lib" "$out/bin"
loader="$(find "$out"/usr/lib "$out"/lib -name 'ld-linux-x86-64.so.2' -type f 2>/dev/null | head -1)"
[ -n "$loader" ] || { echo "no glibc loader found in the image" >&2; exit 1; }
# A real file, not a symlink into a path the bootstrap would have to guess.
cp -L "$loader" "$out/lib/ld-linux-x86-64.so.2"

echo "==> versions"
scanner="$(cd "$out" && find usr/lib usr/libexec -name gst-plugin-scanner -type f 2>/dev/null | head -1)"
{
  echo "profile  $profile"
  echo "built    $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "base     $(. /etc/os-release && echo "$PRETTY_NAME")"
  echo "glibc    $(/lib/x86_64-linux-gnu/libc.so.6 2>/dev/null | head -1 | sed 's/.*version //;s/\..$//' || echo '?')"
  echo "wine     $(wine --version 2>/dev/null || echo 'missing')"
  echo "prefix   $(cat /opt/kretro/prefix-template/.kretro-wine-version 2>/dev/null || echo 'missing')"
  echo "dxvk     $(ls -d /opt/dxvk-* 2>/dev/null | xargs -rn1 basename | tr '\n' ' ')(default $(readlink /opt/dxvk 2>/dev/null || echo none))"
  echo "cnc-ddraw $(cat /opt/cnc-ddraw/VERSION 2>/dev/null || echo 'missing')"
  echo "weston   $(weston --version 2>/dev/null || echo 'missing')"
  echo "xwayland $({ Xwayland -version 2>&1 || echo missing; } | head -1)"
  echo "gst-scanner ${scanner:-missing}"
} | tee "$out/RUNTIME"

echo "==> size"
du -sh "$out"
