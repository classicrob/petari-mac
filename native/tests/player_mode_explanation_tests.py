#!/usr/bin/env python3
"""Run MarioActor's actual explanation dispatch against the PlayerMode declaration."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def block(source, start):
    opening = source.index('{', start)
    end = opening + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    header = (ROOT / 'include/Game/Player/MarioActor.hpp').read_text()
    enum = block(header, header.index('enum PlayerMode {')) + ';'
    actor = (ROOT / 'src/Game/Player/MarioActor.cpp').read_text()
    call = actor.index('MR::explainBeeMarioIfAtFirst();')
    switch = block(actor, actor.rfind('switch (mPlayerMode)', 0, call))
    kinds = ['None', 'Bee', 'Teresa', 'Hopper', 'Fire', 'Ice', 'Flying', 'Invincible']
    code = '#include <cstdio>\n' + enum
    code += '\nint result=0, calls=0; namespace MR {\n'
    for index, kind in enumerate(kinds[1:], 1):
        code += f'void explain{kind}MarioIfAtFirst() {{ result={index}; ++calls; }}\n'
    code += '}\nvoid explain(int mPlayerMode) {' + switch + '}\n'
    # Expected presentation follows the power-up, independently of ordinal order.
    cases = {'PlayerMode_Normal': 'None', 'PlayerMode_Invincible': 'Invincible',
             'PlayerMode_2': 'Fire', 'PlayerMode_Ice': 'Ice', 'PlayerMode_Bee': 'Bee',
             'PlayerMode_Hopper': 'Hopper', 'PlayerMode_Teresa': 'Teresa',
             'PlayerMode_Foo': 'Flying', 'PlayerMode_8': 'None', 'PlayerMode_Tornado': 'None',
             '-1': 'None', '100': 'None'}
    code += 'int main() {\n'
    for mode, kind in cases.items():
        code += (f'result=0; calls=0; explain({mode}); '
                 f'if(result!={kinds.index(kind)} || calls!={int(kind != "None")}) '
                 f'{{ std::fprintf(stderr,"FAIL {mode}: expected {kind}\\n"); return 1; }}\n')
    code += 'std::puts("PASS 12 player-mode explanation mappings"); }\n'
    compiler = sys.argv[1] if len(sys.argv) > 1 else 'clang++'
    sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
    with tempfile.TemporaryDirectory(prefix='petari-explanation-') as directory:
        cpp = Path(directory) / 'test.cpp'
        binary = Path(directory) / 'test'
        cpp.write_text(code)
        subprocess.run([compiler, '-isysroot', sdk, '-std=c++17', str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        # Restore the old order: this must detect Spring producing the Ice tip.
        old = switch
        for name, value in [('Bee', 1), ('Teresa', 2), ('Hopper', 3), ('2', 4), ('Ice', 5), ('Foo', 6), ('Invincible', 7)]:
            old = old.replace('case PlayerMode_' + name + ':', 'case ' + str(value) + ':')
        cpp.write_text(code.replace(switch, old))
        subprocess.run([compiler, '-isysroot', sdk, '-std=c++17', str(cpp), '-o', str(binary)], check=True)
        mutation = subprocess.run([str(binary)], capture_output=True, text=True)
        if mutation.returncode != 1 or 'FAIL' not in mutation.stderr:
            raise AssertionError('old explanation-order switch must fail')
        print('PASS mutation: old explanation-order switch fails mapping regression')


if __name__ == '__main__':
    main()
