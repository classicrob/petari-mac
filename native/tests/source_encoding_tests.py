import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'tools/encode_legacy_sources.py'
spec = importlib.util.spec_from_file_location('legacy_encoding', SCRIPT)
encoding = importlib.util.module_from_spec(spec)
spec.loader.exec_module(encoding)


class EncodingTests(unittest.TestCase):
    def test_comments_and_unicode_literals(self):
        source = '// "壁押し"\n/* "日本" */ L"日本" u"日本" U"日本" u8"日本"'
        self.assertEqual(encoding.transform(source), source.replace('L"', 'u"'))

    def test_utf16_tokens(self):
        source = '#include <wchar_t.h>\n// wchar_t swprintf L\'x\'\nconst wchar_t* value = L"日本"; wchar_t ch = L\'x\'; swprintf(value);'
        converted = encoding.transform(source)
        self.assertIn('#include <wchar_t.h>', converted)
        self.assertIn('// wchar_t swprintf L\'x\'', converted)
        self.assertIn('const PetariChar16* value = u"日本"', converted)
        self.assertIn("PetariChar16 ch = u'x'", converted)
        self.assertIn('petari_utf16_swprintf(value)', converted)

    def test_utf16_linkage_and_host_abi(self):
        source = '''
#include <cwchar>
extern int swprintf(wchar_t*, size_t, const wchar_t*, ...);
int main() {
    wchar_t result[20];
    static_assert(sizeof(wchar_t) == 2);
    swprintf(result, 20, L"%ls:%d", L"日本", 42);
    return wcscmp(result, L"日本:42");
}
'''
        native = SCRIPT.parents[1]
        with tempfile.TemporaryDirectory(prefix='petari-utf16-') as temp:
            path = Path(temp) / 'test.cpp'
            # Check the host ABI outside the transformed legacy source.
            path.write_text('#include <petari/utf16.h>\nstatic_assert(sizeof(wchar_t) == 4);\n' + encoding.transform(source))
            binary = Path(temp) / 'test'
            subprocess.run(['clang++', '-std=c++17', '-I', str(native / 'include'), str(path),
                            str(native / 'text/utf16.cpp'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_escapes_and_adjacent_digits(self):
        converted = encoding.transform(r'"壁1\\\"\n\x80" "\u62bc"')
        self.assertIn(r'\225\3071', converted)
        self.assertIn(r'\\\"\n\x80', converted)
        self.assertNotIn('壁', converted)
        self.assertIn(encoding.octal_bytes('押'), converted)

    def test_raw_literal(self):
        converted = encoding.transform('R"tag(日本\n"x")tag"')
        self.assertEqual(converted.count('\n'), 1)
        self.assertIn(encoding.octal_bytes('日本\n"x"'), converted)

    def test_compiled_bytes(self):
        source = r'''
#include <cstring>
int main() {
    const unsigned char expected[] = {0x95, 0xc7, 0x89, 0x9f, 0x82, 0xb5, '1', 0};
    const char* text = "壁押し1";
    const char* escaped = "\u58c1\u62bc\u3057" "1";
    return std::memcmp(text, expected, sizeof(expected)) ||
           std::memcmp(escaped, expected, sizeof(expected));
}
'''
        with tempfile.TemporaryDirectory(prefix='petari-encoding-') as temp:
            path = Path(temp) / 'test.cpp'
            path.write_text(encoding.transform(source), encoding='utf-8')
            binary = Path(temp) / 'test'
            subprocess.run(['clang++', '-std=c++17', str(path), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
