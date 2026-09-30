#!/usr/bin/env python3
"""Unlocked-save generator: format checks, isolation, and the published saves.

The game-side round trip (unlock through GameDataHolder, the game's save
sequence, reload through SaveDataHandler/UserFile) runs in the app and is
recorded in each save's unlocked-save.json; these tests re-check those files
with an independent parser, so a published save that the game would reject
(checksum, header, chunk layout) or that lost stars fails here.
"""
import importlib.util
import json
import re
from pathlib import Path
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("unlocked", ROOT / "native/tools/make_unlocked_save.py")
unlocked = importlib.util.module_from_spec(spec)
spec.loader.exec_module(unlocked)

SEED = ROOT / "build/observatory-user-2" / unlocked.SAVE_RELATIVE
# Game data sizes in SaveDataHandler's layout order (mario, luigi, config per file; sysconf).
SPANS = [("mario", 0xF80), ("luigi", 0xF80), ("config", 0x60)]


def empty_save() -> bytearray:
    """A save as SaveDataHandler::resetSaveData lays it out, with a valid checksum."""
    names, offset = [], 0x140
    for index in range(1, 7):
        for name, size in SPANS:
            names.append((f"{name}{index}", offset))
            offset += size
    names.append(("sysconf", offset))
    size = offset + 0x80
    data = bytearray((size + 31) // 32 * 32)
    struct.pack_into("<3I", data, 4, 2, len(names), size)
    for index, (name, entry) in enumerate(names):
        struct.pack_into("<12sI", data, 16 + index * 16, name.encode(), entry)
    struct.pack_into("<I", data, 0, unlocked.check_sum(bytes(data[4:size])))
    return data


class FormatTests(unittest.TestCase):
    def check(self, data: bytes) -> dict:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "GameData.bin"
            path.write_bytes(bytes(data))
            return unlocked.inspect(path)

    def test_empty_layout_and_checksum(self):
        self.assertEqual(self.check(empty_save()), {"mario1": None, "luigi1": None})

    def test_rejects_corruption(self):
        data = empty_save()
        data[0x200] ^= 1
        with self.assertRaisesRegex(ValueError, "checksum"):
            self.check(data)
        data = empty_save()
        struct.pack_into("<I", data, 4, 3)
        with self.assertRaisesRegex(ValueError, "header"):
            self.check(data)

    @unittest.skipUnless(SEED.is_file(), "no seed save in this checkout")
    def test_seed_matches_game_checksum(self):
        summary = unlocked.inspect(SEED)
        self.assertEqual(summary["mario1"]["galaxies"], 42)


class IsolationTests(unittest.TestCase):
    def test_refuses_existing_output_and_missing_seed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            seed, output = root / "seed", root / "out"
            output.mkdir()
            with self.assertRaisesRegex(ValueError, "already exists"):
                unlocked.generate("all-missions", root, output, root / "app", root, 1)
            seed.mkdir()
            with self.assertRaisesRegex(ValueError, "no saved file"):
                unlocked.generate("all-missions", seed, root / "new", root / "app", root, 1)
            self.assertFalse((root / "new").exists())


class PublishedSaveTests(unittest.TestCase):
    def test_published_saves(self):
        checked = 0
        for variant in unlocked.VARIANTS:
            directory = ROOT / "build/saves" / variant
            if not directory.is_dir():
                continue
            checked += 1
            with self.subTest(variant=variant):
                manifest = json.loads((directory / "unlocked-save.json").read_text())
                self.assertEqual(manifest["variant"], variant)
                self.assertTrue(any("VERIFIED" in line for line in manifest["game_report"]))
                report = manifest["game_report"]
                self.assertFalse(any(" FAIL " in line for line in report))
                # Only the Grand Finale may be locked, and only without both endings.
                locked = [name for line in report for name in re.findall(r"(\S+) \d+/\d+ LOCKED", line)]
                self.assertEqual(locked, [] if variant == "grand-finale" else ["PeachCastleFinalGalaxy"])
                self.assertFalse((directory / unlocked.MARKER).exists())
                save = directory / unlocked.SAVE_RELATIVE
                self.assertEqual(unlocked.sha256(save), manifest["GameData.bin_sha256"])
                seed = Path(manifest["seed"]) / unlocked.SAVE_RELATIVE
                if seed.is_file() and unlocked.sha256(seed) == manifest["seed_sha256"]:
                    summary = unlocked.check_against_seed(variant, save, seed)
                else:
                    summary = unlocked.inspect(save)
                self.assertEqual(summary, manifest["inspect"])
                self.assertEqual(summary["mario1"]["star_bits"], 120)
                if variant == "grand-finale":
                    self.assertEqual(summary["luigi1"]["star_bits"], 120)
        if checked == 0:
            self.skipTest("no generated saves under build/saves")


if __name__ == "__main__":
    unittest.main()
