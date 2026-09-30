#!/usr/bin/env python3
"""Coverage uses attributed draws, never imported pipeline-cache rows."""
from contextlib import closing
import json
from pathlib import Path
import sqlite3
import tempfile
import unittest
from pipeline_stage_audit import audit
from collect_pipeline_seed_inputs import archive_index, object_archives


def database(path, keys):
    with closing(sqlite3.connect(path)) as db, db:
        db.execute('CREATE TABLE pipeline_cache(type INTEGER, hash INTEGER)')
        db.executemany('INSERT INTO pipeline_cache VALUES(1,?)', [(k,) for k in keys])
        db.execute('INSERT INTO pipeline_cache VALUES(0,4)')


class AuditTests(unittest.TestCase):
    def test_game_archive_aliases_match_case_insensitively(self):
        paths = [Path('ObjectData/PowerUpFire.arc'), Path('ObjectData/PowerUpFireBloom.arc')]
        index = archive_index(paths)
        self.assertEqual(object_archives('MorphItemNeoFire', {'MorphItemNeoFire': 'PowerupFire'}, index), paths)
        self.assertEqual(object_archives('POWERUPFIRE', {}, index), paths)
        self.assertEqual(object_archives('Unknown', {}, index), [])
        with self.assertRaises(ValueError):
            archive_index([Path('A.arc'), Path('a.arc')])

    def test_attribution_and_static_archive_gap(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            seeds = root / 'seeds'
            seeds.mkdir()
            database(seeds / '__global__.db', [1, -1, 9])
            database(seeds / 'Stage.db', [1])
            database(root / 'cache.db', [1, -1, 9])
            database(root / 'replay.db', [-1])
            (seeds / 'Stage.json').write_text('{"archives": ["ObjectData/Placed.arc"]}')
            corpus = root / 'corpus.json'
            corpus.write_text(json.dumps({'ObjectData/Child.arc': {'exit': 0, 'database': str(root / 'replay.db')}}))
            log = root / 'app.log'
            log.write_text('\n'.join([
                '[gx stage config] stage=Stage config=0000000000000001 phase=loading covered=0',
                '[gx stage config] stage=Stage config=ffffffffffffffff phase=after_prep covered=0',
                '[gx stage config] stage=Stage config=ffffffffffffffff phase=after_prep covered=0',
                '[gx stage config] stage=Stage config=0000000000000003 phase=after_prep covered=0',
                '[gx stage config] stage=Stage config=0000000000000004 phase=after_prep covered=0',
                '[gx stage config] stage=Stage config=0000000000000009 phase=after_prep overlay_tag=1',
                '[gx stage config] stage=Unknown config=0000000000000009 phase=after_prep']))
            result = audit(seeds, [(log, root / 'cache.db')], ['Stage', 'Unseen'], corpus)
            stage = next(x for x in result['stages'] if x['stage'] == 'Stage')
            self.assertEqual(stage['observed'], 2)
            self.assertEqual(stage['stage_coverage_pct'], 50)
            self.assertEqual(stage['global_coverage_pct'], 100)
            self.assertEqual(stage['post_prep_observed'], 1)
            self.assertEqual(stage['missing_stage_known_other_replay_archives'],
                             {'ffffffffffffffff': ['ObjectData/Child.arc']})
            self.assertEqual(result['below_95'], ['Stage'])
            self.assertEqual(result['unobserved'], ['Unseen'])
            run = result['runs'][0]
            self.assertFalse(run['smoke_pass'])
            self.assertEqual(run['stages']['Stage']['not_in_retained_cache'],
                             ['0000000000000003', '0000000000000004'])
            self.assertEqual(result['metadata_errors'], {})

    def test_unavailable_or_partial_evidence_is_explicit(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            database(root / '__global__.db', [1])
            database(root / 'cache.db', [1])
            log = root / 'app.log'
            log.write_text('[gx stage config] stage=Absent config=0000000000000001 phase=after_prep')
            (root / 'result.json').write_text('{')
            result = audit(root, [(log, root / 'cache.db'), (log, root / 'missing.db')])
            self.assertEqual(result['stages'][0]['stage_coverage_pct'], 0)
            self.assertFalse(result['stages'][0]['stage_manifest_exists'])
            self.assertIn(str(root / 'result.json'), result['metadata_errors'])
            self.assertEqual(result['excluded'][0]['reason'], 'no retained pipeline cache')


if __name__ == '__main__':
    unittest.main()
