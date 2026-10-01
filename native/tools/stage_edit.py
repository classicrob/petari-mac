#!/usr/bin/env python3
"""Edit a stage archive's placement tables (native/MODS.md "Make a level edit").

Reads a stage archive (Yaz0 or plain RARC) from your own copy of the disc, edits
JMap/BCSV placement rows, and writes a new archive for a mod folder
(<user>/mods/<Mod>/files/StageData/<Stage>.arc). The disc file is only read.

  stage_edit.py tables ARC                       list the archive's BCSV tables
  stage_edit.py show ARC TABLE [--name Coin]      print a table's rows (index, name, l_id, pos)
  stage_edit.py edit ARC OUT [--plain] OPS...     write an edited archive (Yaz0 unless --plain)

OPS (TABLE is a path inside the archive, e.g. jmp/placement/common/objinfo; ROW is
NAME@N, the Nth row with that object name, or #N, the Nth row of the table):
  --move TABLE ROW DX DY DZ      add to pos_x/pos_y/pos_z
  --set TABLE ROW FIELD=VALUE    set one field (number or text)
  --add TABLE ROW X Y Z          append a copy of ROW at X Y Z with a new unique l_id
  --new TABLE NAME X Y Z         append a new NAME object at X Y Z: no rotation, scale 1,
                                 every Obj_arg, switch and id field -1, a new unique l_id

Everything else in the archive is kept byte for byte: other files, directory and
entry records, names and field layouts. Changed tables are re-laid out in the
data section with their new sizes (32-byte aligned, as on the disc).
"""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from collect_pipeline_seed_inputs import decompress  # noqa: E402

# --- Yaz0 -------------------------------------------------------------------------------------


def yaz0_compress(data: bytes) -> bytes:
    """Standard Yaz0 (greedy LZ77, 4 KiB window, matches of 3..273 bytes)."""
    out = bytearray(b"Yaz0" + struct.pack(">I", len(data)) + bytes(8))
    heads = {}  # 3-byte key -> recent positions (newest last)
    pos, size = 0, len(data)

    def remember(at):
        if at + 3 <= size:
            chain = heads.setdefault(data[at:at + 3], [])
            chain.append(at)
            if len(chain) > 48:
                del chain[:16]

    while pos < size:
        code_at = len(out)
        out.append(0)
        code = 0
        for bit in range(8):
            if pos >= size:
                break
            best_len, best_dist = 0, 0
            if pos + 3 <= size:
                limit = min(0x111, size - pos)
                for start in reversed(heads.get(data[pos:pos + 3], ())):
                    dist = pos - start
                    if dist > 0x1000:
                        break
                    length = 3
                    while length < limit and data[start + length] == data[pos + length]:
                        length += 1
                    if length > best_len:
                        best_len, best_dist = length, dist
                        if length == limit:
                            break
            if best_len >= 3:
                d = best_dist - 1
                if best_len >= 0x12:
                    out += bytes((d >> 8, d & 0xFF, best_len - 0x12))
                else:
                    out += bytes((((best_len - 2) << 4) | (d >> 8), d & 0xFF))
                for at in range(pos, pos + best_len):
                    remember(at)
                pos += best_len
            else:
                code |= 0x80 >> bit
                out.append(data[pos])
                remember(pos)
                pos += 1
        out[code_at] = code
    return bytes(out)


# --- RARC -------------------------------------------------------------------------------------


