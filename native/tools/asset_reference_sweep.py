#!/usr/bin/env python3
"""Offline source/disc name audit. JSON preserves uncertain cases; no app is run.

Names found globally prove presence, not correct runtime ownership. Only a direct
this-actor layout lookup with one literal initLayoutManager is scoped strongly
enough to diagnose a definite missing name. The scanner is deliberately not a
C++ compiler: preprocessing, aliases, runtime construction and receiver identity
outside that narrow case remain explicit limitations.
"""
import argparse
from collections import Counter, defaultdict
import difflib
import hashlib
import json
from pathlib import Path
import re
import struct

from collect_pipeline_seed_inputs import (archive_files, bcsv_strings, decompress,
                                         field_hash, region, require, string, u16, u32)


def blocks(data, offset, count):
    for _ in range(count):
        size = u32(data, offset + 4)
        require(size >= 8, 'invalid block size')
        block = region(data, offset, size)
        yield block[:4], block
        offset += size


def name_table(block, offset):
    if not offset:
        return []
    count = u16(block, offset)
    region(block, offset + 4, count * 4)
    return [string(block, offset + u16(block, offset + 6 + i * 4)) for i in range(count)]


def parse_resource(data, filename):
    """Yield typed names from bounded format structures, never arbitrary strings."""
    magic = data[:4]
    if magic == b'AA_<':
        pos = 4
        sizes = {b'ws  ': 12, b'bnk ': 8, b'bl_<': 8, b'>_bl': 0,
                 b'bsc ': 8, b'bst ': 8, b'bstn': 8, b'bms ': 12,
                 b'bmsa': 4, b'vbnk': 8, b'dsqb': 4, b'bsft': 4, b'sect': 4}
        while True:
            command = region(data, pos, 4)
            pos += 4
            if command == b'>_AA':
                return
            require(command in sizes, 'unsupported audio archive command')
            region(data, pos, sizes[command])
            if command == b'bstn':
                start, end = u32(data, pos), u32(data, pos + 4)
                yield from parse_resource(region(data, start, end - start), 'sound.bstn')
            pos += sizes[command]
    if magic == b'BSTN':
        def offsets(base, start):
            count = u32(data, base)
            region(data, base + start, count * 4)
            return [u32(data, base + start + i * 4) for i in range(count)]
        for section in offsets(u32(data, 12), 4):
            if section:
                for group in offsets(section, 8):
                    if group:
                        for item in offsets(group, 8):
                            if item:
                                yield 'sound', string(data, item)
        return
    if magic in (b'RLYT', b'RLAN'):
        require(u16(data, 4) == 0xfeff and u32(data, 8) <= len(data), 'invalid layout header')
        for kind, block in blocks(data, u16(data, 12), u16(data, 14)):
            if magic == b'RLYT' and kind in (b'pan1', b'pic1', b'txt1', b'wnd1', b'bnd1'):
                yield 'pane', region(block, 12, 16).split(b'\0')[0].decode('ascii')
            elif magic == b'RLYT' and kind == b'grp1':
                yield 'group', region(block, 8, 16).split(b'\0')[0].decode('ascii')
            elif magic == b'RLAN' and kind == b'pai1':
                # AnimationInfo offsets are relative to pai1, not its offset table.
                count, table = u16(block, 14), u32(block, 16)
                for i in range(count):
                    pos = u32(block, table + 4 * i)
                    yield 'animation_target', region(block, pos, 20).split(b'\0')[0].decode('ascii')
        if magic == b'RLAN':
            yield 'layout_animation', Path(filename).stem
        return
    if magic == b'J3D2' and data[4:8] in (b'bdl4', b'bmd3'):
        require(u32(data, 8) <= len(data), 'truncated model')
        yield 'model', Path(filename).stem
        for kind, block in blocks(data, 32, u32(data, 12)):
            category = {b'JNT1': 'joint', b'MAT3': 'material', b'MAT2': 'material', b'TEX1': 'texture'}.get(kind)
            if category:
                offset = u32(block, 16 if kind == b'TEX1' else 20)
                for name in name_table(block, offset):
                    yield category, name
        return
    if data[:8] == b'MESGbmg1':
        require(u32(data, 8) <= len(data), 'truncated BMG')
        offset = 32
        for index in range(u32(data, 12)):
            kind, size = region(data, offset, 4), u32(data, offset + 4)
            require(size >= 8, 'invalid BMG block size')
            available = len(data) - offset
            if size > available:
                # RMGE01's final FLI1 declares alignment padding absent from RARC.
                # Accept only absent padding after every declared flow entry.
                require(kind == b'FLI1' and index == u32(data, 12) - 1
                        and 0 < size - available < 32
                        and 16 + u16(data, offset + 8) * 8 <= available,
                        'truncated BMG block contents')
                yield 'bmg_omitted_tail_padding', str(size - available)
            block = region(data, offset, min(size, available))
            offset += size
            if kind == b'INF1':
                count, stride = u16(block, 8), u16(block, 10)
                require(stride >= 4, 'invalid BMG entry size')
                region(block, 16, count * stride)
                yield 'bmg_entries', str(count)
            elif kind == b'MID1':
                for i in range(u16(block, 8)):
                    yield 'message_numeric_id', str(u32(block, 16 + i * 4))
        return
    if magic in (b'J3D1', b'J3D2') and data[4:7] in (b'bck', b'btk', b'brk', b'btp', b'bpk', b'bva', b'bca'):
        require(u32(data, 8) <= len(data), 'truncated model animation')
        # Presence by typed file header; animation payloads are not decoded.
        yield 'model_animation', Path(filename).stem
        return
    # BCSV has no magic; recognize extensionless tables only with a valid header.
    if len(data) < 16:
        return
    count, fields, offset, stride = (u32(data, x) for x in (0, 4, 8, 12))
    looks_table = (0 < fields < 4096 and 16 + fields * 12 <= offset <= len(data)
                   and 0 < stride < 65536 and offset + count * stride <= len(data))
    if filename.lower().endswith(('.bcsv', '.tbl')) or looks_table:
        require(looks_table, 'invalid BCSV header')
        for i in range(fields):
            yield 'field_hash', str(u32(data, 16 + 12 * i))
        for row in bcsv_strings(data):
            for key, value in row.items():
                yield 'table_string', value
                if key == field_hash('MessageId'):
                    yield 'message', value
                if filename.lower() == 'particlenames.bcsv' and key == field_hash('name'):
                    yield 'particle', value
                if key in (field_hash('EffectName'), field_hash('UniqueName')):
                    for part in value.split(' '):
                        if part:
                            yield 'effect', part


