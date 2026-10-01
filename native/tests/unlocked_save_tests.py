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
STAGE_DATA = ROOT / "build/game-data/RMGE01/files/StageData"
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


class GalaxyTicoFedTests(unittest.TestCase):
    """Toy Time's comet started Mario in empty space: the save held Toy Time's
    star but not its Hungry Luma's feeding, so CrossRingZone (SW_SLEEP 1005, the
    TicoFat's SW_A) never woke. Both tables must list every in-galaxy TicoFat."""

    def test_cpp_table_matches_python(self):
        source = (ROOT / "src/Game/System/NativeUnlockedSave.cpp").read_text()
        table = {int(seed): int(num) for _, seed, num in re.findall(r'\{"(\w+)", (\d+), (\d+), \{', source)}
        self.assertEqual(table, unlocked.GALAXY_TICO_FED)

    @unittest.skipUnless(STAGE_DATA.is_dir(), "no extracted disc in this checkout")
    def test_table_lists_every_tico_fat_on_the_disc(self):
        import sys
        sys.path.insert(0, str(ROOT / "native/tools"))
        from collect_pipeline_seed_inputs import field_hash
        from stage_sweep import archive_tree, bcsv_rows
        name, arg1, arg7 = field_hash("name"), field_hash("Obj_arg1"), field_hash("Obj_arg7")
        found = {}
        for archive in sorted(STAGE_DATA.glob("*.arc")):
            for path, data in archive_tree(archive.read_bytes()):
                if not path.endswith("objinfo"):
                    continue
                for row in bcsv_rows(data):
                    if row.get(name) == "TicoFat" and row.get(arg7, -1) >= 0:
                        found[row[arg7]] = row[arg1]
        self.assertEqual(found, unlocked.GALAXY_TICO_FED)

    def test_unfed_seed_is_rejected(self):
        seeds = [0] * 8 + [400, 400, 600, 1200, 1600, 1000, 800, 0]
        self.assertEqual(unlocked.unfed_tico_seeds({"tico_seeds": seeds}), sorted(unlocked.GALAXY_TICO_FED))
        for seed, wanted in unlocked.GALAXY_TICO_FED.items():
            seeds[seed] = wanted
        self.assertEqual(unlocked.unfed_tico_seeds({"tico_seeds": seeds}), [])


class FeedGalaxyLumasTests(unittest.TestCase):
    """--feed-galaxy-lumas may only raise in-galaxy Luma counts to what feeding stores."""

    def setUp(self):
        self.save = next((ROOT / "build/saves" / v / unlocked.SAVE_RELATIVE for v in unlocked.VARIANTS
                          if (ROOT / "build/saves" / v / unlocked.SAVE_RELATIVE).is_file()), None)
        if self.save is None:
            self.skipTest("no generated saves under build/saves")
        self.before = self.unfed(self.save.read_bytes())

    @staticmethod
    def entry_offset(data: bytes, name: str) -> int:
        for index in range(unlocked.FILE_ENTRIES):
            entry, offset = struct.unpack_from("<12sI", data, 16 + index * 16)
            if entry.split(b"\0", 1)[0].decode() == name:
                return offset
        raise KeyError(name)

    def edit(self, data: bytes, position: int, value: int) -> bytes:
        data = bytearray(data)
        struct.pack_into("<H", data, position, value)
        size = struct.unpack_from("<I", data, 12)[0]
        struct.pack_into("<I", data, 0, unlocked.check_sum(bytes(data[4:size])))
        return bytes(data)

    def seed_position(self, data: bytes, seed: int) -> int:
        offset = self.entry_offset(data, "mario1")
        start = unlocked.chunk_ranges(unlocked.parse_entries(data)["mario1"])["PCE1"][0]
        return offset + start + seed * 2

    def unfed(self, data: bytes) -> bytes:
        for seed in unlocked.GALAXY_TICO_FED:
            data = self.edit(data, self.seed_position(data, seed), 0)
        return data

    def test_accepts_feeding(self):
        after = self.edit(self.before, self.seed_position(self.before, 4), 50)
        self.assertEqual(unlocked.check_feed(self.before, after), [("mario1", 4)])
        self.assertEqual(unlocked.check_feed(self.before, self.before), [])

    def test_rejects_anything_else(self):
        for seed, value in ((4, 49), (4, 51), (8, 0), (0, 5)):
            with self.subTest(seed=seed, value=value):
                with self.assertRaises(ValueError):
                    unlocked.check_feed(self.before, self.edit(self.before, self.seed_position(self.before, seed), value))
        star = self.entry_offset(self.before, "mario1") + 0x10
        with self.assertRaisesRegex(ValueError, "outside"):
            unlocked.check_feed(self.before, self.edit(self.before, star, 0xFFFF))
        sysconf = self.entry_offset(self.before, "sysconf")
        start, end = unlocked.sysconf_time_sent(unlocked.parse_entries(self.before)["sysconf"])
        self.assertEqual(unlocked.check_feed(self.before, self.edit(self.before, sysconf + start, 0x1234)), [])
        with self.assertRaisesRegex(ValueError, "sysconf changed outside"):
            unlocked.check_feed(self.before, self.edit(self.before, sysconf + end, 0x1234))
        config = self.entry_offset(self.before, "config1")
        with self.assertRaisesRegex(ValueError, "config1 changed"):
            unlocked.check_feed(self.before, self.edit(self.before, config, 0x1234))


class SpinDriverRecordTests(unittest.TestCase):
    """SPN1: a played save can hold an empty zone header (a Launch Star registered,
    nothing drawn) that the game drops when it saves again; real path records must
    match (found on a playtest save, 2026-10-01)."""

    @staticmethod
    def spn(*scenarios: bytes) -> bytes:
        blocks = b"".join(struct.pack("<H", len(body) + 3) + body + b"\xff" for body in scenarios)
        return bytes([1]) + struct.pack("<HHBB", 0x1234, 6 + len(blocks), len(scenarios), 0) + blocks

    def test_empty_zone_header_is_no_state(self):
        empty_zone = self.spn(b"\xc0", b"")
        self.assertEqual(unlocked.spin_driver_records(empty_zone), unlocked.spin_driver_records(self.spn(b"", b"")))
        self.assertEqual(unlocked.spin_driver_records(empty_zone), {})

    def test_path_records_are_compared(self):
        drawn = unlocked.spin_driver_records(self.spn(b"\xc0\x81\x40", b"\xc2\x83\x00"))
        self.assertEqual(drawn, {(0x1234, 0, 0, 1): ("range", 0x40), (0x1234, 1, 2, 3): ("range", 0)})
        self.assertNotEqual(drawn, unlocked.spin_driver_records(self.spn(b"\xc0\x81\x41", b"\xc2\x83\x00")))
        self.assertNotEqual(drawn, unlocked.spin_driver_records(self.spn(b"", b"\xc2\x83\x00")))


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
                self.assertEqual(unlocked.unfed_tico_seeds(summary["mario1"]), [])
                if variant == "grand-finale":
                    self.assertEqual(summary["luigi1"]["star_bits"], 120)
                    self.assertEqual(unlocked.unfed_tico_seeds(summary["luigi1"]), [])
        if checked == 0:
            self.skipTest("no generated saves under build/saves")


if __name__ == "__main__":
    unittest.main()
