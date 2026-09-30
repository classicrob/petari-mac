#!/usr/bin/env python3
"""Execute the actual Rabbit/Binder method bodies against deterministic collision doubles.

The vector projection body is read from MathUtil.cpp, not reimplemented here.
A poisoned default vector makes the no-write raycast contract deterministic without
invoking test-side undefined behavior. Also compile a mutant restoring the ignored
raycast result; it must fail. Binder probes count actual iterations and run with
float-cast-overflow/UB sanitizers. No app, renderer, save, or game-state injection.
"""
from pathlib import Path
import re
from functools import lru_cache
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


@lru_cache(maxsize=None)
def native_source(path):
    # Preprocess before counting braces: conditional branches can each open the
    # same block, and counting both would capture unrelated later functions.
    source = re.sub(r'^\s*#include[^\n]*', '', (ROOT / path).read_text(), flags=re.MULTILINE)
    compiler = sys.argv[1] if len(sys.argv) > 1 else 'clang++'
    return subprocess.run([compiler, '-E', '-P', '-x', 'c++', '-DPETARI_NATIVE=1', '-'],
                          input=source, text=True, capture_output=True, check=True).stdout


def function(path, anchor):
    s = native_source(path)
    begin = s.index(anchor)
    opening = s.index('{', begin)
    level = 1
    end = opening + 1
    while level:
        if s[end] == '{': level += 1
        if s[end] == '}': level -= 1
        end += 1
    return s[begin:end]


