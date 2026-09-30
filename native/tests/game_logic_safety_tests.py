#!/usr/bin/env python3
"""Exercise actual native pairing and power-up animation dispatch under sanitizers."""
from pathlib import Path
import subprocess
import sys
import tempfile
from collision_safety_tests import function, native_source
from player_mode_explanation_tests import block

ROOT = Path(__file__).resolve().parents[2]


def main():
    header = (ROOT / 'include/Game/Player/MarioActor.hpp').read_text()
    enum = block(header, header.index('enum PlayerMode {')) + ';'
    morph = native_source('src/Game/Player/MarioActorMorph.cpp')
    table = morph[morph.index('struct myStruct'):morph.index('void MarioActor::touchSensor')]
    change = function('src/Game/Player/MarioActorMorph.cpp', 'const char* MarioActor::changeMorphString(')
    dsp = function('src/JSystem/JAudio2/dsptask.cpp', 'int DSPSendCommands2(')
    warp = function('src/Game/MapObj/WarpPod.cpp', 'void WarpPod::initPair()')
    choose = warp[warp.index('const bool someBool'):warp.index('if (mPairPod->mArg7')]
    code = r'''
#include <cstdio>
#include <cstring>
#include <cstdlib>
using u32=unsigned; using s32=int; using u16=unsigned short; using BOOL=bool;
constexpr bool TRUE=true, FALSE=false;
int sent=0; bool busy=false;
int Dsp_Running_Check() { return 1; }
bool OSDisableInterrupts() { return true; }
void OSRestoreInterrupts(bool) {}
void OSReport(const char*,...) {}
int DSPCheckMailToDSP() { return busy; }
void DSPSendMailToDSP(u32) { ++sent; }
void DSPAssertInt() {}
int DspStartWork(u32,void(*)(u16)) { return 7; }
void callback(u16) {}
bool gIsLuigi=false;
struct MarioActor { unsigned short mPlayerMode; const char* changeMorphString(const char*) const; };
namespace MR { u32 getHashCode(const char* s) { u32 h=0; while(*s) h=h*31+static_cast<unsigned char>(*s++); return h; } }
struct Vec { float x,y,z; };
struct Pod { Vec mPosition; };
int checks=0;
void check(bool pass,const char* message) { ++checks; if(!pass) { std::fprintf(stderr,"FAIL %s\n",message); std::exit(1); } }
'''+enum+'\n'+table+'\n'+dsp+'\n'+change+'\nbool select(const Vec& mPosition,const Pod* mPairPod) {\n'+choose+'return someBool; }\n'
    code += r'''
int main() {
    u32 command=123;
    check(DSPSendCommands2(&command,1,nullptr)==0&&sent==2,"no callback returns no work entry");
    check(DSPSendCommands2(&command,1,callback)==7,"callback returns allocated work entry");
    busy=true;check(DSPSendCommands2(&command,1,nullptr)==-1,"busy mailbox preserves error result");busy=false;
    cMorphStringTable[0]._24=MR::getHashCode("DieBlackHole");
    for(unsigned short mode=0;mode<=10;++mode) for(bool luigi : {false,true}) {
        gIsLuigi=luigi;
        MarioActor actor{mode};
        const char* expected=mode==PlayerMode_Bee ? "DieBlackHoleBee" : luigi ? "DieBlackHoleLuigi" : "DieBlackHole";
        check(std::strcmp(actor.changeMorphString("DieBlackHole"),expected)==0,"mode animation preserves Bee/Luigi and safely falls back for Tornado");
        check(std::strcmp(actor.changeMorphString("Unknown"),"Unknown")==0,"unknown animation unchanged");
    }
    for(int x=-1;x<=1;++x) for(int y=-1;y<=1;++y) for(int z=-1;z<=1;++z) {
        Pod a{{0,0,0}},b{{float(x),float(y),float(z)}};
        const bool expected=x>0 || (x==0 && (y<0 || (y==0 && z<0)));
        check(select(a.mPosition,&b)==expected,"pair uses X ascending then Y/Z descending");
        check(x==0&&y==0&&z==0 ? !select(b.mPosition,&a) : select(a.mPosition,&b)!=select(b.mPosition,&a),"non-coincident pairs select exactly one drawing owner");
    }
    std::printf("PASS %d animation/pairing checks\n",checks);
}
'''
    compiler=sys.argv[1] if len(sys.argv)>1 else 'clang++'
    sdk=subprocess.check_output(['xcrun','--sdk','macosx','--show-sdk-path'],text=True).strip()
    with tempfile.TemporaryDirectory(prefix='petari-game-logic-') as tmp:
        cpp=Path(tmp)/'test.cpp';binary=Path(tmp)/'test'
        def run(source):
            cpp.write_text('#include <initializer_list>\n'+source)
            subprocess.run([compiler,'-isysroot',sdk,'-std=c++17','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(cpp),'-o',str(binary)],check=True)
            return subprocess.run([str(binary)],text=True,capture_output=True,timeout=15)
        result=run(code)
        print(result.stdout,end='');print(result.stderr,end='',file=sys.stderr)
        if result.returncode: return result.returncode
        mutant=code.replace('mPlayerMode < PlayerMode_8 ? item->_0[mPlayerMode] : nullptr','item->_0[mPlayerMode]')
        if mutant==code: raise AssertionError('mutation did not match actual guarded lookup')
        result=run(mutant)
        if result.returncode==0: raise AssertionError('unguarded mode table must fail')
        print('PASS mutation: original unguarded mode lookup fails regression')
    return 0


if __name__=='__main__': raise SystemExit(main())
