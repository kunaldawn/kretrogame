#!/usr/bin/env bash
# Opens both runtimes and checks what each one carries, then starts their Wine.
#
#   tests/integration/runtime.sh                 both
#   tests/integration/runtime.sh player          just one (kretro or player)
#
# A runtime is judged by its contents rather than by the script that made it:
# the image is unpacked with the DwarFS tool it will ship beside, and each
# claim the design makes about it - one Wine and no i386 libraries, two DXVKs,
# cnc-ddraw and never dgVoodoo, a prefix template stamped with its Wine, the
# licence notices, and for the player none of the authoring tools - is looked
# for in the files. Then Wine is started out of the unpacked tree the way the
# app starts it, through the runtime's own loader, from a directory the tree
# was never built in: 64-bit cmd, then 32-bit cmd through WoW64, in a copy of
# the template seeded as the app seeds it.
#
# Wants `make runtime` first, and docker for the Wine half. Needs about 3 GB of
# scratch space; set TMPDIR to move it.
set -uo pipefail
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"
BUILD="${BUILD:-build}"
TOOL="$BUILD/dwarfs-universal"

# What a check runs here says nothing worth reading; only its verdict counts.
CHECK_QUIET=1
absent() { local label="$1"; shift; local p; for p in "$@"; do
             if [ -e "$p" ] || [ -L "$p" ]; then bad "$label ($p is there)"; return; fi
           done; ok "$label"; }

[ -x "$TOOL" ] || skip "no $TOOL - run: make runtime"
only="${1:-}"
scratch runtime-test
# The extracted trees hold read-only directories, and the Wine half writes
# through docker as this user; both have to be made removable first.
scratch_cleanup() { chmod -R u+w "$SCRATCH" 2>/dev/null; }