PREAMBLE = r'''
#include <petari/collision_limits.hpp>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <dlfcn.h>
#define PETARI_NATIVE 1
#define __REGISTER
using f32 = float;
using s32 = int;
using u32 = unsigned int;
static float poison = 295.22f;
struct Vec {
    float x, y, z;
    Vec() : x(0), y(poison), z(0) {}
    Vec(float a,float b,float c) : x(a),y(b),z(c) {}
    operator const Vec*() const { return this; }
    void zero() { x=y=z=0; }
    void set(const Vec& v) { *this=v; }
    void add(const Vec& v) { x+=v.x; y+=v.y; z+=v.z; }
    void sub(const Vec& v) { x-=v.x; y-=v.y; z-=v.z; }
    Vec operator*(float f) const { return {x*f,y*f,z*f}; }
    Vec operator-(const Vec& v) const { return {x-v.x,y-v.y,z-v.z}; }
    void operator/=(float f) { x/=f; y/=f; z/=f; }
    float dot(const Vec& v) const { return x*v.x+y*v.y+z*v.z; }
    float length() const { return std::sqrt(dot(*this)); }
};
using TVec3f=Vec;
namespace PetariNative { struct HostAllocationScope {}; }
struct Nerve { virtual void f() {} };
struct Spine { const Nerve* getCurrentNerve() const { return nullptr; } };
struct DemoRabbit {
    Vec mVelocity{2,-3,4}, mPosition{100,200,300}, mGravity{0,-1,0};
    int mAirTimer=0;
    Spine* mSpine=nullptr;
    int getNerveStep() const { return 0; }
    void updateStopVelocity();
};
static constexpr float sGroundFric=.9f,sAirFric=.99f,sGroundGravityAccel=.1f;
static constexpr int sIsAirTime=5;
static bool hit=false;
static Vec faceNormal{0,1,0};
struct HitSensor {};
struct Triangle { Vec normal{0,1,0}; const Vec* getFaceNormal() const { return &normal; } };
static bool getFirstPolyOnLineCategoryExceptSensor(Vec*,Triangle* tri,const Vec&,const Vec&,const HitSensor*,int) {
    if (!hit) return false;
    tri->normal=faceNormal;
    return true;
}
namespace MR {
void attenuateVelocity(DemoRabbit* a,float f) { a->mVelocity=a->mVelocity*f; }
void addVelocityToGravityOrGround(DemoRabbit* a,float f) { a->mVelocity.add(a->mGravity*f); }
void reboundVelocityFromCollision(DemoRabbit*) {}
bool getFirstPolyNormalOnLineToMap(Vec*,const Vec&,const Vec&,Vec*,const HitSensor*);
bool isNearZero(const Vec& v) { return v.dot(v)<.000001f; }
f32 vecKillElement(const TVec3f&,const TVec3f&,TVec3f*);
}
struct HitInfo {};
struct Binder {
    struct { bool _1=true,_3=false; } _1EC;
    void* mCollisionPartsFilter=nullptr; void* mTriangleFilter=nullptr;
    float mRadius=50;
    u32 storeCurrentHitInfo(HitInfo*,u32,bool) { return 1; }
    u32 findBindedPos(Vec*,Vec*,bool*,HitInfo*,u32,bool,bool);
};
struct LiveActor { Binder* mBinder; const char* mName; };
struct AllLiveActorGroup { int getObjNum() const { return 0; } const LiveActor* getActor(int) const { return nullptr; } };
namespace MR { const AllLiveActorGroup* getAllLiveActorGroup() { return nullptr; } }
static int probes=0,hitAt=-1;
namespace Collision {
int checkStrikeBallToMapWithMovingReaction(const Vec&,float,void*,void*) { return ++probes==hitAt ? 1:0; }
int checkStrikeBallToMap(const Vec& v,float r,void* a,void* b) { return checkStrikeBallToMapWithMovingReaction(v,r,a,b); }
}
'''
TESTS = r'''
static int checks=0;
static void check(bool ok,const char* why) {
    ++checks;
    if(!ok) { std::fprintf(stderr,"FAIL %s\n",why); std::exit(1); }
}
static bool near(float a,float b) { return std::fabs(a-b)<1e-4f; }
int main() {
    Vec unchanged{7,8,9};
    check(!MR::getFirstPolyNormalOnLineToMap(&unchanged,{0,0,0},{0,-10,0},nullptr,nullptr)&&unchanged.y==8,"real MapUtil miss does not write normal");
    for(Vec direction : {Vec{1,0,0},Vec{0,1,0}}) {
        Vec output{7,8,9};
        check(!MR::checkHitSemilinePlane(&output,{0,10,0},direction,{0,0,0},{0,1,0})&&output.y==8,"parallel/away plane ray returns false without writing");
    }
    Vec point;
    check(MR::checkHitSemilinePlane(&point,{0,10,0},{0,-1,0},{0,0,0},{0,1,0})&&point.y==0,"plane hit still computes intersection");
    // No-hit calls must not depend on any possible stale normal, including NaN.
    for(float value : {295.22f, -295.22f, 0.f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        poison=value; hit=false; DemoRabbit a;
        a.updateStopVelocity();
        check(near(a.mVelocity.x,1.8f)&&near(a.mVelocity.y,-2.8f)&&near(a.mVelocity.z,3.6f),"ray miss retains friction/gravity motion");
        for(int n=0;n<100;n++) a.updateStopVelocity();
        check(a.mVelocity.length()<2,"miss cannot amplify velocity");
    }
    hit=true;
    for(Vec n : {Vec{0,1,0},Vec{1,0,0},Vec{.6f,.8f,0}}) {
        faceNormal=n; DemoRabbit a;
        const Vec before=a.mVelocity*.9f+a.mGravity*.1f;
        const Vec expected=n*before.dot(n);
        a.updateStopVelocity();
        check(near(a.mVelocity.x,expected.x)&&near(a.mVelocity.y,expected.y)&&near(a.mVelocity.z,expected.z),"hit retains original surface projection");
    }
    // The captured growth factor corresponds to air friction times stale n.y squared.
    poison=295.22f; Vec stale; Vec velocity{0,-210830528.f,0}, tangent;
    velocity=velocity*.99f; MR::vecKillElement(velocity,stale,&tangent); velocity.sub(tangent);
    const double gain=velocity.y/-210830528.0;
    check(gain>86280&&gain<86286,"garbage normal reproduces captured ~86283x growth");
    std::printf("poisoned normal growth factor %.6f\n",gain);
    Binder b; HitInfo planes[1];
    for(float distance : {0.f,34.9f,35.f,350.f,35000.f}) {
        Vec position{1,2,3},movement{distance,0,0}; bool remaining=true; probes=0;hitAt=-1;
        check(b.findBindedPos(&position,&movement,&remaining,planes,1,false,false)==0,"normal sweep no hit");
        check(probes==int((1.f/35.f)*distance)+2,"original subdivision/probe count preserved");
        check(!remaining&&std::fabs(position.x-(1+distance))<1,"normal sweep reaches endpoint");
    }
    for(float distance : {35840.f,210830528.f,1.81911306e13f,1.56958872e18f,std::numeric_limits<float>::max(),std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
        for(bool skip : {false,true}) {
            Vec position{1,2,3},movement{0,-distance,0}; bool remaining=true; probes=0;
            check(b.findBindedPos(&position,&movement,&remaining,planes,1,skip,false)==0,"unsafe sweep rejected");
            check(probes==0&&!remaining&&movement.length()==0&&position.y==2,"reject does no collision work or displacement");
        }
    }
    Vec position{0,0,0},movement{70,0,0}; bool remaining=false;probes=0;hitAt=2;
    check(b.findBindedPos(&position,&movement,&remaining,planes,1,false,false)==1&&remaining&&probes==2,"first-hit behavior preserved");
    position.x=std::numeric_limits<float>::quiet_NaN();movement={1,2,3};remaining=true;probes=0;
    check(b.findBindedPos(&position,&movement,&remaining,planes,1,false,false)==0&&!remaining&&probes==0,"invalid position rejected");
    std::printf("PASS %d collision safety checks\n",checks);
}
'''
# Addition is kept separate so the mock vector's default poison is explicit above.
PREAMBLE += '\nVec operator+(const Vec& a,const Vec& b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }\n'