class Archive:
    """A RARC whose file contents can be replaced; all metadata is kept."""

    def __init__(self, raw: bytes):
        self.yaz0 = raw[:4] == b"Yaz0"
        data = decompress(raw)
        if data[:4] != b"RARC":
            raise ValueError("not a RARC archive")
        self.data = data
        _, _, header, data_offset, _, _, _, _ = struct.unpack_from(">4s7I", data, 0)
        self.payload = header + data_offset
        nodes, node_offset, entries, entry_offset, _, string_offset, _, _, _ = struct.unpack_from(">6IHBB", data, header)
        self.entry_offset = header + entry_offset
        self.string_offset = header + string_offset
        self.files = {}  # path -> entry index
        self.contents = {}  # entry index -> bytes
        self._walk(header + node_offset, 0, "")

    def _string(self, offset):
        start = self.string_offset + offset
        return self.data[start:self.data.index(b"\0", start)].decode("latin-1")

    def _walk(self, nodes, index, path):
        node = nodes + index * 16
        count, first = struct.unpack_from(">HI", self.data, node + 10)
        for i in range(first, first + count):
            entry = self.entry_offset + i * 20
            flags = self.data[entry + 4]
            name = self._string(struct.unpack_from(">H", self.data, entry + 6)[0])
            if name in (".", ".."):
                continue
            value, size = struct.unpack_from(">II", self.data, entry + 8)
            if flags & 2:
                self._walk(nodes, value, f"{path}{name}/")
            else:
                self.files[(path + name).lower()] = i
                self.contents[i] = self.data[self.payload + value:self.payload + value + size]

    def find(self, table: str) -> int:
        """Entry index of a file by its path below the archive's root folder."""
        table = table.lower().strip("/")
        matches = [i for path, i in self.files.items() if path == table or path.split("/", 1)[-1] == table]
        if len(matches) != 1:
            raise ValueError(f"{table}: {'not found' if not matches else 'ambiguous'} in the archive")
        return matches[0]

    def build(self) -> bytes:
        """The archive with every file's current contents, in the original file order."""
        order = sorted(self.contents, key=lambda i: struct.unpack_from(">I", self.data, self.entry_offset + i * 20 + 8)[0])
        out = bytearray(self.data[:self.payload])
        body = bytearray()
        for i in order:
            content = self.contents[i]
            struct.pack_into(">II", out, self.entry_offset + i * 20 + 8, len(body), len(content))
            body += content
            body += bytes(-len(body) % 32)
        out += body
        header = struct.unpack_from(">I", self.data, 8)[0]
        old_length = struct.unpack_from(">I", self.data, 16)[0]
        mram, aram = struct.unpack_from(">II", self.data, 20)
        struct.pack_into(">I", out, 4, len(out))
        struct.pack_into(">I", out, 16, len(body))
        # The preload sizes cover the whole data section on stage archives (MRAM, ARAM 0).
        struct.pack_into(">II", out, 20, len(body) if mram == old_length else mram, len(body) if aram == old_length else aram)
        assert header == 0x20
        return bytes(out)


# --- BCSV (JMap) ------------------------------------------------------------------------------


def jmap_hash(name: str) -> int:
    value = 0
    for byte in name.encode("latin-1"):
        if byte >= 128:
            byte -= 256
        value = (byte + value * 31) & 0xFFFFFFFF
    return value


KNOWN = ("name l_id pos_x pos_y pos_z dir_x dir_y dir_z scale_x scale_y scale_z Obj_arg0 Obj_arg1 Obj_arg2 Obj_arg3 "
         "Obj_arg4 Obj_arg5 Obj_arg6 Obj_arg7 SW_APPEAR SW_DEAD SW_A SW_B SW_SLEEP CameraSetId MessageId CastId "
         "ViewGroupId ShapeModelNo CommonPath_ID ClippingGroupId GroupId DemoGroupId MapParts_ID MarioNo").split()
NAMES = {jmap_hash(n): n for n in KNOWN}


