#!/usr/bin/env python3
"""native/tools/stage_edit.py: Yaz0, RARC rebuild and BCSV edits (native/MODS.md "Make a level edit").

The archive is synthesized here (no Nintendo data). With an extracted disc in
build/game-data, every stage archive must also rebuild byte for byte unedited.
"""
import importlib.util
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "native/tools"))
spec = importlib.util.spec_from_file_location("stage_edit", ROOT / "native/tools/stage_edit.py")
stage_edit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage_edit)
from collect_pipeline_seed_inputs import decompress  # noqa: E402

STAGE_DATA = ROOT / "build/game-data/RMGE01/files/StageData"
H = stage_edit.jmap_hash


def bcsv(rows):
    """objinfo-like table: name (string), l_id (long), pos_x/y/z (float), Obj_arg0 (long), ShapeModelNo (short)."""
    fields = [(H("name"), 0xFFFFFFFF, 0, 0, 6), (H("l_id"), 0xFFFFFFFF, 4, 0, 0), (H("pos_x"), 0xFFFFFFFF, 8, 0, 2),
              (H("pos_y"), 0xFFFFFFFF, 12, 0, 2), (H("pos_z"), 0xFFFFFFFF, 16, 0, 2), (H("Obj_arg0"), 0xFFFFFFFF, 20, 0, 0),
              (H("ShapeModelNo"), 0xFFFF, 24, 0, 4)]
    size = 28
    data_offset = 16 + 12 * len(fields)
    strings, records = bytearray(), bytearray()
    for name, lid, x, y, z in rows:
        at = strings.find(name.encode() + b"\0")
        if at < 0:
            at = len(strings)
            strings += name.encode() + b"\0"
        records += struct.pack(">IifffiH2x", at, lid, x, y, z, 7, 3)
    out = bytearray(struct.pack(">4I", len(rows), len(fields), data_offset, size))
    for field in fields:
        out += struct.pack(">IIHBB", *field)
    out += records + strings
    out += b"@" * (-len(out) % 32)
    return bytes(out)