def inventory(files):
    names = defaultdict(lambda: defaultdict(set))
    scopes = defaultdict(lambda: defaultdict(set))
    errors, counts, hashes = [], Counter(), {}
    for path in sorted(files.rglob('*')):
        if not path.is_file():
            continue
        relative = path.relative_to(files).as_posix()
        names['path'][relative].add(relative)
        names['file'][path.name].add(relative)
        if path.suffix.lower() == '.arc':
            names['archive'][path.stem].add(relative)
        if path.suffix.lower() not in ('.arc', '.szs', '.bstn', '.brlyt', '.brlan', '.bmg', '.bcsv', '.tbl', '.bdl', '.bmd'):
            continue
        raw = path.read_bytes()
        hashes[relative] = hashlib.sha256(raw).hexdigest()
        try:
            data = decompress(raw)
            resources = archive_files(data) if data[:4] == b'RARC' else [(0, path.name, data)]
            for index, filename, payload in resources:
                location = f'{relative}#{index}:{filename}'
                names['file'][filename].add(location)
                counts['resources'] += 1
                counts[payload[:8].decode('ascii', errors='replace') if payload[:4] in (b'RLYT', b'RLAN', b'J3D2', b'MESG') else 'other'] += 1
                try:
                    for category, name in parse_resource(payload, filename):
                        names[category][name].add(location)
                        scopes[relative][category].add(name)
                except (ValueError, IndexError, struct.error, UnicodeError) as exc:
                    errors.append({'resource': location, 'error': str(exc)})
        except (ValueError, IndexError, struct.error, UnicodeError) as exc:
            errors.append({'resource': relative, 'error': str(exc)})
    return {'names': {k: {n: sorted(v) for n, v in ns.items()} for k, ns in names.items()},
            'scopes': {k: {c: sorted(v) for c, v in cs.items()} for k, cs in scopes.items()},
            'errors': errors, 'counts': dict(counts), 'asset_sha256': hashes}


