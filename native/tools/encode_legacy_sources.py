#!/usr/bin/env python3
"""Emit Clang-readable sources whose legacy narrow literals contain Shift-JIS bytes.

The Wii build uses sjiswrap (encoding_rs SHIFT_JIS). Python's cp932 codec covers
the corresponding Windows extensions used by the sources. Legacy wide types and
literals become UTF-16; host system headers retain their native ABI. Comments,
explicit Unicode literals and handwritten byte escapes remain unchanged.
"""
import argparse
import json
from pathlib import Path
import re

TOKEN = re.compile(
    r'(?P<include>^[ \t]*\#[ \t]*include[^\n]*)'
    r'|(?P<comment>//(?:[^\\\n]|\\[^\n]|\\\n)*|/\*[\s\S]*?\*/)'
    r'|(?P<raw>(?:u8|u|U|L)?R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\([\s\S]*?\)(?P=delimiter)")'
    r'|(?P<literal>(?:u8|u|U|L)?(?:"(?:\\[\s\S]|[^"\\\n])*"|\'(?:\\[\s\S]|[^\'\\\n])*\'))'
    r'|(?P<identifier>\b[A-Za-z_][A-Za-z_0-9]*\b)', re.MULTILINE
)
WIDE_FUNCTIONS = {'wcslen', 'wcscpy', 'wcsncpy', 'wcscmp', 'wcschr', 'swprintf', 'vswprintf'}
ESCAPE_OR_CHARACTER = re.compile(r'\\(?:u[0-9a-fA-F]{4}|U[0-9a-fA-F]{8}|[\s\S])|[^\\]')
EXTENSIONS = {'.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.inc'}


def octal_bytes(text):
    # Match encoding_rs's mappings for the two legacy Unicode aliases.
    text = text.replace('\u00a5', '\\').replace('\u203e', '~')
    return ''.join(f'\\{byte:03o}' for byte in text.encode('cp932'))


def transform(source):
    def token(match):
        text = match.group()
        if match.group('include') is not None:
            return text
        if match.group('identifier') is not None:
            if text == 'wchar_t':
                return 'PetariChar16'
            if text in WIDE_FUNCTIONS:
                return 'petari_utf16_' + text
            return text
        if match.lastgroup == 'comment' or match.group('comment') is not None:
            return text
        if text.startswith('L'):
            return 'u' + text[1:]
        if text.startswith(('u', 'U')):
            return text
        if match.group('raw') is not None:
            delimiter = match.group('delimiter')
            body = text[3 + len(delimiter):-(2 + len(delimiter))]
            if body.isascii():
                return text
            return '"' + octal_bytes(body) + '"' + '\\\n' * body.count('\n')
        quote, body = text[0], text[1:-1]
        def character(piece):
            value = piece.group()
            if value.startswith(('\\u', '\\U')) and len(value) > 2:
                return octal_bytes(chr(int(value[2:], 16)))
            if value.startswith('\\') or value.isascii():
                return value
            return octal_bytes(value)
        return quote + ESCAPE_OR_CHARACTER.sub(character, body) + quote
    return TOKEN.sub(token, source)


def source_files(root):
    for directory in ('src', 'include', 'libs'):
        for path in sorted((root / directory).rglob('*')):
            if path.is_file() and path.suffix in EXTENSIONS:
                yield path


def prepare(root, output):
    count = 0
    for path in source_files(root):
        original = path.read_text(encoding='utf-8')
        try:
            encoded = transform(original)
        except UnicodeEncodeError as error:
            raise ValueError(f'{path}: literal is not representable in Shift-JIS: {error}') from error
        destination = output / path.relative_to(root)
        content = '#include <petari/utf16.h>\n#line 1 ' + json.dumps(str(path), ensure_ascii=False) + '\n' + encoded
        if not destination.exists() or destination.read_text(encoding='utf-8') != content:
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(content, encoding='utf-8')
        count += encoded != original
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    count = prepare(args.root.resolve(), args.output.resolve())
    print(f'Prepared native legacy sources; encoded literals in {count} files.')


if __name__ == '__main__':
    main()
