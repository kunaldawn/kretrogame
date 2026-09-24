#!/usr/bin/env python3
"""Writes a v4 file for tests/integration/test_boot.sh, following the binary contract.

    [bootstrap ELF] [payloads, each on a 4096 boundary] [table] [trailer, 64]

This is deliberately the test's own writer, not kretro's: a bootstrap checked
only against the linker that was written beside it agrees with that linker,
which is not the same as agreeing with the contract. It can also write the
files a real linker never would - an entry past the end, a table missing its
runtime - which is most of what the test is for.

Prints "toc_off toc_len" so the test can check what the bootstrap handed down.

The contract it follows is docs/file-format.md.
"""

import argparse
import subprocess
import sys
from pathlib import Path

ALIGN = 4096
KINDS = {"tools": 1, "runtime": 2, "app": 3, "meta": 4, "pack": 5, "player-base": 6}


def b3(helper: Path, data: bytes) -> bytes:
    out = subprocess.run([str(helper)], input=data, capture_output=True, check=True).stdout
    return bytes.fromhex(out.decode().strip())


def record(kind: int, off: int, length: int, digest: bytes, name: str = "") -> bytes:
    r = bytearray(128)
    r[0:4] = kind.to_bytes(4, "little")
    r[4:8] = (0).to_bytes(4, "little")  # flags
    r[8:16] = off.to_bytes(8, "little")
    r[16:24] = length.to_bytes(8, "little")
    r[24:56] = digest
    n = name.encode()
    assert len(n) <= 64
    r[56 : 56 + len(n)] = n
    return bytes(r)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--b3", required=True, type=Path, help="the b3sum helper")
    ap.add_argument("--bootstrap", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument(
        "--part",
        action="append",
        default=[],
        help="KIND=FILE[:NAME], in file order; KIND is one of " + ", ".join(KINDS),
    )
    ap.add_argument(
        "--raw-entry",
        action="append",
        default=[],
        help="KIND:OFF:LEN, a table entry pointing wherever it says (for damage tests)",
    )
    ap.add_argument("--trailer-version", type=int, default=4)
    a = ap.parse_args()

    out = bytearray(a.bootstrap.read_bytes())
    entries = []
    for spec in a.part:
        kind, _, rest = spec.partition("=")
        path, _, name = rest.partition(":")
        data = Path(path).read_bytes()
        if len(out) % ALIGN:
            out += b"\0" * (ALIGN - len(out) % ALIGN)
        off = len(out)
        out += data
        entries.append(record(KINDS[kind], off, len(data), b3(a.b3, data), name))
    for spec in a.raw_entry:
        kind, off, length = (int(x) for x in spec.split(":"))
        entries.append(record(kind, off, length, b"\0" * 32))

    toc = bytearray(b"KTOC")
    toc += (1).to_bytes(4, "little")
    toc += len(entries).to_bytes(4, "little")
    toc += (0).to_bytes(4, "little")
    for e in entries:
        toc += e
    toc_off = len(out)
    out += toc

    trailer = bytearray(64)
    trailer[0:8] = b"KRETROv4"
    trailer[8:12] = a.trailer_version.to_bytes(4, "little")
    trailer[12:16] = (0).to_bytes(4, "little")
    trailer[16:24] = toc_off.to_bytes(8, "little")
    trailer[24:32] = len(toc).to_bytes(8, "little")
    trailer[32:64] = b3(a.b3, bytes(toc))
    out += trailer

    a.out.write_bytes(out)
    a.out.chmod(0o755)
    print(toc_off, len(toc))
    return 0


if __name__ == "__main__":
    sys.exit(main())
