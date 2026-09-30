#!/usr/bin/env python3
"""Heuristic stub finder for native/STUBS.md.

Lists function definitions whose body is empty, only returns a constant, only
(void)-casts its arguments, or is short and carries a TODO/stub/no-op marker.
Constructors with initializer lists show up as EMPTY; skip those by eye.

Usage: native/tools/stub_scan.py DIR... (tests/ and tools/ directories are skipped)
"""
import re, os, sys
SIG = re.compile(r'^[ \t]*((?:extern\s+"C"\s+)?(?:static\s+|inline\s+|virtual\s+|constexpr\s+)*[\w:<>\*&\s,]+?\b([~\w:]+)\s*\(([^;{}]*)\)\s*(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?)\{', re.M)
CONST_RET = re.compile(r'^\s*return\s+(?:\(?\s*(?:[-+]?\d[\w.]*|true|false|nullptr|NULL|0x[0-9a-fA-F]+|[A-Z][A-Z0-9_]+|\w+::[A-Z]\w*)\s*\)?)\s*;\s*$')
MARK = re.compile(r'TODO|FIXME|stub|STUB|unimplemented|not implemented|Not implemented|no-op|noop|nothing to do|intentionally empty|ignored', re.I)
def body_of(text, start):
    depth, i = 1, start
    while i < len(text) and depth:
        c = text[i]
        if c == '{': depth += 1
        elif c == '}': depth -= 1
        i += 1
    return text[start:i-1]
def strip_comments(s):
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    return re.sub(r'//[^\n]*', '', s)
out = []
for root in sys.argv[1:]:
    for dp, dn, fn in os.walk(root):
        if '/tests' in dp or '/tools' in dp or dp.endswith('tests'): continue
        for f in fn:
            if not f.endswith(('.cpp', '.c', '.mm', '.inc', '.hpp', '.h')): continue
            p = os.path.join(dp, f); text = open(p, errors='replace').read()
            for m in SIG.finditer(text):
                name = m.group(2)
                if name in ('if', 'for', 'while', 'switch', 'catch', 'return', 'sizeof', 'defined'): continue
                body = body_of(text, m.end())
                code = strip_comments(body).strip()
                stmts = [s.strip() for s in code.split(';') if s.strip()]
                kind = None
                if not code: kind = 'EMPTY'
                elif len(stmts) == 1 and CONST_RET.match(code): kind = 'CONST:' + code.replace('\n', ' ')[:40]
                elif stmts and all(re.fullmatch(r'\(void\)\s*\w+', s) for s in stmts): kind = 'VOIDCAST'
                elif stmts and all(re.fullmatch(r'\(void\)\s*\w+', s) or CONST_RET.match(s + ';') for s in stmts): kind = 'VOIDCAST+CONST'
                if kind is None and MARK.search(body) and len(code) < 400: kind = 'MARKED'
                if kind:
                    line = text.count('\n', 0, m.start()) + 1
                    out.append(f'{p}:{line}\t{kind}\t{name}({m.group(3).strip()[:60]})')
print('\n'.join(out))
