#!/usr/bin/env python3
# The v3 linker exactly as it shipped before v4, kept so tests/integration/test_boot.sh can
# build the binaries people already have and prove the bootstrap still runs them.
"""Assemble the single kretro binary.

    [ bootstrap ELF        ]  static musl, knows nothing but how to mount
    [ dwarfs-universal     ]  2.6 MB static, the only thing ever unpacked
    [ runtime DwarFS image ]  everything else: glibc, Wine, Mesa, Weston...
    [ kretro itself        ]  a few MB, so an app change relinks in seconds
    [ trailer, 176 bytes   ]  where the payloads are (four slots; the fourth
                              is a game, and is empty until an export fills it)

The runtime is mounted straight out of this file at an offset, so nothing the
size of the runtime is ever copied.
"""

import argparse
import hashlib
import shutil
import sys
from pathlib import Path

MAGIC = b"KRETROv3"
VERSION = 3
TRAILER_SIZE = 176
ALIGN = 4096  # DwarFS wants its image aligned to mount in place


def blake3_or_sha256(data: bytes) -> bytes:
    """The trailer hashes only name cache directories; they are not a security
    boundary, so a hash the standard library already has is enough."""
    return hashlib.sha256(data).digest()


def pad_to(fh, alignment: int) -> int:
    pos = fh.tell()
    if pos % alignment:
        fh.write(b"\0" * (alignment - pos % alignment))
    return fh.tell()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bootstrap", required=True, type=Path)
    ap.add_argument("--tools", required=True, type=Path, help="static dwarfs binary")
    ap.add_argument("--image", required=True, type=Path, help="runtime DwarFS image")
    ap.add_argument("--app", required=True, type=Path, help="the kretro binary")
    ap.add_argument("--out", required=True, type=Path)
    a = ap.parse_args()

    for p in (a.bootstrap, a.tools, a.image, a.app):
        if not p.is_file():
            print(f"mkv3: missing {p}", file=sys.stderr)
            return 1

    tools = a.tools.read_bytes()
    a.out.parent.mkdir(parents=True, exist_ok=True)

    with open(a.out, "wb") as out:
        with open(a.bootstrap, "rb") as f:
            shutil.copyfileobj(f, out)

        tools_off = pad_to(out, ALIGN)
        out.write(tools)
        tools_len = len(tools)

        image_off = pad_to(out, ALIGN)
        image_hash = hashlib.sha256()
        image_len = 0
        with open(a.image, "rb") as f:
            while chunk := f.read(1 << 20):
                out.write(chunk)
                image_hash.update(chunk)
                image_len += len(chunk)

        app = a.app.read_bytes()
        app_off = pad_to(out, ALIGN)
        out.write(app)

        trailer = bytearray(TRAILER_SIZE)
        trailer[0:8] = MAGIC
        trailer[8:12] = VERSION.to_bytes(4, "little")
        trailer[12:16] = (0).to_bytes(4, "little")  # flags
        trailer[16:24] = tools_off.to_bytes(8, "little")
        trailer[24:32] = tools_len.to_bytes(8, "little")
        trailer[32:64] = blake3_or_sha256(tools)
        trailer[64:72] = image_off.to_bytes(8, "little")
        trailer[72:80] = image_len.to_bytes(8, "little")
        trailer[80:112] = image_hash.digest()
        trailer[112:120] = app_off.to_bytes(8, "little")
        trailer[120:128] = len(app).to_bytes(8, "little")
        trailer[128:160] = blake3_or_sha256(app)
        # Slot four: a game carried inside the binary. Empty here; filled by
        # `kretro export --standalone`, which rewrote this trailer until the
        # one-game player replaced it.
        trailer[160:168] = (0).to_bytes(8, "little")
        trailer[168:176] = (0).to_bytes(8, "little")
        out.write(trailer)

    a.out.chmod(0o755)
    total = a.out.stat().st_size
    print(
        f"  linked {a.out}: {total / 1e6:.1f} MB "
        f"(bootstrap {tools_off / 1e3:.0f} kB, tools {tools_len / 1e6:.1f} MB, "
        f"runtime {image_len / 1e6:.1f} MB at +{image_off}, "
        f"app {len(app) / 1e6:.1f} MB)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
