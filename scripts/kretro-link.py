#!/usr/bin/env python3
"""Assemble a kretro file: kretro itself, or the player base it carries.

    [ bootstrap ELF        ]  static musl, knows nothing but how to mount
    [ dwarfs-universal     ]  kind 1: 2.6 MB static, the only tool it needs
    [ runtime DwarFS image ]  kind 2: everything else: glibc, Wine, Mesa...
    [ the app              ]  kind 3: kretro, or the player; a few MB, so an
                              app change relinks in seconds
    [ player base          ]  kind 6, kretro only: the whole player-base file,
                              so building a player needs no network and no
                              source tree
    [ table of contents    ]  16-byte header, one 128-byte record per payload
    [ trailer, 64 bytes    ]  "KRETROv4", where the table is, and its BLAKE3

Every payload starts on a 4096-byte page, so the runtime is mounted straight
out of this file at an offset and nothing the size of the runtime is ever
copied. The same program links both files: kretro is linked with
--player-base, the player base without it. A player is the player base with
bundle.meta and games appended; kretro builds those (src/bundle/build.cpp), and
src/bundle/toc.h is the definition of the format both writers follow.

The table records a BLAKE3 for every payload. Python has no BLAKE3, so the
hashes come from kretro-b3 (scripts/kretro-b3.c, built by the Makefile), or from
the blake3 module when one is installed.
"""

import argparse
import os
import struct
import subprocess
import sys
from pathlib import Path

TRAILER_MAGIC = b"KRETROv4"
TRAILER_VERSION = 4
TRAILER_SIZE = 64
TOC_MAGIC = b"KTOC"
TOC_VERSION = 1
RECORD_SIZE = 128
NAME_SIZE = 64
ALIGN = 4096  # DwarFS wants its image on a page to mount in place

TOOLS, RUNTIME, APP, META, PACK, PLAYER_BASE = 1, 2, 3, 4, 5, 6


class Hasher:
    """BLAKE3 of whole files, and of the table's bytes."""

    def __init__(self, b3: Path | None):
        # Absolute, because a bare "kretro-b3" handed to subprocess is looked
        # up on PATH, not in the directory it was named from.
        self.b3 = b3.absolute() if b3 else None
        self.module = None
        if b3 is None:
            try:
                import blake3  # type: ignore

                self.module = blake3
            except ImportError:
                raise SystemExit(
                    "kretro-link: no BLAKE3: pass --b3 build/kretro-b3, or install the blake3 module"
                )
        elif not os.access(b3, os.X_OK):
            raise SystemExit(f"kretro-link: {b3} is not an executable kretro-b3")

    def _run(self, args: list[str], data: bytes | None = None) -> list[bytes]:
        r = subprocess.run([str(self.b3), *args], input=data, capture_output=True, check=False)
        if r.returncode != 0:
            raise SystemExit(f"kretro-link: kretro-b3 failed: {r.stderr.decode(errors='replace').strip()}")
        return [bytes.fromhex(line.split()[0]) for line in r.stdout.decode().splitlines()]

    def files(self, paths: list[Path]) -> list[bytes]:
        if self.module:
            out = []
            for p in paths:
                h = self.module.blake3()
                with open(p, "rb") as f:
                    while chunk := f.read(1 << 20):
                        h.update(chunk)
                out.append(h.digest())
            return out
        return self._run([str(p) for p in paths])

    def data(self, b: bytes) -> bytes:
        if self.module:
            return self.module.blake3(b).digest()
        return self._run(["-"], b)[0]


def pad_to(fh, alignment: int) -> int:
    pos = fh.tell()
    if pos % alignment:
        fh.write(b"\0" * (alignment - pos % alignment))
    return fh.tell()


