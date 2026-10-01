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

--feed-galaxy-lumas USER_DIR patches an existing save with real progress (quit
Petari first): in every file, the in-galaxy Hungry Lumas whose mission star the
file holds are recorded as fed, as the game would have. The app does it on a
work copy; this script checks that only those Star Bit counts changed, keeps a
timestamped backup of GameData.bin in USER_DIR/save-backups, then replaces it.
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
# Hungry Lumas inside galaxies (TicoFat Obj_arg7 seed: Obj_arg1 star bits). Fed, they
# turn into a planet later missions start on, e.g. Toy Time's comet (FactoryGalaxy).
GALAXY_TICO_FED = {1: 20, 2: 50, 3: 40, 4: 50, 5: 80, 6: 30}


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


def chunk_ranges(blob: bytes) -> dict:
    """Byte range of each chunk's payload in a game data blob."""
    ranges, offset = {}, 4
    for _ in range(blob[1]):
        sig, _hash, size = struct.unpack_from("<3I", blob, offset)
        ranges[signature(sig)] = (offset + 12, offset + size)
        offset += size
    return ranges


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
    # StarPieceAlmsStorage: 16 host-order u16 star bit counts, seeds 0-7 in galaxies, 8+ observatory.
    alms = list(struct.unpack_from("<16H", chunks["PCE1"], 0))
    return {"galaxies": galaxy_num, "star_bits": stars, "stored_flags": len(flags) // 2,
            "stored_flags_on": flags_on, "story_progress": story, "tico_seeds": alms}


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


def unfed_tico_seeds(game_summary: dict) -> list:
    seeds = game_summary["tico_seeds"]
    return [seed for seed, wanted in sorted(GALAXY_TICO_FED.items()) if seeds[seed] < wanted]


def sysconf_time_sent(blob: bytes) -> tuple:
    """Byte range of SysConfigChunk mTimeSent (its 2nd attribute, the Wii Mail send
    window's day; NWC24Function updateWiiMailSentSize) in a sysconf blob."""
    if blob[:2] != b"\x01\x01" or blob[4:8] != b"CSYS":
        raise ValueError(f"sysconf header {blob[:8].hex()}")
    attributes = struct.unpack_from("<H", blob, 16)[0]
    if attributes != 3:
        raise ValueError(f"sysconf: {attributes} attributes")
    record = 16 + 4 + attributes * 4
    offset = struct.unpack_from("<HH", blob, 16 + 4 + 4)[1]
    return record + offset, record + offset + 8


def check_feed(before: bytes, after: bytes) -> list:
    """The changes a feed-galaxy-lumas run made; raises ValueError on any other change.

    Allowed: in a mario<N>/luigi<N> game data entry, PCE1 seeds listed in
    GALAXY_TICO_FED raised from below to exactly their count. The game's save also
    rewrites sysconf; there only its Wii Mail timestamp (mTimeSent) may change, as
    it does in ordinary play. Returns [(entry, seed)]."""
    old, new = parse_entries(before), parse_entries(after)
    if list(old) != list(new) or any(len(old[name]) != len(new[name]) for name in old):
        raise ValueError("entry layout changed")
    if before[4:16] != after[4:16] or len(before) != len(after):
        raise ValueError("file header changed")
    fed = []
    for name in old:
        if old[name] == new[name]:
            continue
        if name == "sysconf":
            start, end = sysconf_time_sent(old[name])
            if old[name][:start] != new[name][:start] or old[name][end:] != new[name][end:]:
                raise ValueError("sysconf changed outside its Wii Mail timestamp")
            continue
        if not (name.startswith("mario") or name.startswith("luigi")) or not any(old[name]):
            raise ValueError(f"{name} changed")
        start, end = chunk_ranges(old[name])["PCE1"]
        if chunk_ranges(new[name])["PCE1"] != (start, end) or old[name][:start] != new[name][:start] or old[name][end:] != new[name][end:]:
            raise ValueError(f"{name} changed outside the Star Bit (PCE1) counts")
        seeds_old = struct.unpack_from("<16H", old[name], start)
        seeds_new = struct.unpack_from("<16H", new[name], start)
        for seed, (was, now) in enumerate(zip(seeds_old, seeds_new)):
            if was == now:
                continue
            if seed not in GALAXY_TICO_FED or was >= GALAXY_TICO_FED[seed] or now != GALAXY_TICO_FED[seed]:
                raise ValueError(f"{name} seed {seed} changed {was} -> {now}")
            fed.append((name, seed))
    return fed