class Table:
    """A BCSV table: rows as raw records, strings in its string table."""

    def __init__(self, data: bytes):
        count, fields, self.data_offset, self.entry_size = struct.unpack_from(">4I", data, 0)
        self.fields = []  # (hash, mask, offset, shift, type)
        for i in range(fields):
            self.fields.append(struct.unpack_from(">IIHBB", data, 16 + i * 12))
        self.header = data[:self.data_offset]
        self.rows = [bytearray(data[self.data_offset + r * self.entry_size:self.data_offset + (r + 1) * self.entry_size])
                     for r in range(count)]
        end = self.data_offset + count * self.entry_size
        strings = data[end:]
        # The string table is followed by '@' padding to 32 bytes.
        self.strings = bytearray(strings.rstrip(b"@")) if strings.rstrip(b"@").endswith(b"\0") else bytearray(strings)

    def field(self, name: str):
        key = jmap_hash(name)
        for field in self.fields:
            if field[0] == key:
                return field
        raise KeyError(f"no field {name}")

    def get(self, row: int, name: str):
        _, mask, offset, shift, kind = self.field(name)
        record = self.rows[row]
        if kind == 2:
            return struct.unpack_from(">f", record, offset)[0]
        if kind == 6:
            at = struct.unpack_from(">I", record, offset)[0]
            return self.strings[at:self.strings.index(b"\0", at)].decode("shift_jis", "replace")
        if kind == 1:
            return bytes(record[offset:offset + 32]).split(b"\0")[0].decode("shift_jis", "replace")
        width = {0: 4, 3: 4, 4: 2, 5: 1}[kind]
        raw = int.from_bytes(record[offset:offset + width], "big")
        value = (raw & mask) >> shift
        bits = width * 8
        return value - (1 << bits) if value >= 1 << (bits - 1) else value

    def set(self, row: int, name: str, value):
        _, mask, offset, shift, kind = self.field(name)
        record = self.rows[row]
        if kind == 2:
            struct.pack_into(">f", record, offset, float(value))
        elif kind == 6:
            text = str(value).encode("shift_jis") + b"\0"
            at = self.strings.find(text)
            if at < 0 or (at > 0 and self.strings[at - 1] != 0):
                at = len(self.strings)
                self.strings += text
            struct.pack_into(">I", record, offset, at)
        elif kind == 1:
            record[offset:offset + 32] = str(value).encode("shift_jis").ljust(32, b"\0")[:32]
        else:
            width = {0: 4, 3: 4, 4: 2, 5: 1}[kind]
            raw = int.from_bytes(record[offset:offset + width], "big")
            raw = (raw & ~mask) | ((int(value) << shift) & mask)
            record[offset:offset + width] = (raw & ((1 << (width * 8)) - 1)).to_bytes(width, "big")

    def has(self, name: str) -> bool:
        return any(field[0] == jmap_hash(name) for field in self.fields)

    def find(self, spec: str) -> int:
        """ROW spec: NAME@N (Nth row named NAME) or #N (Nth row)."""
        if spec.startswith("#"):
            row = int(spec[1:])
            if not 0 <= row < len(self.rows):
                raise ValueError(f"{spec}: the table has {len(self.rows)} rows")
            return row
        name, _, nth = spec.partition("@")
        rows = [r for r in range(len(self.rows)) if self.get(r, "name") == name]
        nth = int(nth or 0)
        if nth >= len(rows):
            raise ValueError(f"{spec}: {len(rows)} rows named {name}")
        return rows[nth]

    def build(self) -> bytes:
        out = bytearray(self.header)
        struct.pack_into(">I", out, 0, len(self.rows))
        for record in self.rows:
            out += record
        out += self.strings
        out += b"@" * (-len(out) % 32)
        return bytes(out)


# --- edits ------------------------------------------------------------------------------------


class StageEdit:
    def __init__(self, raw: bytes):
        self.archive = Archive(raw)
        self.tables = {}

    def table(self, path: str) -> Table:
        index = self.archive.find(path)
        if index not in self.tables:
            self.tables[index] = Table(self.archive.contents[index])
        return self.tables[index]

    def move(self, path, spec, dx, dy, dz) -> int:
        table = self.table(path)
        row = table.find(spec)
        for axis, delta in zip("xyz", (dx, dy, dz)):
            table.set(row, f"pos_{axis}", table.get(row, f"pos_{axis}") + delta)
        return row

    def add(self, path, spec, x, y, z) -> int:
        table = self.table(path)
        source = table.find(spec)
        table.rows.append(bytearray(table.rows[source]))
        row = len(table.rows) - 1
        for axis, value in zip("xyz", (x, y, z)):
            table.set(row, f"pos_{axis}", value)
        if table.has("l_id"):
            # Unique within the zone's placement tables, as editors assign it.
            used = {t.get(r, "l_id") for t in self._placement_tables() if t.has("l_id") for r in range(len(t.rows))}
            table.set(row, "l_id", max(used | {0}) + 1)
        return row

    def new(self, path, name, x, y, z) -> int:
        table = self.table(path)
        if not table.rows:
            raise ValueError(f"{path} has no row to take the field layout from")
        row = self.add(path, "#0", x, y, z)
        table.set(row, "name", name)
        for hash_, *_ in table.fields:
            field = NAMES.get(hash_)
            if field is None or field in ("name", "l_id") or field.startswith("pos_"):
                continue
            if field.startswith("dir_"):
                table.set(row, field, 0.0)
            elif field.startswith("scale_"):
                table.set(row, field, 1.0)
            else:
                table.set(row, field, -1)
        return row

    def _placement_tables(self):
        for path in self.archive.files:
            if "/placement/" in f"/{path}" or "/mapparts/" in f"/{path}" or "/start/" in f"/{path}":
                if path.endswith("info"):
                    yield self.table(path)

    def build(self, compress: bool) -> bytes:
        for index, table in self.tables.items():
            self.archive.contents[index] = table.build()
        data = self.archive.build()
        return yaz0_compress(data) if compress else data


