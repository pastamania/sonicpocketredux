#!/usr/bin/env python3
"""Check that built files hold no stretch of a cartridge image.

Run it on everything that is going to be handed out:

    python tools/check_rom_free.py --rom cartridge.ngc build/player/sonic-pocket.exe build/player/SDL3.dll

It looks for any run of MIN_RUN or more bytes (32 by default) that appears in the cartridge
and in the file. Runs with nothing in them are ignored, because any program has them: padding
and fill (0xFF, zeroes), and regular tables such as counters (00 00 00 04, 00 00 00 05, ...),
ramps (0, 1, 2, ...) and the alphabet spread over 16 bits. What counts is MIN_ENTROPY bits a
byte and a pattern that is not just steps repeated. It exits 0 when nothing is found, 1 when something is (each match is printed
with its offsets), 2 when it cannot run.

This finds copied bytes: code, tables, graphics or sound pasted in as data. Code translated
from the game's holds none of its bytes, so a clean result for the recompiled build says
nothing; this is for the build that holds no game at all (SPA_PLAYER).
"""

from __future__ import annotations

import argparse
import math
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path


GRAM = 8          # bytes in the index that finds candidates
MIN_RUN = 32      # bytes in a match that is reported
MIN_DISTINCT = 4  # distinct byte values a window of a match needs to count
MIN_ENTROPY = 1.0 # bits a byte (Shannon) a window of a match needs to count


@dataclass(frozen=True)
class Match:
    offset: int       # in the file that was searched
    rom_offset: int   # in the cartridge
    length: int


def _entropy(window: bytes) -> float:
    counts = Counter(window)
    total = len(window)
    return -sum(n / total * math.log2(n / total) for n in counts.values())


def _interesting(window: bytes) -> bool:
    """False for a window that any program could hold: fill, counters, ramps."""
    if len(set(window)) < MIN_DISTINCT or _entropy(window) < MIN_ENTROPY:
        return False
    return not _regular(window)


def _regular(window: bytes) -> bool:
    """Whether the window is a ramp or a constant, once split into 1, 2 or 4 interleaved bytes.

    Counters (00 00 00 04, 00 00 00 05, ...) and letters spread over 16 bits (00 41 00 42, ...)
    are such: each byte lane goes up by one step or not at all. Code, text and graphics are not."""
    for stride in (1, 2, 4):
        lanes = (window[k::stride] for k in range(stride))
        if all(len({(b - a) & 0xFF for a, b in zip(lane, lane[1:])}) <= 2 for lane in lanes):
            return True
    return False


def build_index(rom: bytes) -> set[bytes]:
    """Every GRAM-byte string of the cartridge. It only finds where to look; _verify decides."""
    return {rom[i : i + GRAM] for i in range(len(rom) - GRAM + 1)}


def find_matches(rom: bytes, index: set[bytes], data: bytes, min_run: int = MIN_RUN) -> list[Match]:
    """Runs of at least min_run bytes that are in both the cartridge and data."""
    if min_run < GRAM:
        raise ValueError(f"min_run must be at least {GRAM}")
    matches: list[Match] = []
    needed = min_run - GRAM + 1   # consecutive hits that span min_run bytes
    hits = 0                      # consecutive offsets whose GRAM bytes are in the cartridge
    end = len(data) - GRAM + 1
    for i in range(end):
        if data[i : i + GRAM] in index:
            hits += 1
            continue
        if hits >= needed:
            matches.extend(_verify(rom, data, i - hits, i - 1 + GRAM, min_run))
        hits = 0
    if hits >= needed:
        matches.extend(_verify(rom, data, end - hits, len(data), min_run))
    return matches


def _verify(rom: bytes, data: bytes, start: int, stop: int, min_run: int) -> list[Match]:
    """Confirms that data[start:stop] really is one stretch of the cartridge, and measures it.

    The index only says each short piece is somewhere in the cartridge. Here min_run bytes
    in a row have to be found in it, and then the match is extended as far as both agree."""
    found: list[Match] = []
    position = start
    while position + min_run <= stop:
        window = data[position : position + min_run]
        at = rom.find(window) if _interesting(window) else -1
        if at < 0:
            position += 1
            continue
        begin = position
        while begin > 0 and at > 0 and data[begin - 1] == rom[at - 1]:
            begin -= 1
            at -= 1
        length = position - begin + min_run
        while begin + length < len(data) and at + length < len(rom) and data[begin + length] == rom[at + length]:
            length += 1
        found.append(Match(begin, at, length))
        position = begin + length
    return found


def expand(paths: list[str]) -> list[Path]:
    files: list[Path] = []
    for name in paths:
        path = Path(name)
        if path.is_dir():
            files.extend(sorted(p for p in path.rglob("*") if p.is_file()))
        else:
            files.append(path)
    return files


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--rom", required=True, type=Path, help="the cartridge image to look for")
    parser.add_argument("--min-run", type=int, default=MIN_RUN, help=f"bytes in a reported match (default {MIN_RUN})")
    parser.add_argument("paths", nargs="+", help="files, or folders of files, to check")
    args = parser.parse_args(argv)

    if args.min_run < GRAM:
        print(f"--min-run must be at least {GRAM}", file=sys.stderr)
        return 2
    try:
        rom = args.rom.read_bytes()
    except OSError as error:
        print(f"cannot read the cartridge: {error}", file=sys.stderr)
        return 2
    if len(rom) < args.min_run:
        print("the cartridge is smaller than a match", file=sys.stderr)
        return 2

    index = build_index(rom)
    dirty = False
    for path in expand(args.paths):
        try:
            data = path.read_bytes()
        except OSError as error:
            print(f"cannot read {path}: {error}", file=sys.stderr)
            return 2
        matches = find_matches(rom, index, data, args.min_run)
        if matches:
            dirty = True
            print(f"FOUND  {path}: {len(matches)} stretch(es) of the cartridge")
            for match in matches[:10]:
                print(f"         {match.length} bytes at file offset 0x{match.offset:x}, cartridge offset 0x{match.rom_offset:x}")
            if len(matches) > 10:
                print(f"         ... and {len(matches) - 10} more")
        else:
            print(f"clean  {path} ({len(data)} bytes)")
    return 1 if dirty else 0


if __name__ == "__main__":
    sys.exit(main())
