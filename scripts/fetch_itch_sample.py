#!/usr/bin/env python3
"""Fetch a prefix of a real Nasdaq TotalView-ITCH 5.0 sample file.

Why a prefix, and why not committed
-----------------------------------
Nasdaq publishes full-day sample files. One day of 30 January 2019 is 4.76 GB
compressed. That cannot live in a repository that is meant to stay under 10 MB,
and it is Nasdaq's data to distribute rather than this project's, so nothing
fetched here is committed. This script is the download step: run it when you
want to check the decoder against real bytes.

The archive supports HTTP range requests, so this pulls only the first few MB
and decompresses the gzip prefix. That is enough for several hundred thousand
real messages covering every book-affecting type.

Source (verified reachable 2026-09-16):
  https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/

Note the historical FTP endpoint named in older documentation,
ftp://emi.nasdaq.com/ITCH/, no longer accepts connections (curl exit 7). The
HTTPS path above is the working location.

Usage:
  python3 scripts/fetch_itch_sample.py [--mb 4] [--out data/itch_sample.bin]
"""
from __future__ import annotations

import argparse
import hashlib
import pathlib
import sys
import urllib.request
import zlib

BASE = "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/"
DEFAULT_FILE = "01302019.NASDAQ_ITCH50.gz"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--file", default=DEFAULT_FILE, help="sample file name in the archive")
    ap.add_argument("--mb", type=int, default=4, help="compressed megabytes to fetch")
    ap.add_argument("--out", default="data/itch_sample.bin",
                    help="where to write the decompressed prefix")
    args = ap.parse_args()

    url = BASE + args.file
    nbytes = args.mb * 1024 * 1024
    print(f"fetching first {args.mb} MB of {url}")

    req = urllib.request.Request(url, headers={"Range": f"bytes=0-{nbytes - 1}"})
    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            if resp.status not in (200, 206):
                print(f"error: unexpected status {resp.status}", file=sys.stderr)
                return 1
            compressed = resp.read()
    except OSError as exc:
        print(f"error: fetch failed: {exc}", file=sys.stderr)
        print("The archive may have moved. Check https://emi.nasdaq.com/ITCH/",
              file=sys.stderr)
        return 1

    print(f"  got {len(compressed):,} compressed bytes")

    # A gzip prefix decompresses cleanly up to the truncation point; the
    # trailing error is expected and not a failure.
    dec = zlib.decompressobj(16 + zlib.MAX_WBITS)
    try:
        raw = dec.decompress(compressed)
    except zlib.error as exc:
        print(f"error: decompression failed: {exc}", file=sys.stderr)
        return 1

    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(raw)

    digest = hashlib.sha256(raw).hexdigest()
    print(f"  wrote {out} ({len(raw):,} bytes)")
    print(f"  sha256 {digest}")
    print()
    print("This file is NOT committed: it is Nasdaq's data, and the full source")
    print("is 4.76 GB. Replay it with:")
    print(f"  build/tools/itch50_replay {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