TOKEN = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|(?:u8|L|u|U)?"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[A-Za-z_]\w*|::|->|[^\s]')
STRING = re.compile(r'(?:u8|L|u|U)?"((?:\\.|[^"\\])*)"\Z')


def tokens(source):
    return [(m.group(), m.start()) for m in TOKEN.finditer(source) if not m.group().startswith(('//', '/*'))]


def calls(ts):
    for i in range(len(ts) - 1):
        if not re.fullmatch(r'[A-Za-z_]\w*', ts[i][0]) or ts[i + 1][0] != '(':
            continue
        args, current, depth = [], [], 0
        for token, pos in ts[i + 2:]:
            if token == ')' and depth == 0:
                args.append(current)
                yield ts[i][0], args, ts[i][1]
                break
            if token == ',' and depth == 0:
                args.append(current)
                current = []
                continue
            if token in ('(', '[', '{'):
                depth += 1
            elif token in (')', ']', '}'):
                depth -= 1
            current.append(token)


def class_functions(ts):
    """Conservative spans for out-of-line member definitions (not declarations)."""
    spans = []
    for i in range(2, len(ts) - 1):
        if ts[i - 1][0] != '::' or ts[i + 1][0] != '(':
            continue
        depth, j = 1, i + 2
        while j < len(ts) and depth:
            depth += (ts[j][0] == '(') - (ts[j][0] == ')')
            j += 1
        while j < len(ts) and ts[j][0] in ('const', 'noexcept', 'override', 'final'):
            j += 1
        if j < len(ts) and ts[j][0] == ':':
            j += 1
            nesting = 0
            while j < len(ts):
                token = ts[j][0]
                if token in (';', '{') and nesting == 0:
                    break
                nesting += (token == '(') - (token == ')')
                j += 1
        if j == len(ts) or ts[j][0] != '{':
            continue
        start, depth = ts[j][1], 1
        j += 1
        while j < len(ts) and depth:
            depth += (ts[j][0] == '{') - (ts[j][0] == '}')
            j += 1
        if depth == 0:
            spans.append((start, ts[j - 1][1], ts[i - 2][0]))
    return spans


def literal(arg):
    if arg and all(STRING.fullmatch(t) for t in arg):
        return ''.join(STRING.fullmatch(t)[1] for t in arg)
    return None


def assigned_literals(arg, source, pos, spans):
    """Resolve one local assignment, limited to literals or literal ternary arms."""
    expression = ' '.join(arg)
    if len(arg) == 1 and re.fullmatch(r'[A-Za-z_]\w*', arg[0]):
        span = next(((start, end) for start, end, _ in spans if start <= pos <= end), None)
        if span is None:
            return []
        assignments = list(re.finditer(r'\b' + re.escape(arg[0]) + r'\s*=(?!=)\s*([^;]+);', source[span[0]:pos]))
        if not assignments:
            formats = [literal(args[2]) for fn, args, _ in calls(tokens(source[span[0]:pos]))
                       if fn in ('snprintf', 'sprintf') and len(args) > 2 and args[0] == arg
                       and fn == 'snprintf']
            if len(formats) == 1 and formats[0] is not None:
                return formats
            return []
        if len(assignments) != 1:
            return []
        expression = assignments[0][1]
    ts = [token for token, _ in tokens(expression)]
    direct = literal(ts)
    if direct is not None:
        return [direct]
    if ts.count('?') == 1 and ts.count(':') == 1:
        question, colon = ts.index('?'), ts.index(':')
        left, right = literal(ts[question + 1:colon]), literal(ts[colon + 1:])
        if left is not None and right is not None:
            return sorted({left, right})
    return []


