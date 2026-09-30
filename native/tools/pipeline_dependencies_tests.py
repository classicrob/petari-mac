#!/usr/bin/env python3
"""Dependency closure includes tables, conditional callbacks and child actors."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from types import SimpleNamespace
import collect_pipeline_seed_inputs as collector
from pipeline_archive_dependencies import DependencyIndex, table, uncomment
from close_pipeline_seed_gaps import eligible


class DependencyTests(unittest.TestCase):
    def test_only_clean_pass_runs_are_eligible(self):
        clean = {'smoke_result': 'PASS', 'exit_status': 0, 'timed_out': False, 'outcome': 'PASS'}
        self.assertTrue(eligible(clean))
        self.assertTrue(eligible(dict(clean, outcome='PASS_WARN')))
        for key, value in [('smoke_result', 'ASSISTED'), ('exit_status', -6), ('timed_out', True),
                           ('outcome', 'PASS_RENDER_ERRORS'), ('renderer_error_count', 1),
                           ('assisted_inputs', ['Return']), ('heap_failure', True), ('crash', True)]:
            self.assertFalse(eligible(dict(clean, **{key: value})))
        self.assertFalse(eligible({}))


    def test_tables_callbacks_children_and_shared_resources(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sources = {
                'NameObj/NameObjFactory.cpp': '''
const Entry cCreateTable[] = {{"Parent", createNameObj<Parent>, "ParentModel"}};
const Entry cName2ArchiveNamesTable[] = {{"Parent", "Extra"}, {"Parent", "Second"}, {"Other", "Unused"}};
const Entry cName2MakeArchiveListFuncTable[] = {{"Parent", Helper::makeArchiveList}, {"Other", Missing::makeArchiveList}};
''',
                'Map/PlanetMapCreator.cpp': '''
const Entry sUniquePlanetCreateFuncTable[] = {{"Planet", createNameObj<Planet>}};
const Entry sUniquePlanetUniqueArchiveName[] = {{"Planet", "Shadow"}};
''',
                'System/StationedFileInfo.cpp': 'const char* shared = "/LayoutData/COMET.arc";',
                'MapObj/Parent.cpp': 'void Parent::init() { new Child(); }',
                'MapObj/Child.cpp': 'void Child::init() { initModelManagerWithAnm("ChildModel"); new Parent(); }',
                'MapObj/HELPER.cpp': '''
void Helper::makeArchiveList(NameObjArchiveListCollector* c, const Iter& i) {
 if (arg == 0) c->addArchive("Box"); else c->addArchive("Bubble");
 // c->addArchive("Unused");
}
'''}
            for relative, text in sources.items():
                path = root / 'src/Game' / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text)
            for name in ('Extra', 'Second', 'Unused', 'Shadow', 'ChildModel', 'Box', 'Bubble'):
                path = root / 'disc/ObjectData' / (name + '.arc')
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            (root / 'disc/LayoutData').mkdir()
            (root / 'disc/LayoutData/Comet.arc').touch()
            index = DependencyIndex(root / 'disc', root)
            paths, unresolved = index.resolve('Parent')
            self.assertEqual({p.stem for p, _ in paths}, {'Extra', 'Second', 'ChildModel', 'Box', 'Bubble'})
            self.assertEqual(unresolved, [])
            self.assertTrue(all(evidence for _, evidence in paths))
            self.assertEqual({p.stem for p, _ in index.shared}, {'Comet'})
            self.assertEqual({p.stem for p, _ in index.resolve('Planet')[0]}, {'Shadow'})
            self.assertEqual(index.resolve('Other')[1], ['Missing::makeArchiveList'])
            self.assertIn('src/Game/MapObj/HELPER.cpp', index.sources)

    def test_shared_registry_does_not_place_every_planet(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            factory = root / 'src/Game/NameObj/NameObjFactory.cpp'
            factory.parent.mkdir(parents=True)
            factory.write_text('')
            files = root / 'files'
            scenario = files / 'StageData/Stage/StageScenario.arc'
            scenario.parent.mkdir(parents=True)
            scenario.write_bytes(b'scenario')
            (files / 'StageData/ChildZone.arc').write_bytes(b'zone')
            catalog = files / 'ObjectData/PlanetMapDataTable.arc'
            catalog.parent.mkdir()
            catalog.write_bytes(b'catalog')
            (catalog.parent / 'UnrelatedPlanet.arc').write_bytes(b'planet')
            (files / 'LayoutData').mkdir()
            index = SimpleNamespace(shared=[(catalog, 'stationed registry')], sources={},
                                    resolve=lambda actor: ([], []))
            def resources(data):
                return [(0, 'data.bcsv', data)] if data in (b'catalog', b'scenario') else []
            def records(data):
                if data == b'catalog':
                    return [{collector.field_hash('name'): 'UnrelatedPlanet'}]
                return [{collector.field_hash('name'): 'ChildZone'}] if data == b'scenario' else []
            with patch.object(collector, 'DependencyIndex', return_value=index), \
                 patch.object(collector, 'archive_files', side_effect=resources), \
                 patch.object(collector, 'bcsv_strings', side_effect=records):
                result = collector.collect(files, root, ['Stage'], [])
            selected = {entry['path'] for entry in result['stages']['Stage']['archives']}
            self.assertIn('ObjectData/PlanetMapDataTable.arc', selected)
            self.assertIn('StageData/ChildZone.arc', selected)
            self.assertNotIn('ObjectData/UnrelatedPlanet.arc', selected)

    def test_table_boundaries_ignore_literal_braces(self):
        text = uncomment('const T table[] = {{"}","{"}, /* } */ {"A","B"}}; const T other[]={{"X","Y"}};')
        body, _ = table(text, 'table')
        self.assertIn('"A","B"', body)
        self.assertNotIn('"X","Y"', body)
        with self.assertRaises(ValueError):
            table(text, 'missing')


if __name__ == '__main__':
    unittest.main()
