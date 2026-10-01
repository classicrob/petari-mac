#!/usr/bin/env python3
"""Make an "everything unlocked" Super Mario Galaxy save in a new directory.

The native app does the work with the game's own code (Petari --make-unlocked-save):
it loads the seed's saved file at boot, unlocks file 1 through GameDataHolder,
writes it with the game's save sequence, reloads it through the game's loader
and checks it, then exits. This script copies the seed's NAND into a work
directory, runs that, independently re-checks the file format (inspect()), and
only then publishes the NAND into --output. The seed is only read.

Variants (native/SAVES.md):
  all-missions    Mario, 120 stars: every dome, galaxy and mission selectable.
  complete-luigi  all-missions plus the 120-star ending: Luigi playable (fresh).
  grand-finale    120 stars for Mario and Luigi: Grand Finale Galaxy open too.

Example (run one app at a time):
  python3 native/tools/make_unlocked_save.py --variant all-missions
"""
import argparse
import hashlib
import json
import shlex
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

VARIANTS = ("all-missions", "complete-luigi", "grand-finale")
SAVE_RELATIVE = Path("NAND/title/00010000/524d4745/data/GameData.bin")
MARKER = ".petari-make-unlocked-save"
# SaveDataHandler: version 2, 19 entries (6 x mario/luigi/config + sysconf), < 0x10000 bytes.
FILE_VERSION = 2
FILE_ENTRIES = 19
GAME_DATA_SIZE = 0xF80
# GameDataHolder chunk signatures in their fixed order.
GAME_CHUNKS = ("PLAY", "FLG1", "PCE1", "SPN1", "VLE1", "GALA")


