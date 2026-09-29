#!/usr/bin/env python3
"""Isolation guarantees for the observatory test fixture copier."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "fixture", Path(__file__).parents[1] / "tools/create_observatory_fixture.py")
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


class FixtureTests(unittest.TestCase):
    def test_copy_and_refuse_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, output = root / "source", root / "fixture"
            save = source / "NAND/title/00010000/524d4745/data/GameData.bin"
            save.parent.mkdir(parents=True)
            save.write_bytes(b"seed")
            fixture.create(source, output)
            copied = output / save.relative_to(source)
            self.assertEqual(copied.read_bytes(), b"seed")
            self.assertEqual((output / ".petari-test-fixture").read_text(), "observatory\n")
            manifest = json.loads((output / "fixture-source-hashes.json").read_text())
            self.assertIn(str(save.relative_to(source / "NAND")), manifest)
            copied.write_bytes(b"test progress")
            self.assertEqual(save.read_bytes(), b"seed")
            with self.assertRaises(ValueError):
                fixture.create(source, output)
            self.assertEqual(copied.read_bytes(), b"test progress")
            link = root / "link"
            link.symlink_to(source, target_is_directory=True)
            with self.assertRaises(ValueError):
                fixture.create(source, link)

    def test_missing_seed_leaves_no_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaises(ValueError):
                fixture.create(root, root / "output")
            self.assertFalse((root / "output").exists())


if __name__ == "__main__":
    unittest.main()
