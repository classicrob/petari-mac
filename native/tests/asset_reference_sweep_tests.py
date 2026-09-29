#!/usr/bin/env python3
"""Format, classification, real-disc and negative-mutation asset audit checks."""
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'native/tools'))
import asset_reference_sweep as audit


def layout(name):
    block = b'txt1' + struct.pack('>I', 28) + bytes(4) + name.encode().ljust(16, b'\0')
    return b'RLYT' + struct.pack('>HHIHH', 0xfeff, 8, 16 + len(block), 16, 1) + block


class SweepTests(unittest.TestCase):
    def test_panes_and_bounds(self):
        self.assertEqual(list(audit.parse_resource(layout('TxtGalaxyName'), 'a.brlyt')), [('pane', 'TxtGalaxyName')])
        with self.assertRaises(ValueError):
            list(audit.parse_resource(layout('TxtGalaxyName')[:-1], 'a.brlyt'))

    def test_format_constraints(self):
        pattern = audit.format_pattern('%s%02d.arc')
        self.assertTrue(pattern.fullmatch('Astro01.arc'))
        self.assertFalse(pattern.fullmatch('Astro.arc'))
        self.assertFalse(audit.format_pattern('Map%d.arc').fullmatch('MapHello.arc'))
        self.assertTrue(audit.format_pattern('Percent%%_%X').fullmatch('Percent%_A9'))

    def test_comments_and_concatenation(self):
        ts = audit.tokens('// hidePane(this,"Bad");\n MR::hidePane(this, "Txt" "Good"); /* "Bad" */')
        call, = list(audit.calls(ts))
        self.assertEqual(audit.literal(call[1][1]), 'TxtGood')
        self.assertEqual(audit.literal(['name']), None)

    def test_local_ternary_and_constructor(self):
        source = 'Actor::Actor() : Base("Name", true) { initLayoutManager("Test", 1); }\nvoid Actor::show(bool flag) { const char* pane; pane = flag ? "TxtGalaxyName" : "TxtGalaxyNameU"; hidePane(this, pane); }'
        ts = audit.tokens(source)
        spans = audit.class_functions(ts)
        self.assertEqual(len(spans), 2)
        pos = source.index('hidePane')
        self.assertEqual(audit.assigned_literals(['pane'], source, pos, spans), ['TxtGalaxyName', 'TxtGalaxyNameU'])
        old = source.replace('TxtGalaxyName', 'TxtGaxyName')
        self.assertEqual(audit.assigned_literals(['pane'], old, old.index('hidePane'), audit.class_functions(audit.tokens(old))), ['TxtGaxyName', 'TxtGaxyNameU'])

    def test_bmg_tail_padding_only(self):
        def bmg(payload):
            return b'MESGbmg1' + struct.pack('>II', 32, 1) + bytes(16) + payload
        fli = b'FLI1' + struct.pack('>IHHI', 32, 1, 0, 0) + bytes(8)
        self.assertEqual(list(audit.parse_resource(bmg(fli), 'x.bmg')), [('bmg_omitted_tail_padding', '8')])
        with self.assertRaises(ValueError):
            list(audit.parse_resource(bmg(fli[:-1]), 'x.bmg'))

    @unittest.skipUnless((ROOT / 'build/game-data/RMGE01/files/AudioRes/SMR.szs').is_file(), 'disc not installed')
    def test_sound_typo_regressions(self):
        data = audit.decompress((ROOT / 'build/game-data/RMGE01/files/AudioRes/SMR.szs').read_bytes())
        sounds = {n for kind, n in audit.parse_resource(data, 'SMR.szs') if kind == 'sound'}
        self.assertGreater(len(sounds), 3000)
        cases = [('src/Game/Boss/DinoPackunBattleEggVs2.cpp', 'exeTurn', 'SE_BM_D_PAKKUN_SLAVER', 'SE_BM_D_PAKKUN_LAVER'),
                 ('src/Game/NPC/Tico.cpp', 'exeReaction', 'SE_SM_BUTLER_ABSORB', 'SE_BM_BUTLER_ABSORB')]
        for path, method, correct, typo in cases:
            source = (ROOT / path).read_text()
            body = source.split('::' + method + '()', 1)[1].split('\n}')[0]
            refs = [audit.literal(args[1]) for fn, args, _ in audit.calls(audit.tokens(body)) if fn == 'startSound']
            self.assertIn(correct, refs)
            self.assertNotIn(typo, sounds)
            self.assertTrue(all(name in sounds for name in refs))
            mutated = [typo if name == correct else name for name in refs]
            self.assertFalse(all(name in sounds for name in mutated))
        print(f'BSTN: {len(sounds)} registered names; both original typo mutations rejected')

    def test_name_table(self):
        table = bytes(8) + struct.pack('>HHHH', 1, 0xffff, 0, 8) + b'Joint\0'
        self.assertEqual(audit.name_table(table, 8), ['Joint'])
        with self.assertRaises(ValueError):
            audit.name_table(table[:-1], 8)

    def test_bcsv_hash_and_messages(self):
        data = struct.pack('>4I', 1, 1, 28, 4)
        data += struct.pack('>IIHBB', audit.field_hash('MessageId'), 0xffffffff, 0, 0, 6)
        data += bytes(4) + b'Layout_Test\0'
        parsed = list(audit.parse_resource(data, 'MessageId.tbl'))
        self.assertIn(('message', 'Layout_Test'), parsed)
        self.assertIn(('field_hash', str(audit.field_hash('MessageId'))), parsed)

    def test_scoping_and_negative_mutation(self):
        with tempfile.TemporaryDirectory() as temp:
            repo = Path(temp)
            (repo / 'src/Game/Util').mkdir(parents=True)
            (repo / 'src/Game/Util/LayoutUtil.cpp').write_text('void hidePane(LayoutActor* pActor, const char* pPaneName) {}\nvoid setLayoutPosAtPaneTrans(LayoutActor* pActor, const LayoutActor* pFollowActor, const char* pPaneName) {}')
            actor = repo / 'src/Actor.cpp'
            actor.write_text('void Actor::init() { initLayoutManager("Test", 1); }\n'
                             'void Actor::draw() { MR::hidePane(this, "TxtGood"); MR::startAnim(this,"Appear",0); MR::hidePane(other,"Bad"); MR::hidePane(this, dynamic); MR::setLayoutPosAtPaneTrans(this, other, "Bad"); }\n'
                             'void Other::draw() { MR::hidePane(this,"Bad"); }')
            inv = {'names': {'pane': {'TxtGood': ['Test']}, 'layout_animation': {'appear': ['Test']}, 'path': {}, 'file': {}},
                   'scopes': {'LayoutData/Test.arc': {'pane': ['TxtGood'], 'layout_animation': ['appear']}}, 'errors': []}
            refs, _, _ = audit.sweep_source(repo, inv)
            self.assertEqual(sum(r['status'] == 'present_scoped' for r in refs), 2)
            self.assertFalse(any(r['status'] == 'definite_mismatch' for r in refs))
            self.assertTrue(any(r['status'] == 'unverifiable_dynamic' for r in refs))
            actor.write_text(actor.read_text().replace('TxtGood', 'TxtGodo'))
            refs, _, _ = audit.sweep_source(repo, inv)
            self.assertEqual(sum(r['status'] == 'definite_mismatch' for r in refs), 1)
            inv['errors'] = [{'resource': 'LayoutData/Test.arc#0:x', 'error': 'truncated'}]
            refs, _, _ = audit.sweep_source(repo, inv)
            self.assertFalse(any(r['status'] == 'definite_mismatch' for r in refs))

    @unittest.skipUnless((ROOT / 'build/game-data/RMGE01/files/LayoutData/GalaxyNamePlate.arc').is_file(), 'disc not installed')
    def test_real_nameplate(self):
        archive = ROOT / 'build/game-data/RMGE01/files/LayoutData/GalaxyNamePlate.arc'
        names = [entry for _, name, data in audit.archive_files(archive.read_bytes()) for entry in audit.parse_resource(data, name)]
        self.assertIn(('pane', 'TxtGalaxyName'), names)
        self.assertIn(('pane', 'TxtGalaxyNameU'), names)
        self.assertNotIn(('pane', 'TxtGaxyName'), names)
        self.assertIn(('layout_animation', 'appear'), names)
        with tempfile.TemporaryDirectory() as temp:
            repo = Path(temp)
            source_path = repo / 'src/Game/Screen/GalaxyNamePlate.cpp'
            util_path = repo / 'src/Game/Util/LayoutUtil.cpp'
            source_path.parent.mkdir(parents=True)
            util_path.parent.mkdir(parents=True)
            util_path.write_text((ROOT / 'src/Game/Util/LayoutUtil.cpp').read_text())
            original = (ROOT / 'src/Game/Screen/GalaxyNamePlate.cpp').read_text()
            source_path.write_text(original)
            typed = {}
            for category, name in names:
                typed.setdefault(category, {})[name] = ['LayoutData/GalaxyNamePlate.arc']
            typed.update(path={}, file={})
            inv = {'names': typed, 'scopes': {'LayoutData/GalaxyNamePlate.arc': {k: list(v) for k, v in typed.items()}}, 'errors': []}
            refs, _, _ = audit.sweep_source(repo, inv)
            self.assertFalse(any(r['status'] == 'definite_mismatch' for r in refs))
            source_path.write_text(original.replace('TxtGalaxyName', 'TxtGaxyName'))
            refs, _, _ = audit.sweep_source(repo, inv)
            missing = [r['value'] for r in refs if r['status'] == 'definite_mismatch']
            self.assertEqual(sorted(missing), ['TxtGaxyName', 'TxtGaxyNameU'])



if __name__ == '__main__':
    unittest.main(verbosity=2)
