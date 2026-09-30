#!/usr/bin/env python3
"""Conservative stage dependencies from game factory tables and actor sources.

Literal archive candidates in a selected actor/callback translation unit include
all conditional branches. This is not a C++ interpreter: constructed resource
names and indirect calls can still require observed supplementation.
"""
from collections import defaultdict
import hashlib
from pathlib import Path
import re

TOKEN = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/')


def uncomment(text):
    return TOKEN.sub(lambda m: re.sub(r'[^\n]', ' ', m[0]) if m[0].startswith('/') else m[0], text)


def table(text, name):
    match = re.search(r'\b' + re.escape(name) + r'\s*\[\s*\]\s*=\s*\{', text)
    if not match:
        raise ValueError('dependency table missing: ' + name)
    # Mask strings while preserving offsets so braces in literals cannot end a table.
    masked = TOKEN.sub(lambda m: ' ' * len(m[0]), text)
    depth, start = 1, match.end()
    for index in range(start, len(text)):
        depth += (masked[index] == '{') - (masked[index] == '}')
        if not depth:
            return text[start:index], start
    raise ValueError('unterminated dependency table: ' + name)


class DependencyIndex:
    def __init__(self, files, repo):
        self.files, self.repo = files, repo
        self.catalog = defaultdict(list)
        self.archive_paths = {}
        for directory in ('ObjectData', 'LayoutData'):
            for path in sorted((files / directory).glob('*.arc')):
                self.catalog[path.stem.casefold()].append(path)
                self.archive_paths[str(path.relative_to(files)).casefold()] = path
        self.needs, self.classes, self.callbacks = defaultdict(list), {}, defaultdict(list)
        self.sources, self.units, self.shared = {}, defaultdict(list), []
        source_paths = sorted((repo / 'src/Game').rglob('*.cpp'))
        callback_names = set()
        for relative, pair_table, creator_table, callback_table in (
            ('src/Game/NameObj/NameObjFactory.cpp', 'cName2ArchiveNamesTable', 'cCreateTable', 'cName2MakeArchiveListFuncTable'),
            ('src/Game/Map/PlanetMapCreator.cpp', 'sUniquePlanetUniqueArchiveName', 'sUniquePlanetCreateFuncTable', None),
        ):
            path = repo / relative
            text = uncomment(path.read_text())
            self.sources[relative] = hashlib.sha256(path.read_bytes()).hexdigest()
            body, offset = table(text, pair_table)
            for match in re.finditer(r'\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,?\s*\}', body):
                self.needs[match[1].casefold()].append((match[2], f'{relative}:{text.count(chr(10), 0, offset + match.start()) + 1} {pair_table}'))
            body, _ = table(text, creator_table)
            for match in re.finditer(r'\{\s*"([^"]+)"\s*,\s*createNameObj\s*<\s*(\w+)\s*>', body):
                self.classes[match[1].casefold()] = match[2]
            if callback_table:
                body, _ = table(text, callback_table)
                for actor, callback in re.findall(r'\{\s*"([^"]+)"\s*,\s*([\w:]+)\s*,?\s*\}', body):
                    self.callbacks[actor.casefold()].append(callback)
                    callback_names.add(callback.rsplit('::', 1)[-1])
        self.code = {}
        self.callback_units = defaultdict(list)
        for path in source_paths:
            text = uncomment(path.read_text())
            relative = str(path.relative_to(repo))
            self.code[relative] = text
            self.units[path.stem.casefold()].append(relative)
            # Callback definitions have a collector parameter; declarations/calls
            # ending in semicolons cannot match this definition pattern.
            for name in callback_names:
                if re.search(r'\b' + re.escape(name) + r'\s*\([^;{}]*NameObjArchiveListCollector[^;{}]*\)\s*\{', text):
                    self.callback_units[name].append(relative)
        stationed = 'src/Game/System/StationedFileInfo.cpp'
        text = self.code[stationed]
        for match in re.finditer(r'"(/(?:ObjectData|LayoutData)/[^"]+\.arc)"', text):
            path = self.archive_paths.get(match[1].lstrip('/').casefold())
            if path is not None:
                self.shared.append((path, f'{stationed}:{text.count(chr(10), 0, match.start()) + 1} stationed shared resource'))
        self.sources[stationed] = hashlib.sha256((repo / stationed).read_bytes()).hexdigest()
        self.cache = {}

    def resolve(self, actor):
        key = actor.casefold()
        if key in self.cache:
            return self.cache[key]
        found, visited, unresolved = {}, set(), set()
        queue = [('actor', actor, 'placed actor ' + actor)]

        def add_name(name, reason):
            name = Path(name).name
            if name.lower().endswith('.arc'):
                name = name[:-4]
            for path in self.catalog.get(name.casefold(), []):
                found.setdefault(path, set()).add(reason)

        while queue:
            kind, name, reason = queue.pop()
            identity = (kind, name.casefold())
            if identity in visited:
                continue
            visited.add(identity)
            if kind == 'actor':
                for child, evidence in self.needs.get(name.casefold(), []):
                    add_name(child, evidence)
                    queue.append(('actor', child, evidence))
                cls = self.classes.get(name.casefold(), name)
                for unit in self.units.get(cls.casefold(), []):
                    queue.append(('unit', unit, reason + ' -> ' + cls))
                for callback in self.callbacks.get(name.casefold(), []):
                    owner, method = callback.rsplit('::', 1)
                    units = self.units.get(owner.casefold(), []) if owner != 'MR' else self.callback_units.get(method, [])
                    if not units:
                        unresolved.add(callback)
                    for unit in units:
                        queue.append(('unit', unit, 'callback ' + callback))
            else:
                text = self.code[name]
                self.sources[name] = hashlib.sha256((self.repo / name).read_bytes()).hexdigest()
                for match in re.finditer(r'"([^"\n]+)"', text):
                    evidence = f'{name}:{text.count(chr(10), 0, match.start()) + 1} conservative actor/callback literal ({reason})'
                    add_name(match[1], evidence)
                for child in re.findall(r'\bnew\s+([A-Za-z_]\w*)\s*\(', text):
                    if child.casefold() in self.units:
                        queue.append(('actor', child, name + ' constructs ' + child))
        result = [(path, sorted(reasons)) for path, reasons in sorted(found.items())], sorted(unresolved)
        self.cache[key] = result
        return result
