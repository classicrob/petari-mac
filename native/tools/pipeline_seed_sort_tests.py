#!/usr/bin/env python3
"""Run the read-only SQLite sort regression with small synthetic seed databases."""
from contextlib import closing
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile


def main(executable):
    with tempfile.TemporaryDirectory(prefix='petari-seed-sort-') as temporary:
        root = Path(temporary)
        unindexed, indexed = root / 'unindexed.db', root / 'indexed.db'
        with closing(sqlite3.connect(unindexed)) as db, db:
            db.execute('CREATE TABLE pipeline_cache(type,config_version,config,first_frame_used)')
            # Sixteen MiB exceeds SQLite's default sorter memory budget.
            blob = bytes(4096)
            db.executemany('INSERT INTO pipeline_cache VALUES(1,13,?,?)',
                           ((blob, 4095 - row) for row in range(4096)))
        shutil.copyfile(unindexed, indexed)
        with closing(sqlite3.connect(indexed)) as db, db:
            db.execute('CREATE INDEX first_use ON pipeline_cache(first_frame_used)')
        return subprocess.run([str(executable), str(unindexed), str(indexed)], timeout=30).returncode


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: pipeline_seed_sort_tests.py TEST_EXECUTABLE')
    raise SystemExit(main(Path(sys.argv[1]).resolve()))