def main():
    compiler=sys.argv[1] if len(sys.argv)>1 else 'clang++'
    sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
    rabbit=function('src/Game/NPC/DemoRabbit.cpp','void DemoRabbit::updateStopVelocity()')
    binder=function('src/Game/LiveActor/Binder.cpp','u32 Binder::findBindedPos(')
    projection=function('src/Game/Util/MathUtil.cpp','f32 PSVECKillElement(')
    wrapper=function('src/Game/Util/MathUtil.cpp','f32 vecKillElement(')
    query=function('src/Game/Util/MapUtil.cpp','bool getFirstPolyNormalOnLineToMap(')
    plane=function('src/Game/Util/MathUtil.cpp','bool checkHitSemilinePlane(')
    source=PREAMBLE+'\n'+projection+'\nnamespace MR {\n'+wrapper+'\n'+query+'\n'+plane+'\n}\n'+rabbit+'\n'+binder+'\n'+TESTS
    with tempfile.TemporaryDirectory(prefix='petari-collision-') as tmp:
        tmp=Path(tmp)
        def execute(text,name):
            cpp=tmp/(name+'.cpp');binary=tmp/name;cpp.write_text('#include <initializer_list>\n'+text)
            subprocess.run([compiler,'-isysroot',sdk,'-std=c++17','-O2','-fsanitize=undefined,float-cast-overflow','-fno-sanitize-recover=all','-I'+str(ROOT/'native/include'),str(cpp),'-o',str(binary)],check=True)
            return subprocess.run([str(binary)],capture_output=True,text=True,timeout=15)
        result=execute(source,'fixed')
        print(result.stdout,end='');print(result.stderr,end='',file=sys.stderr)
        if result.returncode: return result.returncode
        # Restore exactly the old unchecked query, retaining the real projection below it.
        start=rabbit.index('if (!MR::getFirstPolyNormalOnLineToMap')
        end=rabbit.index('{',start)+1
        depth=1
        while depth:
            if rabbit[end]=='{': depth+=1
            if rabbit[end]=='}': depth-=1
            end+=1
        mutant=rabbit[:start]+'MR::getFirstPolyNormalOnLineToMap(&normal, mPosition, mGravity * 10.0f, nullptr, nullptr);'+rabbit[end:]
        result=execute(source.replace(rabbit,mutant),'unchecked')
        if result.returncode==0 or 'ray miss retains' not in result.stderr:
            raise AssertionError('unchecked-raycast mutant must fail the no-hit regression: '+result.stderr)
        print('PASS mutation: original unchecked raycast fails no-hit regression')
    return 0

if __name__=='__main__': raise SystemExit(main())