def feed_galaxy_lumas(user: Path, app: Path, disc: Path, timeout: float) -> list:
    """Patch USER_DIR's save in place (see the module docstring); returns [(entry, seed)] fed."""
    user = user.resolve(strict=True)
    save = user / SAVE_RELATIVE
    if not save.is_file():
        raise ValueError(f"no saved file: {save}")
    running = subprocess.run(["pgrep", "-f", f"Petari.*{user}"], capture_output=True, text=True).stdout.split()
    if running:
        raise ValueError(f"Petari is running with {user} (pids {' '.join(running)}); quit it first")
    original = save.read_bytes()
    parse_entries(original)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    work = user.with_name(f".{user.name}.feed-galaxy-lumas-{stamp}")
    work.mkdir()
    (work / SAVE_RELATIVE).parent.mkdir(parents=True)
    shutil.copy2(save, work / SAVE_RELATIVE)
    (work / MARKER).write_text("feed-galaxy-lumas\n")
    log = work / "generation.log"
    command = [str(app), "--disc", str(disc), "--user", str(work), "--make-unlocked-save", "feed-galaxy-lumas"]
    with log.open("w") as stream:
        stream.write("$ " + shlex.join(command) + "\n")
        stream.flush()
        try:
            status = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            status = "timeout"
    text = log.read_text(errors="replace")
    if status != 0 or "PETARI UNLOCKED SAVE: VERIFIED (feed-galaxy-lumas," not in text:
        raise ValueError(f"app status {status}; see {log} (your save was not changed)")
    patched = (work / SAVE_RELATIVE).read_bytes()
    try:
        fed = check_feed(original, patched)
    except ValueError as error:
        raise ValueError(f"{error}; see {work} (your save was not changed)") from None
    if save.read_bytes() != original:
        raise ValueError(f"{save} changed while patching; nothing replaced, see {work}")
    for line in text.splitlines():
        if line.startswith("PETARI UNLOCKED SAVE: file "):
            print(line.removeprefix("PETARI UNLOCKED SAVE: "))
    if fed:
        backups = user / "save-backups"
        backups.mkdir(exist_ok=True)
        backup = backups / f"GameData.bin.before-feed-galaxy-lumas-{stamp}"
        shutil.copy2(save, backup)
        replacement = save.with_name(save.name + ".feed-galaxy-lumas")
        replacement.write_bytes(patched)
        replacement.replace(save)
        print(f"Backup of the previous save: {backup}")
    shutil.rmtree(work)
    return fed


def unlocked_entries(variant: str) -> tuple:
    """Entries the generator rewrites; sysconf is rewritten by every game save."""
    return ("mario1", "luigi1", "config1", "sysconf") if variant == "grand-finale" else ("mario1", "config1", "sysconf")


def check_against_seed(variant: str, save: Path, seed_save: Path) -> dict:
    """Unlocked game data must hold 120 stars; every other file must equal the seed's."""
    summary = inspect(save)
    for name in ("mario1", "luigi1"):
        if name in unlocked_entries(variant) and (summary[name] or {}).get("star_bits") != 120:
            raise ValueError(f"{name}: {summary[name]}, expected 120 star bits")
        if name in unlocked_entries(variant):
            unfed = unfed_tico_seeds(summary[name])
            if unfed:
                raise ValueError(f"{name}: hungry Lumas in galaxies not fed, seeds {unfed}: {summary[name]['tico_seeds']}")
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
    parser.add_argument("--feed-galaxy-lumas", type=Path, metavar="USER_DIR",
                        help="patch an existing save in place: feed the in-galaxy Hungry Lumas its stars imply")
    args = parser.parse_args()
    try:
        if args.feed_galaxy_lumas:
            fed = feed_galaxy_lumas(args.feed_galaxy_lumas, args.app.absolute(), args.disc.absolute(), args.timeout)
            print(f"Fed {len(fed)} Hungry Luma(s)." if fed else "Nothing to feed; the save was not changed.")
            return 0
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
