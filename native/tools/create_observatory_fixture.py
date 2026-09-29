#!/usr/bin/env python3
"""Copy a saved-file seed into a new, explicitly marked observatory test directory."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil


def create(source: Path, output: Path) -> None:
    source = source.resolve(strict=True)
    output = output.absolute()
    if output.exists() or output.is_symlink():
        raise ValueError(f"output already exists; choose a new directory: {output}")
    save = source / "NAND/title/00010000/524d4745/data/GameData.bin"
    if not save.is_file():
        raise ValueError("source must contain an existing RMGE01 saved file")
    output.mkdir(parents=True)
    shutil.copytree(source / "NAND", output / "NAND")
    hashes = {
        str(p.relative_to(output / "NAND")): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted((output / "NAND").rglob("*")) if p.is_file()
    }
    (output / "fixture-source-hashes.json").write_text(json.dumps(hashes, indent=2) + "\n")
    (output / ".petari-test-fixture").write_text("observatory\n")
    print(f"Created {output}. Source NAND was only read.")
    print("Launch with --user PATH --test-fixture observatory.")
    print("This supplies post-tutorial progression at load; it does not prove tutorial completion.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path, help="user directory with an existing saved file")
    parser.add_argument("--output", required=True, type=Path, help="new test directory; existing directories are refused")
    args = parser.parse_args()
    try:
        create(args.source, args.output)
    except (OSError, ValueError) as error:
        parser.exit(1, f"fixture: {error}\n")