def check_sum(data: bytes) -> int:
    """MR::calcCheckSum over host (little-endian) u16 words."""
    total = inverse = 0
    for (word,) in struct.iter_unpack("<H", data[: len(data) // 2 * 2]):
        total = (total + word) & 0xFFFF
        inverse = (inverse + (~word & 0xFFFF)) & 0xFFFF
    return total << 16 | inverse


def signature(value: int) -> str:
    return value.to_bytes(4, "big").decode("latin-1")


def parse_entries(data: bytes) -> dict:
    stored_sum, version, count, size = struct.unpack_from("<4I", data, 0)
    if version != FILE_VERSION or count != FILE_ENTRIES or size >= 0x10000 or size > len(data):
        raise ValueError(f"bad header: version {version}, entries {count}, size {size:#x}, file {len(data):#x}")
    if len(data) != (size + 31) // 32 * 32:
        raise ValueError(f"file length {len(data):#x} is not the 32-byte rounded size {size:#x}")
    if check_sum(data[4:size]) != stored_sum:
        raise ValueError(f"checksum {stored_sum:#010x} does not match {check_sum(data[4:size]):#010x}")
    entries = {}
    for index in range(count):
        name, offset = struct.unpack_from("<12sI", data, 16 + index * 16)
        entries[name.split(b"\0", 1)[0].decode("ascii")] = offset
    ordered = sorted(entries.items(), key=lambda item: item[1])
    spans = {}
    for position, (name, offset) in enumerate(ordered):
        end = ordered[position + 1][1] if position + 1 < len(ordered) else size
        spans[name] = data[offset:end]
    return spans


def parse_game_data(blob: bytes) -> dict:
    """BinaryDataChunkHolder layout: u8 1, u8 chunk count, 2 pad, then chunks."""
    if not any(blob):
        return {}
    if blob[0] != 1 or blob[1] != len(GAME_CHUNKS):
        raise ValueError(f"game data header {blob[:4].hex()}")
    chunks, offset = {}, 4
    for _ in range(blob[1]):
        sig, _hash, size = struct.unpack_from("<3I", blob, offset)
        if size < 12 or offset + size > len(blob):
            raise ValueError(f"chunk {signature(sig)} size {size:#x} at {offset:#x} overruns the file")
        chunks[signature(sig)] = blob[offset + 12: offset + size]
        offset += size
    if tuple(chunks) != GAME_CHUNKS:
        raise ValueError(f"chunks {tuple(chunks)}")
    return chunks


def summarize_game_data(chunks: dict) -> dict:
    gala = chunks["GALA"]
    galaxy_num = struct.unpack_from("<H", gala, 0)[0]
    # BinaryDataContentHeaderSerializer header: u16 attribute count, u16 record size, then
    # (u16 name hash, u16 record offset) per attribute, added as mGalaxyName, mPowerStarFlag,
    # mFirstPlayFlag, mMaxCoinNum.
    attributes, record_size = struct.unpack_from("<HH", gala, 2)
    if attributes != 4:
        raise ValueError(f"GALA: {attributes} attributes")
    star_offset = struct.unpack_from("<HH", gala, 2 + 4 + 4)[1]
    records = gala[2 + 4 + attributes * 4:]
    if len(records) != galaxy_num * record_size:
        raise ValueError(f"GALA: {galaxy_num} records of {record_size} bytes, have {len(records)}")
    stars = sum(bin(records[i * record_size + star_offset]).count("1") for i in range(galaxy_num))
    flags = chunks["FLG1"]
    flags_on = sum(1 for (word,) in struct.iter_unpack("<H", flags) if word & 0x8000)
    story = chunks["PLAY"][0]
    return {"galaxies": galaxy_num, "star_bits": stars, "stored_flags": len(flags) // 2,
            "stored_flags_on": flags_on, "story_progress": story}


def inspect(path: Path) -> dict:
    """Independent format check of a GameData.bin; raises ValueError."""
    spans = parse_entries(path.read_bytes())
    summary = {}
    for name in ("mario1", "luigi1"):
        if len(spans[name]) != GAME_DATA_SIZE:
            raise ValueError(f"{name} span {len(spans[name]):#x}")
        chunks = parse_game_data(spans[name])
        summary[name] = summarize_game_data(chunks) if chunks else None
    return summary


def unlocked_entries(variant: str) -> tuple:
    """Entries the generator rewrites; sysconf is rewritten by every game save."""
    return ("mario1", "luigi1", "config1", "sysconf") if variant == "grand-finale" else ("mario1", "config1", "sysconf")


def check_against_seed(variant: str, save: Path, seed_save: Path) -> dict:
    """Unlocked game data must hold 120 stars; every other file must equal the seed's."""
    summary = inspect(save)
    for name in ("mario1", "luigi1"):
        if name in unlocked_entries(variant) and (summary[name] or {}).get("star_bits") != 120:
            raise ValueError(f"{name}: {summary[name]}, expected 120 star bits")
    spans, seed_spans = parse_entries(save.read_bytes()), parse_entries(seed_save.read_bytes())
    changed = [name for name in spans if name not in unlocked_entries(variant) and spans[name] != seed_spans[name]]
    if changed:
        raise ValueError(f"entries changed that should equal the seed: {changed}")
    return summary


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def generate(variant: str, seed: Path, output: Path, app: Path, disc: Path, timeout: float) -> dict:
    seed = seed.resolve(strict=True)
    output = output.absolute()
    if output.exists() or output.is_symlink():
        raise ValueError(f"output already exists; choose a new directory: {output}")
    if not (seed / SAVE_RELATIVE).is_file():
        raise ValueError(f"seed has no saved file: {seed / SAVE_RELATIVE}")
    work = output.with_name(f".{output.name}.work-{int(time.time())}")
    work.mkdir(parents=True)
    shutil.copytree(seed / "NAND", work / "NAND")
    (work / MARKER).write_text(variant + "\n")
    log = work / "generation.log"
    command = [str(app), "--disc", str(disc), "--user", str(work), "--make-unlocked-save", variant]
    with log.open("w") as stream:
        stream.write("$ " + shlex.join(command) + "\n")
        stream.flush()
        try:
            status = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            status = "timeout"
    text = log.read_text(errors="replace")
    if status != 0 or "PETARI UNLOCKED SAVE: VERIFIED" not in text:
        raise ValueError(f"app status {status}; see {log} (work directory kept)")
    return publish(variant, seed, work, output, app)


def publish(variant: str, seed: Path, work: Path, output: Path, app: Path) -> dict:
    """Re-check a verified work directory, then copy its NAND into the new output."""
    text = (work / "generation.log").read_text(errors="replace")
    if "PETARI UNLOCKED SAVE: VERIFIED" not in text or f"VERIFIED ({variant}," not in text:
        raise ValueError(f"{work} holds no verified {variant} run")
    try:
        summary = check_against_seed(variant, work / SAVE_RELATIVE, seed / SAVE_RELATIVE)
    except ValueError as error:
        raise ValueError(f"{error}; see {work}") from None
    log = work / "generation.log"
    output.mkdir(parents=True)
    shutil.copytree(work / "NAND", output / "NAND")
    shutil.copy2(log, output / "generation.log")
    manifest = {
        "variant": variant,
        "seed": str(seed),
        "seed_sha256": sha256(seed / SAVE_RELATIVE),
        "app_sha256": sha256(app),
        "GameData.bin_sha256": sha256(output / SAVE_RELATIVE),
        "inspect": summary,
        "game_report": [line for line in text.splitlines() if line.startswith("PETARI UNLOCKED SAVE")],
    }
    (output / "unlocked-save.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
    shutil.rmtree(work)
    return manifest


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--variant", choices=VARIANTS, help="which save to make")
    parser.add_argument("--seed", type=Path, default=root / "build/observatory-user-2",
                        help="user directory whose file 1 exists (its icon is kept); only read")
    parser.add_argument("--output", type=Path, help="new directory (default build/saves/VARIANT); never overwritten")
    parser.add_argument("--app", type=Path, default=root / "build/macos-gx/native/app/Petari.app/Contents/MacOS/Petari")
    parser.add_argument("--disc", type=Path, default=root / "build/game-data/RMGE01")
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--inspect", type=Path, metavar="GAMEDATA_BIN", help="only check an existing GameData.bin")
    args = parser.parse_args()
    try:
        if args.inspect:
            print(json.dumps(inspect(args.inspect), indent=2))
            return 0
        if not args.variant:
            parser.error("--variant is required")
        output = args.output or root / "build/saves" / args.variant
        manifest = generate(args.variant, args.seed, output, args.app.absolute(), args.disc.absolute(),
                            args.timeout)
    except (OSError, ValueError) as error:
        print(f"make_unlocked_save: {error}", file=sys.stderr)
        return 1
    print("\n".join(manifest["game_report"]))
    print(f"Created {output}. Play it with: Petari --user {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