def format_pattern(value):
    out, pos = '', 0
    fmt = re.compile(r'%[-+ #0]*\d*(?:\.\d+)?(?:hh|ll|[hljztL])?([diuoxXfFeEgGaAcsp%])')
    for match in fmt.finditer(value):
        out += re.escape(value[pos:match.start()])
        code = match[1]
        out += {'s': '.*', 'd': '-?\\d+', 'i': '-?\\d+', 'u': '\\d+', 'x': '[0-9a-f]+',
                'X': '[0-9A-F]+', 'o': '[0-7]+', 'c': '.', '%': '%'}.get(code, '.+')
        pos = match.end()
    return re.compile('^' + out + re.escape(value[pos:]) + '$')


def api_rules(repo):
    rules = defaultdict(list)
    # Parameter names in the utility definitions provide the argument position.
    source = (repo / 'src/Game/Util/LayoutUtil.cpp').read_text(errors='replace')
    for match in re.finditer(r'\b(\w+)\s*\(([^(){};]*)\)\s*\{', source):
        fn, params = match.groups()
        for i, param in enumerate(params.split(',')):
            if 'char*' not in param.replace(' *', '*'):
                continue
            if re.search(r'\bpPaneName\b', param):
                rules[fn].append((i, 'pane'))
            elif re.search(r'\bpGroupName\b', param):
                rules[fn].append((i, 'group'))
            elif re.search(r'\bpAnimName\b', param):
                rules[fn].append((i, 'layout_animation'))
            elif re.search(r'\bpMessageId\b', param):
                rules[fn].append((i, 'message'))
    for fn in ('startAnim', 'startAnimAtFirstStep', 'isAnimStopped', 'isAnimStoppedAtFirstStep'):
        if fn.startswith('start'):
            rules[fn] = [(1, 'layout_animation')]
    for fn in ('getGameMessageDirect', 'getSystemMessageDirect', 'getLayoutMessageDirect', 'isExistGameMessage'):
        rules[fn] = [(0, 'message')]
    for fn in ('getJointMtx', 'getJointPos', 'getJointIndex', 'isExistJoint', 'hideMaterial', 'showMaterial'):
        rules[fn] = [(1, 'material' if 'Material' in fn else 'joint')]
    for fn in ('emitEffect', 'deleteEffect', 'isEffectValid', 'emitEffectWithScale'):
        rules[fn] = [(1, 'effect')]
    for fn in ('startSound', 'startLevelSound', 'startSystemSE'):
        rules[fn] = [(0 if fn == 'startSystemSE' else 1, 'sound')]
    for fn in ('startBck', 'startBckNoInterpole', 'startBtk', 'startBrk', 'startBtp', 'startBpk', 'startBva', 'isBckPlaying'):
        rules[fn] = [(1, 'model_animation')]
    rules['initLayoutManager'] = [(0, 'archive')]
    rules['initModelManagerWithAnm'] = [(0, 'archive')]
    return rules


