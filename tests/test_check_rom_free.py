from __future__ import annotations

import random
import sys
import tempfile
import unittest
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import check_rom_free  # noqa: E402


def make_rom() -> bytes:
    rng = random.Random(1234)
    return bytes(rng.randrange(256) for _ in range(20000))


def noise(length: int, seed: int) -> bytes:
    rng = random.Random(seed)
    return bytes(rng.randrange(256) for _ in range(length))


class CheckRomFreeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.rom = make_rom()
        self.index = check_rom_free.build_index(self.rom)

    def find(self, data: bytes, min_run: int = check_rom_free.MIN_RUN):
        return check_rom_free.find_matches(self.rom, self.index, data, min_run)

    def test_unrelated_data_is_clean(self) -> None:
        self.assertEqual(self.find(noise(50000, 99)), [])

    def test_copied_stretch_is_found_with_both_offsets(self) -> None:
        data = noise(3000, 1) + self.rom[5000:5200] + noise(3000, 2)
        matches = self.find(data)
        self.assertEqual(len(matches), 1)
        self.assertEqual(matches[0].offset, 3000)
        self.assertEqual(matches[0].rom_offset, 5000)
        self.assertEqual(matches[0].length, 200)

    def test_stretch_at_the_very_start_and_end(self) -> None:
        self.assertEqual([m.length for m in self.find(self.rom[100:160])], [60])
        self.assertEqual([m.length for m in self.find(noise(500, 3) + self.rom[900:960])], [60])

    def test_short_copy_is_ignored(self) -> None:
        data = noise(1000, 4) + self.rom[700:720] + noise(1000, 5)
        self.assertEqual(self.find(data), [])

    def test_two_copies_are_two_matches(self) -> None:
        data = self.rom[10:90] + noise(500, 6) + self.rom[4000:4100]
        self.assertEqual([(m.rom_offset, m.length) for m in self.find(data)], [(10, 80), (4000, 100)])

    def test_pieces_from_different_places_are_not_a_match(self) -> None:
        # Every 8-byte piece is in the cartridge, but the pieces are not one stretch of it.
        pieces = b"".join(self.rom[i * 997 : i * 997 + 12] for i in range(1, 12))
        self.assertEqual(self.find(pieces), [])

    def test_padding_is_ignored(self) -> None:
        rom = self.rom + b"\xff" * 4096 + bytes(4096)
        index = check_rom_free.build_index(rom)
        data = noise(100, 7) + b"\xff" * 2000 + bytes(2000)
        self.assertEqual(check_rom_free.find_matches(rom, index, data), [])

    def test_low_variety_stretch_is_ignored(self) -> None:
        rom = self.rom + bytes([1, 2, 1, 2, 3, 3, 1, 2]) * 20
        index = check_rom_free.build_index(rom)
        data = noise(100, 8) + bytes([1, 2, 1, 2, 3, 3, 1, 2]) * 20
        self.assertEqual(check_rom_free.find_matches(rom, index, data), [])

    def test_counter_tables_are_ignored(self) -> None:
        # 00 00 00 NN, as a table of 32-bit indices: in the cartridge and in any program.
        table = b"".join(n.to_bytes(4, "big") for n in range(4, 40))
        rom = self.rom + table
        index = check_rom_free.build_index(rom)
        data = noise(100, 9) + table + noise(100, 10)
        self.assertEqual(check_rom_free.find_matches(rom, index, data), [])

    def test_ramps_are_ignored(self) -> None:
        for step in (1, 2, 16):
            ramp = bytes((n * step) & 0xFF for n in range(64))
            rom = self.rom + ramp
            index = check_rom_free.build_index(rom)
            self.assertEqual(check_rom_free.find_matches(rom, index, noise(50, 13) + ramp), [], step)

    def test_letters_spread_over_16_bits_are_ignored(self) -> None:
        letters = b"".join(bytes([0, c]) for c in range(0x41, 0x5B))
        rom = self.rom + letters
        index = check_rom_free.build_index(rom)
        self.assertEqual(check_rom_free.find_matches(rom, index, noise(50, 14) + letters + noise(50, 15)), [])

    def test_real_stretch_beside_a_table_is_still_found(self) -> None:
        table = b"".join(n.to_bytes(4, "big") for n in range(4, 40))
        rom = self.rom + table
        index = check_rom_free.build_index(rom)
        data = table + self.rom[2000:2150]
        self.assertEqual([(m.rom_offset, m.length) for m in check_rom_free.find_matches(rom, index, data)],
                         [(2000, 150)])

    def test_min_run_below_the_index_size_is_refused(self) -> None:
        with self.assertRaises(ValueError):
            self.find(b"x" * 100, min_run=4)

    def test_command_line_exit_codes(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            rom = root / "cart.ngc"
            clean = root / "clean.bin"
            dirty = root / "dirty.bin"
            rom.write_bytes(self.rom)
            clean.write_bytes(noise(4000, 11))
            dirty.write_bytes(noise(100, 12) + self.rom[300:400])
            self.assertEqual(check_rom_free.main(["--rom", str(rom), str(clean)]), 0)
            self.assertEqual(check_rom_free.main(["--rom", str(rom), str(clean), str(dirty)]), 1)
            self.assertEqual(check_rom_free.main(["--rom", str(rom), str(root / "missing")]), 2)
            self.assertEqual(check_rom_free.main(["--rom", str(root / "missing"), str(clean)]), 2)


if __name__ == "__main__":
    unittest.main()