def parse_value(text):
    for kind in (int, float):
        try:
            return kind(text)
        except ValueError:
            pass
    return text


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("tables")
    p.add_argument("archive", type=Path)
    p = sub.add_parser("show")
    p.add_argument("archive", type=Path)
    p.add_argument("table")
    p.add_argument("--name")
    p = sub.add_parser("edit")
    p.add_argument("archive", type=Path)
    p.add_argument("output", type=Path)
    p.add_argument("--plain", action="store_true", help="write an uncompressed RARC instead of Yaz0")
    p.add_argument("--move", nargs=5, action="append", default=[], metavar=("TABLE", "ROW", "DX", "DY", "DZ"))
    p.add_argument("--set", nargs=3, action="append", default=[], metavar=("TABLE", "ROW", "FIELD=VALUE"))
    p.add_argument("--add", nargs=5, action="append", default=[], metavar=("TABLE", "ROW", "X", "Y", "Z"))
    p.add_argument("--new", nargs=5, action="append", default=[], metavar=("TABLE", "NAME", "X", "Y", "Z"))
    args = parser.parse_args(argv)
    try:
        edit = StageEdit(args.archive.read_bytes())
        if args.command == "tables":
            for path in sorted(edit.archive.files):
                if path.endswith("info") or path.endswith(".bcsv"):
                    print(path)
            return 0
        if args.command == "show":
            table = edit.table(args.table)
            for row in range(len(table.rows)):
                name = table.get(row, "name") if table.has("name") else ""
                if args.name and name != args.name:
                    continue
                pos = tuple(round(table.get(row, f"pos_{a}"), 1) for a in "xyz") if table.has("pos_x") else ()
                lid = table.get(row, "l_id") if table.has("l_id") else ""
                print(f"#{row}\t{name}\tl_id {lid}\t{pos}")
            return 0
        if args.output.resolve() == args.archive.resolve():
            raise ValueError("the output must be a new file; the disc archive is only read")
        for path, spec, dx, dy, dz in args.move:
            row = edit.move(path, spec, float(dx), float(dy), float(dz))
            print(f"moved {path} #{row} by ({dx}, {dy}, {dz})")
        for path, spec, assignment in args.set:
            field, _, value = assignment.partition("=")
            table = edit.table(path)
            row = table.find(spec)
            table.set(row, field, parse_value(value))
            print(f"set {path} #{row} {field}={value}")
        for path, spec, x, y, z in args.add:
            row = edit.add(path, spec, float(x), float(y), float(z))
            print(f"added {path} #{row} at ({x}, {y}, {z}), l_id {edit.table(path).get(row, 'l_id')}")
        for path, name, x, y, z in args.new:
            row = edit.new(path, name, float(x), float(y), float(z))
            print(f"new {name} in {path} #{row} at ({x}, {y}, {z}), l_id {edit.table(path).get(row, 'l_id')}")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(edit.build(not args.plain))
        print(f"wrote {args.output}")
    except (OSError, ValueError, KeyError) as error:
        print(f"stage_edit: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