def sweep_source(repo, inv):
    rules, results, hashes, literal_count = api_rules(repo), [], {}, 0
    receivers = {}
    util = (repo / 'src/Game/Util/LayoutUtil.cpp').read_text(errors='replace')
    for match in re.finditer(r'\b(\w+)\s*\(([^(){};]*)\)\s*\{', util):
        params = match[2].split(',')
        for target in ('pActor', 'pFollowActor'):
            for i, param in enumerate(params):
                if re.search(r'\b' + target + r'\b', param):
                    receivers[match[1]] = i
    for path in sorted((repo / 'src').rglob('*')):
        if path.suffix not in ('.cpp', '.hpp', '.h', '.c'):
            continue
        raw = path.read_bytes()
        source = raw.decode('utf-8', errors='replace')
        relative = path.relative_to(repo).as_posix()
        hashes[relative] = hashlib.sha256(raw).hexdigest()
        ts = tokens(source)
        literal_count += sum(bool(STRING.fullmatch(t)) for t, _ in ts)
        parsed_calls = list(calls(ts))
        spans = class_functions(ts)
        def owner(pos):
            return next((name for start, end, name in spans if start <= pos <= end), None)
        actor_layouts = defaultdict(set)
        for fn, args, pos in parsed_calls:
            if fn == 'initLayoutManager' and args and owner(pos):
                actor_layouts[owner(pos)].add(literal(args[0]))
        for fn, args, pos in parsed_calls:
            layouts = actor_layouts.get(owner(pos), set())
            qualified = re.search(r'MR\s*::\s*$', source[max(0, pos - 20):pos])
            specs = list(rules.get(fn, [])) if qualified or fn.startswith('init') else []
            if fn in ('getValue', 'findElement', 'findElementBinary', 'getCsvDataStr', 'getCsvDataS32', 'getCsvDataF32'):
                specs += [(i, 'field_hash') for i, arg in enumerate(args) if literal(arg) is not None]
            expanded = []
            for index, category in specs:
                if index >= len(args):
                    continue
                direct = literal(args[index])
                values = [direct] if direct is not None else assigned_literals(args[index], source, pos, spans)
                expanded.extend((index, category, value, direct is None and value is not None) for value in (values or [None]))
            for index, category, value, derived in expanded:
                record = {'source': relative, 'line': source.count('\n', 0, pos) + 1,
                          'api': fn, 'argument': index, 'category': category,
                          'expression': ' '.join(args[index])}
                if value is None:
                    record.update(status='unverifiable_dynamic', confidence='none', reason='nonliteral argument; no C++ dataflow inference')
                    results.append(record)
                    continue
                record['value'] = value
                if derived:
                    record['resolution'] = 'local literal assignment, literal ternary arms, or snprintf format'
                pool = inv['names'].get(category, {})
                scope = None
                # Restrict scoped evidence to this receiver and one explicit layout.
                if category in ('pane', 'group', 'layout_animation') and receivers.get(fn, 0) < len(args) and args[receivers.get(fn, 0)] == ['this'] and len(layouts) == 1 and None not in layouts:
                    layout = next(iter(layouts))
                    candidates = [p for p in inv['scopes'] if p.endswith('/' + layout + '.arc') and 'LayoutData/' in p]
                    if candidates:
                        scope = candidates
                        pool = {name: [p for p in candidates if name in inv['scopes'][p].get(category, [])]
                                for p in candidates for name in inv['scopes'][p].get(category, [])}
                        record['scope'] = candidates
                if category in ('layout_animation', 'model', 'model_animation'):
                    pool = {k.lower(): v for k, v in pool.items()}
                    key_value = value.lower()
                else:
                    key_value = value
                key = str(field_hash(value)) if category == 'field_hash' and value.isascii() else key_value
                if '%' in value:
                    matched = sorted(n for n in pool if format_pattern(key_value).fullmatch(n))
                    record.update(status='pattern_candidates' if matched else 'unverifiable_dynamic', confidence='low',
                                  matches=matched, reason='format arguments and reachable expansions unknown')
                elif key in pool:
                    record.update(status='present_scoped' if scope else 'present_global', confidence='high' if scope else 'medium', evidence=pool[key])
                else:
                    sound_registry = category == 'sound' and bool(pool) and bool(re.match(r'^(?:SE_|BGM_|STM_)', value)) and not any(e['resource'].startswith('AudioRes/') for e in inv['errors'])
                    definite = sound_registry or bool(scope) and not any(any(e['resource'].startswith(p) for p in scope) for e in inv['errors'])
                    record.update(status='definite_mismatch' if definite else 'unverifiable_missing', confidence='high' if definite else 'low',
                                  suggestions=difflib.get_close_matches(value, list(pool), n=5),
                                  reason='absent in complete BSTN sound registry' if sound_registry else 'absent in explicit layout archive(s)' if definite else 'missing global name; receiver, optional lookup, unsupported format or runtime registration unknown')
                results.append(record)
        # Archive/file literals outside known APIs, including printf patterns.
        sound_calls = {(r['line'], r.get('value')) for r in results if r['source'] == relative and r['category'] == 'sound'}
        for token, pos in ts:
            value = literal([token])
            if value and re.match(r'^(?:SE_|BGM_|STM_)[A-Z0-9_%]+$', value) and (source.count('\n', 0, pos) + 1, value) not in sound_calls:
                pool = inv['names'].get('sound', {})
                matches = sorted(n for n in pool if format_pattern(value).fullmatch(n)) if '%' in value else ([value] if value in pool else [])
                complete = bool(pool) and not any(e['resource'].startswith('AudioRes/') for e in inv['errors'])
                results.append({'source': relative, 'line': source.count('\n', 0, pos) + 1, 'category': 'sound_literal', 'value': value,
                                'status': 'pattern_candidates' if '%' in value and matches else 'unverifiable_dynamic' if '%' in value else 'present_global' if matches else 'definite_mismatch' if complete else 'unverifiable_missing',
                                'confidence': 'low' if '%' in value else 'high' if complete else 'low',
                                'evidence': [p for n in matches for p in pool[n]],
                                'suggestions': [] if matches else difflib.get_close_matches(value, list(pool), n=3)})
            if not value or not re.search(r'\.(?:arc|bdl|bmd|brlyt|brlan|bcsv|tbl|bmg|thp)$', value, re.I):
                continue
            pool = inv['names']['path'] if '/' in value else inv['names']['file']
            pool = {k.lower(): v for k, v in pool.items()}
            normalized = value.lstrip('/').lower()
            matches = sorted(n for n in pool if format_pattern(normalized).fullmatch(n)) if '%' in value else ([normalized] if normalized in pool else [])
            results.append({'source': relative, 'line': source.count('\n', 0, pos) + 1, 'category': 'file_path', 'value': value,
                            'status': 'pattern_candidates' if matches and '%' in value else 'present_global' if matches else 'unverifiable_missing',
                            'confidence': 'low' if '%' in value or not matches else 'medium', 'matches': matches})
    return results, hashes, literal_count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--files', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--inventory', type=Path, help='write reusable inventory, or reuse with --reuse-inventory')
    parser.add_argument('--reuse-inventory', action='store_true', help='explicitly reuse snapshot; does not validate changed assets')
    args = parser.parse_args()
    files = args.files or args.repo / 'build/game-data/RMGE01/files'
    require(files.is_dir(), 'disc files directory missing')
    if args.reuse_inventory:
        require(args.inventory and args.inventory.is_file(), 'inventory snapshot required')
        inv = json.loads(args.inventory.read_text())
    else:
        inv = inventory(files)
        if args.inventory:
            args.inventory.write_text(json.dumps(inv, indent=2, sort_keys=True) + '\n')
    refs, hashes, literals = sweep_source(args.repo, inv)
    report = {'schema': 1, 'files_root': str(files.resolve()), 'source_sha256': hashes,
              'asset_sha256': inv['asset_sha256'], 'inventory_reused': args.reuse_inventory,
              'summary': dict(Counter(r['status'] for r in refs)), 'source_files': len(hashes), 'string_literals': literals,
              'inventory_counts': inv['counts'], 'name_counts': {k: len(v) for k, v in inv['names'].items()},
              'parse_errors': inv['errors'], 'references': refs,
              'limitations': ['Lexical scanner, not preprocessed C++ or interprocedural dataflow; unrelated literals are not classified.',
                              'Global presence is not proof of correct resource ownership; definite layout mismatches still require intent review.',
                              'Sound names are parsed from BSTN; runtime effect registration remains incomplete.',
                              'BCSV uses 32-bit field hashes; matches may collide and do not prove per-table presence.',
                              'BMG numeric IDs and entry counts are parsed; string message IDs come from MessageId.tbl.',
                              'RARC file basenames are indexed; internal directory-qualified paths are not resolved.',
                              'Formatted candidates do not establish all reachable expansions.']}
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
    print(json.dumps({k: report[k] for k in ('summary', 'source_files', 'string_literals', 'name_counts', 'parse_errors')}, indent=2))


if __name__ == '__main__':
    main()