def record(kind: int, off: int, length: int, digest: bytes, name: str = "") -> bytes:
    n = name.encode()
    if len(n) > NAME_SIZE:
        raise SystemExit(f"kretro-link: the name {name!r} does not fit the table's {NAME_SIZE} bytes")
    return (
        struct.pack("<IIQQ", kind, 0, off, length)
        + digest
        + n.ljust(NAME_SIZE, b"\0")
        + b"\0" * 8
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bootstrap", required=True, type=Path)
    ap.add_argument("--tools", required=True, type=Path, help="static dwarfs binary")
    ap.add_argument("--image", required=True, type=Path, help="runtime DwarFS image")
    ap.add_argument("--app", required=True, type=Path, help="kretro, or the player app")
    ap.add_argument("--player-base", type=Path, help="a linked player base; kretro carries one")
    ap.add_argument("--b3", type=Path, help="the kretro-b3 BLAKE3 tool")
    ap.add_argument("--out", required=True, type=Path)
    a = ap.parse_args()

    parts = [(TOOLS, a.tools), (RUNTIME, a.image), (APP, a.app)]
    if a.player_base:
        parts.append((PLAYER_BASE, a.player_base))
    for p in [a.bootstrap] + [p for _, p in parts]:
        if not p.is_file():
            print(f"kretro-link: missing {p}", file=sys.stderr)
            return 1
    if a.player_base:
        # Carried byte for byte and never looked inside again until a build, so
        # the one check worth making is made now: it is a v4 file at all.
        # A file shorter than a trailer is not one either, and seeking back past
        # its start would be a traceback rather than this message.
        with open(a.player_base, "rb") as f:
            magic = b""
            if a.player_base.stat().st_size >= TRAILER_SIZE:
                f.seek(-TRAILER_SIZE, os.SEEK_END)
                magic = f.read(8)
            if magic != TRAILER_MAGIC:
                print(f"kretro-link: {a.player_base} is not a linked v4 player base", file=sys.stderr)
                return 1

    hasher = Hasher(a.b3)
    digests = hasher.files([p for _, p in parts])

    a.out.parent.mkdir(parents=True, exist_ok=True)
    partial = a.out.with_name(a.out.name + ".partial")
    records = []
    sizes = {}
    # Nothing half-linked is left looking like a file: a .partial that failed
    # partway - a kretro-b3 that died, a full disk, an input that changed - is
    # removed, and only a finished one is renamed into place.
    try:
        with open(partial, "wb") as out:
            with open(a.bootstrap, "rb") as f:
                while chunk := f.read(1 << 20):
                    out.write(chunk)

            for (kind, path), digest in zip(parts, digests):
                off = pad_to(out, ALIGN)
                with open(path, "rb") as f:
                    while chunk := f.read(1 << 20):
                        out.write(chunk)
                length = out.tell() - off
                # The hash was of the file as it was a moment ago; a file that
                # changed size since is a file that changed, and the table would
                # describe bytes that are not the ones copied.
                if length != path.stat().st_size:
                    print(f"kretro-link: {path} changed while it was being linked", file=sys.stderr)
                    partial.unlink(missing_ok=True)
                    return 1
                records.append(record(kind, off, length, digest))
                sizes[kind] = (off, length)

            toc = TOC_MAGIC + struct.pack("<III", TOC_VERSION, len(records), 0) + b"".join(records)
            toc_off = out.tell()
            out.write(toc)
            out.write(
                TRAILER_MAGIC
                + struct.pack("<IIQQ", TRAILER_VERSION, 0, toc_off, len(toc))
                + hasher.data(toc)
            )

        partial.chmod(0o755)
        partial.replace(a.out)
    except BaseException:
        partial.unlink(missing_ok=True)
        raise
    total = a.out.stat().st_size
    summary = [f"bootstrap {sizes[TOOLS][0] / 1e3:.0f} kB"]
    names = {TOOLS: "tools", RUNTIME: "runtime", APP: "app", PLAYER_BASE: "player base"}
    for kind, (off, length) in sizes.items():
        summary.append(f"{names[kind]} {length / 1e6:.1f} MB at +{off}")
    print(f"  linked {a.out}: {total / 1e6:.1f} MB ({', '.join(summary)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