one() {
  local profile="$1" image="$2"
  [ -z "$only" ] || [ "$only" = "$profile" ] || return 0
  if [ ! -f "$BUILD/$image" ]; then echo "skip: no $BUILD/$image"; return 0; fi
  echo "$profile runtime  ($(du -h "$BUILD/$image" | cut -f1) packed)"
  local rt="$SCRATCH/$profile"
  mkdir -p "$rt"
  if ! "$TOOL" --tool=dwarfsextract -i "$BUILD/$image" -o "$rt" --log-level=error 2>&1; then
    bad "unpacks"; return
  fi
  echo "        $(du -sh "$rt" | cut -f1) unpacked"
  local wine="$rt/opt/wine-staging"

  # The bootstrap contract and the one Wine.
  check "the loader is a real file" test -f "$rt/lib/ld-linux-x86-64.so.2" -a ! -L "$rt/lib/ld-linux-x86-64.so.2"
  check "Wine 11 is at opt/wine-staging" grep -q '^wine *wine-11\.' "$rt/RUNTIME"
  check "Wine has its 32-bit PE half" test -f "$wine/lib/wine/i386-windows/kernel32.dll"
  absent "no i386 Linux libraries (new WoW64)" "$rt/usr/lib/i386-linux-gnu" "$rt/lib/i386-linux-gnu" \
    "$wine/lib/wine/i386-unix"
  absent "no second Wine from Ubuntu" "$rt/usr/lib/x86_64-linux-gnu/wine" "$rt/usr/bin/wine"
  absent "no Wine headers, gecko or mono" "$wine/include" "$wine/share/wine/gecko" "$wine/share/wine/mono"
  if [ -z "$(find "$wine/lib/wine" -name '*.a' -print -quit)" ]; then ok "no import libraries"
  else bad "no import libraries"; fi

  # Graphics pieces.
  check "DXVK 3.x" test -f "$rt/opt/dxvk-3/x32/d3d9.dll" -a -f "$rt/opt/dxvk-3/x64/d3d9.dll"
  check "DXVK 2.7.x" test -f "$rt/opt/dxvk-2/x32/d3d9.dll" -a -f "$rt/opt/dxvk-2/x64/d3d9.dll"
  check "opt/dxvk is a relative link that resolves" \
    sh -c "l=\$(readlink '$rt/opt/dxvk') && [ \"\${l#/}\" = \"\$l\" ] && [ -f '$rt/opt/dxvk/x32/d3d11.dll' ]"
  check "cnc-ddraw" test -f "$rt/opt/cnc-ddraw/ddraw.dll" -a -f "$rt/opt/cnc-ddraw/ddraw.ini"
  # The pinned releases ship as released: nothing on the way into the runtime
  # (objcopy, above all) may have rewritten them.
  local d
  for d in dxvk-3 dxvk-2 cnc-ddraw; do
    check "$d is byte for byte the pinned release" \
      sh -c "cd '$rt/opt/$d' && [ -s SHA256SUMS ] && sha256sum --quiet -c SHA256SUMS"
  done
  if [ -z "$(find "$rt" -iname '*dgvoodoo*' -print -quit)" ]; then ok "never dgVoodoo"
  else bad "never dgVoodoo ($(find "$rt" -iname '*dgvoodoo*' | head -1))"; fi

  # The prefix template.
  local pfx="$rt/opt/kretro/prefix-template"
  check "prefix template is there" test -f "$pfx/system.reg" -a -f "$pfx/user.reg"
  check "its user is 'player'" test -d "$pfx/drive_c/users/player"
  check "it is stamped with the runtime's Wine" \
    sh -c "[ \"\$(cat '$pfx/.kretro-wine-version')\" = \"\$(sed -n 's/^wine *//p' '$rt/RUNTIME')\" ]"
  if [ -z "$(find "$pfx" -type l -lname '/*' -print -quit)" ]; then ok "no absolute links in it"
  else bad "no absolute links in it ($(find "$pfx" -type l -lname '/*' | head -1))"; fi
  check "c: is still there, relative" test "$(readlink "$pfx/dosdevices/c:")" = "../drive_c"
  check "audio is pulse then alsa" grep -q '"Audio"="pulse,alsa"' "$pfx/user.reg"
  check "winebus reads pads through SDL" grep -q '"Enable SDL"=dword:00000001' "$pfx/system.reg"

  # Sound, fonts, licences.
  check "the bundled ALSA configuration" grep -q 'type dmix' "$rt/usr/share/kretro/alsa/asound.conf"
  # Parsed by the runtime's own alsa-lib, with nothing but that file, every
  # name the default device reaches must resolve - dmix opens its card's timer
  # by name, and a config that forgot timer.hw fails on every machine. This
  # asks alsa-lib to expand the definitions without opening a device, so it
  # needs no sound card; it does need a python3 on the host to call it.
  if command -v python3 >/dev/null; then
    local alsa
    alsa="$(ALSA_CONFIG_PATH="$rt/usr/share/kretro/alsa/asound.conf" python3 - \
              "$rt/usr/lib/x86_64-linux-gnu/libasound.so.2" 2>&1 <<'EOF'
import ctypes, sys
a = ctypes.CDLL(sys.argv[1])
if a.snd_config_update() < 0: sys.exit("the file does not parse")
top = ctypes.c_void_p.in_dll(a, "snd_config")
bad = []
for kind, name in [("pcm", "default"), ("pcm", "dmix:CARD=0,DEV=0"), ("pcm", "plughw:0,0"),
                   ("ctl", "default"), ("ctl", "hw:0"),
                   ("timer", "hw:CLASS=3,SCLASS=0,CARD=0,DEV=0,SUBDEV=0")]:
    out = ctypes.c_void_p()
    if a.snd_config_search_definition(top, kind.encode(), name.encode(), ctypes.byref(out)) < 0:
        bad.append(kind + " " + name)
    else:
        a.snd_config_delete(out)
print("undefined: " + ", ".join(bad) if bad else "ok")
EOF
)"
    [ "$alsa" = ok ] && ok "ALSA resolves default, dmix and its timer" \
      || bad "ALSA resolves default, dmix and its timer ($alsa)"
  else
    echo "  skip: no python3 to ask alsa-lib"
  fi
  check "a font" test -f "$rt/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"
  local lic="$rt/usr/share/kretro/licenses" n missing=""
  for n in wine glibc gstreamer mesa weston xwayland dxvk cnc-ddraw fuse fuse-overlayfs sdl2 blake3 imgui dwarfs; do
    [ -n "$(ls -A "$lic/$n" 2>/dev/null)" ] || missing="$missing $n"
  done
  if [ -z "$missing" ]; then ok "licence notices for every named component"
  else bad "licence notices missing:$missing"; fi
  check "SOURCES.txt names Wine and the Ubuntu packages" \
    sh -c "grep -q 'wine-11' '$lic/SOURCES.txt' && grep -q 'launchpad.net/ubuntu/+source/glibc/' '$lic/SOURCES.txt'"

  # What each profile keeps.
  local tools=(usr/bin/7z usr/bin/unshield usr/bin/innoextract usr/bin/cabextract usr/bin/flac
               usr/bin/oggenc usr/bin/winetricks usr/bin/bsdtar usr/bin/objcopy)
  if [ "$profile" = player ]; then
    local paths=() t
    for t in "${tools[@]}"; do paths+=("$rt/$t"); done
    absent "none of the authoring tools" "${paths[@]}"
    absent "nor libarchive under them" "$rt/usr/lib/x86_64-linux-gnu/libarchive.so.13"
    absent "none of the wizard's manifests" "$rt/usr/share/kretro/games" "$rt/usr/share/kretro/discs.txt"
    absent "none of Wine's developer tools" "$wine/bin/winegcc" "$wine/bin/widl" "$wine/bin/winebuild"
    check "fuse-overlayfs stays" test -x "$rt/usr/bin/fuse-overlayfs"
  else
    local t missing_tools=""
    for t in usr/bin/7z usr/bin/unshield usr/bin/innoextract usr/bin/cabextract usr/bin/flac usr/bin/winetricks; do
      [ -e "$rt/$t" ] || missing_tools="$missing_tools $t"
    done
    if [ -z "$missing_tools" ]; then ok "the authoring tools"; else bad "authoring tools missing:$missing_tools"; fi
    # The manifests are local data: a runtime built where there is a games/
    # carries them, and one built without is just as valid.
    if [ -d games ]; then
      check "the wizard's manifests, from games/" test -d "$rt/usr/share/kretro/games"
    else
      note "no games/ here, so no manifests to look for"
    fi
  fi

  command -v docker >/dev/null || { echo "  skip: no docker for the Wine half"; return; }
  # The tree is mounted at /rt, a path it was never built at, read-only, as a
  # FUSE mount would present it. The environment is what src/rt/env.cpp sets,
  # plus the user name the template was built for. The container is the
  # runtime's own base, because Wine execs wineserver and itself directly and
  # those land on the host's /lib64 loader; that part of portability is
  # tests/integration/portability.sh's job, not this one's.
  local out
  # A Wine that cannot start tends to sit in winedbg rather than exit, so the
  # whole run has a deadline; when it passes, the container's only process
  # dies and takes Wine with it.
  out="$(docker run --rm -u "$(id -u):$(id -g)" -v "$rt:/rt:ro" ubuntu:26.04 timeout -k 10 300 sh -c '
    set -e
    R=/rt W=/rt/opt/wine-staging
    export LD_LIBRARY_PATH=$R/lib:$R/lib/x86_64-linux-gnu:$R/usr/lib/x86_64-linux-gnu:$R/usr/lib
    export PATH=$R/usr/bin:$R/bin:/usr/bin:/bin
    export WINELOADER=$W/bin/wine WINESERVER=$W/bin/wineserver WINEDLLPATH=$W/lib/wine
    export FONTCONFIG_PATH=$R/etc/fonts XDG_DATA_DIRS=$R/usr/share
    export ALSA_CONFIG_PATH=$R/usr/share/kretro/alsa/asound.conf
    export WINEDEBUG=-all WINEUSERNAME=player USER=player HOME=/tmp/home
    mkdir -p "$HOME"
    run() { "$R/lib/ld-linux-x86-64.so.2" "$@"; }
    echo "VERSION $(run $W/bin/wine --version)"
    # Seeded as the app seeds it: a writable copy, dosdevices recreated.
    export WINEPREFIX=/tmp/pfx
    cp -r $R/opt/kretro/prefix-template $WINEPREFIX
    chmod -R u+w $WINEPREFIX
    echo "ARCH64 $(run $W/bin/wine cmd /c echo %PROCESSOR_ARCHITECTURE% 2>&1 | tr -d "\r" | tail -1)"
    echo "ARCH32 $(run $W/bin/wine "C:\\windows\\syswow64\\cmd.exe" /c echo %PROCESSOR_ARCHITECTURE% 2>&1 | tr -d "\r" | tail -1)"
    echo "VER $(run $W/bin/wine cmd /c ver 2>&1 | tr -d "\r" | grep -i windows | tail -1)"
    echo "PROFILES $(ls $WINEPREFIX/drive_c/users | tr "\n" " ")"
    run $W/bin/wineserver -k 2>/dev/null || true
  ' 2>&1)"
  local v a64 a32 ver prof
  v="$(printf '%s\n' "$out" | sed -n 's/^VERSION //p')"
  a64="$(printf '%s\n' "$out" | sed -n 's/^ARCH64 //p')"
  a32="$(printf '%s\n' "$out" | sed -n 's/^ARCH32 //p')"
  ver="$(printf '%s\n' "$out" | sed -n 's/^VER //p')"
  prof="$(printf '%s\n' "$out" | sed -n 's/^PROFILES //p')"
  case "$v" in wine-11.*) ok "wine --version: $v" ;; *) bad "wine --version"; printf '%s\n' "$out" | head -8 | sed 's/^/        /' ;; esac
  [ "$a64" = AMD64 ] && ok "64-bit cmd runs ($a64)" || bad "64-bit cmd runs (got '$a64')"
  [ "$a32" = x86 ] && ok "32-bit cmd runs through WoW64 ($a32)" || bad "32-bit cmd runs through WoW64 (got '$a32')"
  [ -n "$ver" ] && ok "cmd /c ver: $ver" || bad "cmd /c ver"
  case "$prof" in *player*) ;; *) prof="" ;; esac
  [ -n "$prof" ] && [ "$(echo $prof | wc -w)" -le 2 ] && ok "one user profile: $prof" \
    || bad "one user profile (got '$(printf '%s\n' "$out" | sed -n 's/^PROFILES //p')')"

  # And with no glibc anywhere on the system, the version at least - what the
  # bootstrap does before anything else on a musl host.
  v="$(docker run --rm -v "$rt:/rt:ro" alpine:latest sh -c '
    R=/rt W=/rt/opt/wine-staging
    export LD_LIBRARY_PATH=$R/lib:$R/lib/x86_64-linux-gnu:$R/usr/lib/x86_64-linux-gnu
    export WINELOADER=$W/bin/wine WINEDLLPATH=$W/lib/wine
    $R/lib/ld-linux-x86-64.so.2 $W/bin/wine --version' 2>&1 | tail -1)"
  case "$v" in wine-11.*) ok "wine --version on musl: $v" ;; *) bad "wine --version on musl ($v)" ;; esac
}

one kretro runtime.dwarfs
one player player-runtime.dwarfs

finish