def rarc(files):
    """A RARC with one root folder "stage" holding jmp/placement/common/<files>."""
    names = bytearray(b".\0..\0")

    def name(text):
        at = names.find(text.encode() + b"\0")
        if at < 0:
            at = len(names)
            names.extend(text.encode() + b"\0")
        return at

    folders = [("ROOT", "stage"), ("JMP ", "jmp"), ("PLAC", "placement"), ("COMM", "common")]
    nodes, entries, data = bytearray(), bytearray(), bytearray()
    first = 0
    for index, (kind, folder) in enumerate(folders):
        children = [(f, True) for f in files] if index == 3 else [(folders[index + 1][1], False)]
        nodes += struct.pack(">4sIHHI", kind.encode(), name(folder), 0, len(children) + 2, first)
        for child, is_file in children:
            if is_file:
                content = files[child]
                entries += struct.pack(">HHHHIII", len(entries) // 20, 0, 0x1100, name(child), len(data), len(content), 0)
                data += content + bytes(-len(content) % 32)
            else:
                entries += struct.pack(">HHHHIII", 0xFFFF, 0, 0x0200, name(child), index + 1, 0x10, 0)
        entries += struct.pack(">HHHHIII", 0xFFFF, 0, 0x0200, 0, index, 0x10, 0)
        entries += struct.pack(">HHHHIII", 0xFFFF, 0, 0x0200, 2, index - 1 if index else 0xFFFFFFFF, 0x10, 0)
        first += len(children) + 2
    names += bytes(-len(names) % 32)
    info = 0x20
    node_offset = 0x20
    entry_offset = node_offset + len(nodes) + (-len(nodes) % 32)
    string_offset = entry_offset + len(entries) + (-len(entries) % 32)
    data_offset = string_offset + len(names)
    head = bytearray(data_offset)
    struct.pack_into(">6IHBB", head, info, len(folders), node_offset, len(entries) // 20, entry_offset, len(names),
                     string_offset, len(files), 1, 0)
    head[info + node_offset:info + node_offset + len(nodes)] = nodes
    head[info + entry_offset:info + entry_offset + len(entries)] = entries
    head[info + string_offset:info + string_offset + len(names)] = names
    out = bytearray(b"RARC" + struct.pack(">7I", 0, 0x20, data_offset, len(data), len(data), 0, 0)) + head[0x20:] + data
    struct.pack_into(">I", out, 4, len(out))
    return bytes(out[:0x20]) + bytes(head[0x20:]) + bytes(data)


def sample():
    return rarc({"objinfo": bcsv([("Coin", 1, 10.0, 20.0, 30.0), ("Kuribo", 2, 0.0, 0.0, 0.0), ("Coin", 3, -5.0, 0.0, 5.0)]),
                 "areaobjinfo": bcsv([("Cube", 9, 1.0, 1.0, 1.0)])})


class Yaz0Tests(unittest.TestCase):
    def test_round_trip(self):
        for data in (b"", b"a", b"abcabcabcabcabc" * 50, bytes(range(256)) * 9, b"\0" * 5000):
            with self.subTest(size=len(data)):
                packed = stage_edit.yaz0_compress(data)
                self.assertEqual(packed[:4], b"Yaz0")
                self.assertEqual(decompress(packed), data)
        self.assertLess(len(stage_edit.yaz0_compress(b"\0" * 5000)), 200)


class ArchiveTests(unittest.TestCase):
    def test_unedited_rebuild_is_identical(self):
        raw = sample()
        self.assertEqual(stage_edit.StageEdit(raw).build(compress=False), raw)
        edit = stage_edit.StageEdit(stage_edit.yaz0_compress(raw))
        edit.table("jmp/placement/common/objinfo")
        self.assertEqual(decompress(edit.build(compress=True)), raw)

    def test_move_set_add_new(self):
        edit = stage_edit.StageEdit(sample())
        edit.move("jmp/placement/common/objinfo", "Coin@1", 1.0, 2.0, 3.0)
        edit.table("placement/common/objinfo").set(1, "name", "Kinopio")
        added = edit.add("jmp/placement/common/objinfo", "Coin@0", 100.0, 200.0, 300.0)
        made = edit.new("jmp/placement/common/objinfo", "StarPiece", 7.0, 8.0, 9.0)
        rebuilt = stage_edit.StageEdit(edit.build(compress=True))
        table = rebuilt.table("jmp/placement/common/objinfo")
        self.assertEqual([table.get(r, "name") for r in range(len(table.rows))], ["Coin", "Kinopio", "Coin", "Coin", "StarPiece"])
        self.assertEqual([table.get(2, f"pos_{a}") for a in "xyz"], [-4.0, 2.0, 8.0])
        self.assertEqual([table.get(added, f"pos_{a}") for a in "xyz"], [100.0, 200.0, 300.0])
        # New l_ids are unique across the archive's placement tables (the cube has 9).
        self.assertEqual((table.get(added, "l_id"), table.get(made, "l_id")), (10, 11))
        self.assertEqual((table.get(added, "Obj_arg0"), table.get(made, "Obj_arg0"), table.get(made, "ShapeModelNo")), (7, -1, -1))
        # The other table is untouched.
        original = stage_edit.StageEdit(sample()).archive
        cube = "jmp/placement/common/areaobjinfo"
        self.assertEqual(rebuilt.archive.contents[rebuilt.archive.find(cube)], original.contents[original.find(cube)])

    def test_rows_and_errors(self):
        table = stage_edit.StageEdit(sample()).table("jmp/placement/common/objinfo")
        self.assertEqual((table.find("Coin@1"), table.find("#1")), (2, 1))
        with self.assertRaises(ValueError):
            table.find("Coin@2")
        with self.assertRaises(ValueError):
            stage_edit.StageEdit(sample()).table("jmp/placement/common/missing")
        self.assertEqual(stage_edit.main(["edit", "/dev/null", "/dev/null"]), 1)

    @unittest.skipUnless(STAGE_DATA.is_dir(), "no extracted disc in this checkout")
    def test_every_disc_stage_rebuilds_identically(self):
        for path in sorted(STAGE_DATA.glob("*.arc")):
            with self.subTest(archive=path.name):
                raw = path.read_bytes()
                edit = stage_edit.StageEdit(raw)
                for name in edit.archive.files:
                    if name.endswith("info") and "/jmp/" in f"/{name}":
                        edit.table(name)
                self.assertEqual(edit.build(compress=False), decompress(raw))


if __name__ == "__main__":
    unittest.main()
