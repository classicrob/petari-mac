#!/usr/bin/env python3
"""Check seed publication, resume invalidation, and coverage attribution without GPU."""
import contextlib
from contextlib import closing
import io
import json
import sqlite3
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
import build_pipeline_stage_seeds as seeds
import pipeline_coverage
import pipeline_owner_report
import merge_pipeline_observations


def database(path, key):
    with closing(sqlite3.connect(path)) as db, db:
        db.execute('CREATE TABLE pipeline_cache(type,hash,config_version,config_size,config,first_frame_used)')
        db.execute('INSERT INTO pipeline_cache VALUES(1,?,13,1,?,0)', (key, b'x'))


class ReplayTests(unittest.TestCase):
    def test_owner_join_and_observed_additions_keep_provenance(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ('Stage', '__global__'):
                with closing(sqlite3.connect(root / (name + '.db'))) as db, db:
                    db.execute('CREATE TABLE pipeline_cache(type,hash,config_version,config_size,config,first_frame_used,PRIMARY KEY(type,hash))')
                    db.execute('INSERT INTO pipeline_cache VALUES(1,1,13,1,?,0)', (b'x',))
                (root / (name + '.json')).write_text(json.dumps({'kind': 'native-display-list-replay-plus-observed'}))
            database(root / 'capture.db', -1)
            log = root / 'app.log'
            log.write_text('[gx draw owner] owner=7 frame=2 pc=0x123 image_base=0x100 symbol=JPAEmitter::draw()\n'
                           '[gx stage config] stage=Stage config=ffffffffffffffff phase=after_prep covered=0 stage_covered=0 overlay_tag=0 owner=7\n')
            result = pipeline_owner_report.report(log, root, root / 'capture.db')['records'][0]
            self.assertEqual(result['producer_family'], 'JPA particles')
            self.assertTrue(result['missing_from_seed'])
            self.assertTrue(result['captured_config_present'])
            self.assertEqual(merge_pipeline_observations.merge(root, 'Stage', root / 'capture.db', [log]), {'Stage': 1, '__global__': 1})
            result = pipeline_owner_report.report(log, root, root / 'capture.db')['records'][0]
            self.assertFalse(result['missing_from_seed'])
            provenance = json.loads((root / 'Stage.json').read_text())['targeted_observed'][0]
            self.assertEqual(provenance['added_configs'], 1)
            self.assertFalse(provenance['offline_material_replay'])

    def test_resume_merge_and_failed_run_preserves_published_seeds(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ('ObjectData', 'StageData', 'LayoutData'):
                (root / name).mkdir()
            (root / 'ObjectData/Player.arc').write_bytes(b'player')
            (root / 'ObjectData/Hidden.arc').write_bytes(b'unmapped')
            (root / 'LayoutData/Menu.arc').write_bytes(b'menu')
            (root / 'replay').write_bytes(b'executable snapshot')
            manifest = {'stages': {'FileSelect': {'archives': [{'path': 'ObjectData/Player.arc'}]}}}
            (root / 'inputs.json').write_text(json.dumps(manifest))
            database(root / 'baseline.db', 1)
            with closing(sqlite3.connect(root / 'baseline.db')) as db, db:
                db.execute('INSERT INTO pipeline_cache VALUES(1,2,13,1,?,7)', (b'x',))
            args = SimpleNamespace(files=root, replay=root / 'replay', inputs=root / 'inputs.json',
                                   baseline=root / 'baseline.db', work=root / 'work', output=root / 'seeds', timeout=2)
            def replay(command, **kwargs):
                self.assertNotEqual(Path(command[0]), args.replay)
                key = {'Player.arc': 2, 'Menu.arc': 3, 'Hidden.arc': 4}[Path(command[1]).name]
                database(command[2], key)
                kwargs['stdout'].write('models=1 pairs=1 draws=1 configs=1 shader_variants=5\n')
                return SimpleNamespace(returncode=0)
            with contextlib.redirect_stdout(io.StringIO()), patch.object(seeds.subprocess, 'run', side_effect=replay) as invoke:
                self.assertEqual(seeds.run(args), 0)
                self.assertEqual(invoke.call_count, 3)
                self.assertEqual({row[1] for row in seeds.rows(args.output / 'FileSelect.db')}, {1, 2, 3})
                self.assertEqual({row[1] for row in seeds.rows(args.output / '__global__.db')}, {1, 2, 3, 4})
                with closing(sqlite3.connect(args.output / 'FileSelect.db')) as db:
                    plan = db.execute('EXPLAIN QUERY PLAN SELECT type,config_version,config FROM pipeline_cache ORDER BY first_frame_used').fetchall()
                    self.assertFalse(any('TEMP B-TREE' in row[3] for row in plan), plan)
                self.assertEqual({row[1]: row[5] for row in seeds.rows(args.output / 'FileSelect.db')},
                                 {1: 0, 2: 7, 3: (1 << 32) - 1})
                invoke.reset_mock()
                self.assertEqual(seeds.run(args), 0)
                invoke.assert_not_called()
                (root / 'ObjectData/Player.arc').write_bytes(b'changed')
                self.assertEqual(seeds.run(args), 0)
                self.assertEqual(invoke.call_count, 1)
            before = (args.output / 'FileSelect.db').read_bytes()
            args.replay.write_bytes(b'new executable')
            with contextlib.redirect_stdout(io.StringIO()), patch.object(seeds.subprocess, 'run', return_value=SimpleNamespace(returncode=-6)):
                self.assertEqual(seeds.run(args), 1)
            self.assertEqual((args.output / 'FileSelect.db').read_bytes(), before)

    def test_coverage_deduplicates_and_excludes_overlay_tags(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            log = root / 'app.log'
            entry = '[gx stage config] stage=Stage phase=after_prep config=ffffffffffffffff covered=0 stage_covered=0 overlay_tag=0\n'
            log.write_text(entry * 2 + entry.replace('overlay_tag=0', 'overlay_tag=1').replace('stage=Stage', 'stage=Overlay')
                           + entry.replace('phase=after_prep', 'phase=loading'))
            database(root / 'Stage.db', -1)
            result = pipeline_coverage.report([log], root)
            self.assertEqual(len(result), 1)
            self.assertEqual(result[0]['post_prep_first_use'], 1)
            self.assertEqual(result[0]['uncovered'], 1)
            self.assertEqual(result[0]['missing_from_supplied_seed'], [])


if __name__ == '__main__':
    unittest.main()
