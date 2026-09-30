#!/usr/bin/env python3
"""Observation snapshots retain provenance and reject unsafe PASS evidence."""
import hashlib
import json
from pathlib import Path
import sqlite3
import tempfile
import unittest

from collect_pipeline_observations import snapshot, survey_identity


class ObservationTests(unittest.TestCase):
    def test_survey_requires_matching_full_and_launch_identities(self):
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / 'Petari'
            binary.write_bytes(b'synthetic identity fixture')
            entry = {'clean_pass': True, 'exit_status': 0, 'smoke_result': 'PASS',
                     'set_aside': False, 'watchdog_or_abort': False,
                     'app_sha1_prefix': hashlib.sha1(binary.read_bytes()).hexdigest()[:12],
                     'app_frozen_copy': {'path': str(binary), 'sha256': hashlib.sha256(binary.read_bytes()).hexdigest()}}
            self.assertEqual(survey_identity(entry), entry['app_frozen_copy']['sha256'])
            for field, value in [('clean_pass', False), ('exit_status', 134), ('set_aside', True),
                                 ('watchdog_or_abort', True), ('app_frozen_copy', None),
                                 ('app_sha1_prefix', '000000000000')]:
                with self.subTest(field=field), self.assertRaises(ValueError):
                    survey_identity(dict(entry, **{field: value}))
            binary.write_bytes(b'replaced executable')
            with self.assertRaisesRegex(ValueError, 'SHA256 changed'):
                survey_identity(entry)

    def test_snapshot_rechecks_pass_and_retains_original_identity(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            log, cache, metadata = root / 'original.log', root / 'cache.db', root / 'manifest.json'
            log.write_text('PETARI SMOKE RESULT: PASS (stage); pressing the power button\n')
            metadata.write_text('{}')
            with sqlite3.connect(cache) as db:
                db.execute('CREATE TABLE pipeline_cache(hash INTEGER)')
                db.execute('INSERT INTO pipeline_cache VALUES(123)')
            result = {'smoke_result': 'PASS', 'exit_status': 0, 'timed_out': False,
                      'outcome': 'PASS', 'app_sha256': 'a' * 64, 'scenario': 4}
            evidence = {'source_files_sha256': {str(metadata): hashlib.sha256(metadata.read_bytes()).hexdigest()}}
            def run(name):
                return snapshot(root / 'out', name, log, cache, root / 'absent.csv',
                                root / 'user', 'ExampleGalaxy', result, evidence)
            record = run('good')
            saved = json.loads(Path(record['result']).read_text())
            self.assertEqual(saved['source_evidence'], evidence)
            self.assertEqual(saved['original_log'], str(log.resolve()))
            self.assertEqual(saved['scenario'], 4)
            self.assertEqual(saved['app_sha256'], 'a' * 64)
            log.write_text(log.read_text() + '[gx pipeline compile] status=failed\n')
            with self.assertRaisesRegex(ValueError, 'reparsed log'):
                run('renderer-error')
            log.write_text('PETARI SMOKE RESULT: PASS (stage); pressing the power button\n')
            metadata.write_text('{"changed": true}')
            with self.assertRaisesRegex(ValueError, 'provenance changed'):
                run('changed')
            self.assertFalse((root / 'out/runs/changed/app.log').exists())


if __name__ == '__main__':
    unittest.main()
