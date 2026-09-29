#!/usr/bin/env python3
"""Compile production lookup/operation bodies with CPU-only layout fixtures."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(path, signature):
    source = (ROOT / path).read_text()
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


PREFIX = r'''
#include <petari/asset_diagnostics.hpp>
#include <petari/host_allocation.hpp>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <list>
#include <string>
#include <thread>
#include <vector>
using u8 = unsigned char;
using u32 = unsigned;
using s32 = int;
using s16 = short;
using f32 = float;
using JAISoundID = int;
std::atomic<int> scopeEntries{0};
namespace PetariNative {
HostAllocationScope::HostAllocationScope() { ++scopeEntries; }
HostAllocationScope::~HostAllocationScope() {}
}
namespace nw4r { namespace lyt {
struct AnimTransform {
    float mFrame = 0;
    int GetFrameSize() const { return 30; }
    bool IsLoopData() const { return true; }
};
struct Pane;
struct PaneList {
    using Iterator = std::list<Pane>::iterator;
    std::list<Pane> panes;
    Iterator GetBeginIter() { return panes.begin(); }
    Iterator GetEndIter() { return panes.end(); }
};
struct Pane {
    const char* mName;
    bool text = false;
    int binds = 0;
    PaneList children;
    Pane(const char* name) : mName(name) {}
    Pane* FindPaneByName(const char* name, bool) {
        if (!std::strcmp(mName, name)) return this;
        for (auto& child : children.panes) if (auto* result = child.FindPaneByName(name, true)) return result;
        return nullptr;
    }
    PaneList& GetChildList() { return children; }
    void BindAnimation(AnimTransform*, bool) { ++binds; }
    void UnbindAnimation(AnimTransform*, bool) { --binds; }
};
struct TextBox : Pane {
    int writes = 0;
    TextBox(const char* name) : Pane(name) { text = true; }
};
struct Layout { Pane* mpRootPane; };
}}
namespace nw4r { namespace ut {
struct Rect { float GetWidth() const { return 48; } };
template<class T> T DynamicCast(nw4r::lyt::Pane* pane) {
    return pane && pane->text ? static_cast<T>(pane) : nullptr;
}
}}
struct J3DFrameCtrl {
    float frame = 0, rate = 0;
    int end = 0, attribute = 0;
    void init(s16 size) { end = size; }
    void setAttribute(int a) { attribute = a; }
    void setFrame(float f) { frame = f; }
    void setRate(float r) { rate = r; }
};
namespace MR {
u32 getHashCodeLower(const char* name) {
    u32 result = 0;
    while (*name) result = result * 31 + std::tolower(static_cast<unsigned char>(*name++));
    return result;
}
}
struct LayoutHolder {
    struct { u32 mCount = 1; } mAnimRes;
    bool isAnimationHashEqual(u32 hash, u32) { return hash == MR::getHashCodeLower("appear.brlan"); }
};
struct LayoutPaneCtrl;
struct LayoutPaneInfo { const char* mName; LayoutPaneCtrl* mPaneCtrl; };
struct LayoutManager {
    const char* mLayoutName = "GuardFixture";
    nw4r::lyt::Layout* mLayout = nullptr;
    u32 mPaneCount = 1;
    LayoutPaneInfo* mPaneInfoList = nullptr;
    LayoutHolder* mLayoutHolder = nullptr;
    nw4r::lyt::AnimTransform** mAnimTransList = nullptr;
    bool _61 = true;
    nw4r::lyt::Pane* getPane(const char*) const;
    s32 getIndexOfPane(const char*) const;
    LayoutPaneCtrl* getPaneCtrl(const char*) const;
    bool isExistPaneCtrl(const char*) const;
    nw4r::lyt::AnimTransform* getAnimTransform(const char*) const;
    nw4r::lyt::Pane* findPaneByName(const char* name) const { return mLayout->mpRootPane->FindPaneByName(name,true); }
    void bindPaneCtrlAnim(LayoutPaneCtrl*, nw4r::lyt::AnimTransform*) {}
    void unbindPaneCtrlAnim(LayoutPaneCtrl*, nw4r::lyt::AnimTransform*) {}
};
struct LayoutAnmPlayer {
    LayoutManager* mManager;
    nw4r::lyt::AnimTransform* mAnimTransform = nullptr;
    const char* mAnimName = nullptr;
    J3DFrameCtrl mFrameCtrl;
    void start(const char*);
};
struct LayoutPaneCtrl {
    LayoutManager* mHost;
    nw4r::lyt::Pane* mPane;
    std::vector<LayoutAnmPlayer*> mAnmPlayerArray;
    void start(const char*,u32);
    J3DFrameCtrl* getFrameCtrl(u32) const;
};
struct LayoutActor {
    LayoutManager* manager;
    LayoutManager* getLayoutManager() const { return manager; }
};
struct TextBoxRecursiveOperation {
    virtual void execute(nw4r::lyt::TextBox*) const = 0;
};
struct TextBoxRecursiveSetMessage : TextBoxRecursiveOperation {
    TextBoxRecursiveSetMessage(const wchar_t*) {}
    void execute(nw4r::lyt::TextBox* text) const override { ++text->writes; }
};
int measuredText = 0, frameUpdates = 0;
bool getTextDrawRectRecursive(nw4r::ut::Rect*, const nw4r::lyt::Pane* pane, bool) { assert(pane); ++measuredText; return true; }
namespace MR { void setAnimFrameAndStop(LayoutActor*, float, u32) { ++frameUpdates; } }
struct PowerStarList : LayoutActor { void setTotalPowerStarNumForMessageBoardCapture(); };
namespace MR {
wchar_t* addPictureFontTag(wchar_t* p, wchar_t tag) { *p = tag; return p + 1; }
s32 getPowerStarNum() { return 120; }
}
struct AudSoundNameData { u32 mHash; const char* mName; JAISoundID mID; };
struct AudSoundNameConverter {
    u32* mGroupItemOffsets;
    s32 mNumItems;
    AudSoundNameData* mSoundNameData;
    int getSeSoundCategory(const char* name) const { return std::strncmp(name, "SE_BM_", 6) ? -1 : 0; }
    int getOtherSoundCategory(const char*) const { return -1; }
    JAISoundID getSoundID(const char*,u32) const;
};
'''

MAIN = r'''
int main() {
    nw4r::lyt::TextBox root("RootPane");
    nw4r::lyt::Layout layout{&root};
    LayoutManager manager;
    manager.mLayout = &layout;
    LayoutHolder holder;
    manager.mLayoutHolder = &holder;
    nw4r::lyt::AnimTransform anim;
    nw4r::lyt::AnimTransform* transforms[] = {&anim};
    manager.mAnimTransList = transforms;
    LayoutAnmPlayer player{&manager};
    LayoutPaneCtrl ctrl{&manager, &root, {&player}};
    LayoutPaneInfo panes[] = {{"RootPane", &ctrl}};
    manager.mPaneInfoList = panes;
    LayoutActor actor{&manager};
    assert(petari_layout_missing_reference_count() == 0);
    assert(!manager.isExistPaneCtrl("OptionalMissing"));
    assert(petari_layout_missing_reference_count() == 0);
    assert(manager.getIndexOfPane("Missing") == -1);
    assert(manager.getPaneCtrl("Missing") == nullptr);
    assert(manager.getPaneCtrl(nullptr) == &ctrl);
    assert(manager.getPane("Missing") == nullptr);
    assert(petari_layout_missing_reference_count() == 2);
    assert(petari_layout_missing_reference_unique_count() == 1);
    MR::setTextBoxMessageRecursive(&actor, "Missing", L"bad");
    assert(root.writes == 0);
    MR::setTextBoxMessageRecursive(&actor, "RootPane", L"good");
    assert(root.writes == 1);
    MR::setAnimFrameAndStopAdjustTextWidth(&actor, "Missing", 0);
    assert(measuredText == 0 && frameUpdates == 0);
    MR::setAnimFrameAndStopAdjustTextWidth(&actor, "RootPane", 0);
    assert(measuredText == 1 && frameUpdates == 1);
    MR::startPaneAnim(&actor, "Missing", "Appear", 0);
    assert(player.mAnimTransform == nullptr);
    MR::startPaneAnim(&actor, "RootPane", "MissingAnimation", 0);
    assert(player.mAnimTransform == nullptr);
    assert(root.binds == 0);
    MR::startPaneAnim(&actor, "RootPane", "Appear", 0);
    assert(player.mAnimTransform == &anim);
    assert(player.mFrameCtrl.end == 30 && player.mFrameCtrl.rate == 1);
    player.mFrameCtrl.frame = 7;
    const auto binds = root.binds;
    ctrl.start("MissingAnimation", 0);
    player.start("MissingAnimation");
    assert(player.mAnimTransform == &anim && player.mFrameCtrl.frame == 7);
    assert(root.binds == binds);
    ctrl.start("Appear", 4);
    assert(root.binds == binds);
    PowerStarList stars;
    stars.manager = &manager;
    auto before = petari_layout_missing_reference_count();
    stars.setTotalPowerStarNumForMessageBoardCapture();
    assert(petari_layout_missing_reference_count() == before && root.writes == 1);
    root.mName = "TxtStarTotal";
    stars.setTotalPowerStarNumForMessageBoardCapture();
    assert(root.writes == 2);
    u32 offsets[] = {0};
    AudSoundNameData sounds[] = {{123,"SE_BM_VALID", 42}};
    AudSoundNameConverter sound{offsets, 1, sounds};
    assert(sound.getSoundID("SE_BM_VALID",123) == 42);
    assert(sound.getSoundID(nullptr,123) == -1);
    assert(sound.getSoundID("",123) == -1);
    assert(sound.getSoundID("SE_ZZ_UNKNOWN",123) == -1);
    assert(sound.getSoundID("SE_BM_UNKNOWN",123) == -1);
    assert(sound.getSoundID("SE_BM_UNKNOWN",123) == -1);
    assert(petari_sound_missing_reference_count() == 5);
    assert(petari_sound_missing_reference_unique_count() == 4);
    before = petari_layout_missing_reference_count();
    auto unique = petari_layout_missing_reference_unique_count();
    std::vector<std::thread> workers;
    for (int i=0;i<4;++i) workers.emplace_back([] {
        for (int j=0;j<100;++j) PetariNative::reportMissingLayoutReference("Threaded", "Missing", "pane");
    });
    for (auto& worker : workers) worker.join();
    assert(petari_layout_missing_reference_count() == before + 400);
    assert(petari_layout_missing_reference_unique_count() == unique + 1);
    assert(scopeEntries > 400);
    std::printf("layout asset guards: production bodies passed; 400 concurrent failures counted once; optional capture absent/present passed\n");
}
'''


def main():
    parts = [PREFIX]
    for signature in ('nw4r::lyt::Pane* LayoutManager::getPane(', 's32 LayoutManager::getIndexOfPane(',
                      'LayoutPaneCtrl* LayoutManager::getPaneCtrl(', 'bool LayoutManager::isExistPaneCtrl(',
                      'nw4r::lyt::AnimTransform* LayoutManager::getAnimTransform('):
        parts.append(function('src/Game/Screen/LayoutManager.cpp', signature))
    for path, signature in [('src/Game/Animation/LayoutAnmPlayer.cpp', 'void LayoutAnmPlayer::start('),
                            ('src/Game/Screen/LayoutPaneCtrl.cpp', 'void LayoutPaneCtrl::start('),
                            ('src/Game/Screen/LayoutPaneCtrl.cpp', 'J3DFrameCtrl* LayoutPaneCtrl::getFrameCtrl(')]:
        parts.append(function(path, signature))
    parts.append(function('src/Game/Util/LayoutUtil.cpp', 'bool nativeLayoutPaneReady('))
    parts.append('namespace MR {')
    for signature in ('void executeTextBoxRecursive(', 'void setTextBoxMessageRecursive(', 'void startPaneAnim(', 'void setAnimFrameAndStopAdjustTextWidth('):
        parts.append(function('src/Game/Util/LayoutUtil.cpp', signature))
    parts.append('}')
    parts.append(function('src/Game/Screen/PowerStarList.cpp', 'void PowerStarList::setTotalPowerStarNumForMessageBoardCapture('))
    parts.append(function('src/Game/AudioLib/AudSoundNameConverter.cpp', 'JAISoundID AudSoundNameConverter::getSoundID(const char* pName, u32 hash)'))
    parts.append(MAIN)
    with tempfile.TemporaryDirectory(prefix='petari-layout-guards-') as temp:
        source = Path(temp) / 'guards.cpp'
        executable = Path(temp) / 'guards'
        source.write_text('\n'.join(parts))
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-DPETARI_NATIVE', '-pthread',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g',
                        '-I', str(ROOT / 'native/include'), str(source), str(ROOT / 'native/src/asset_diagnostics.cpp'),
                        '-o', str(executable)], check=True, timeout=60)
        result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=20)
        print(result.stdout, end='')
        print(result.stderr, end='', file=sys.stderr)
        assert result.returncode == 0, 'guard fixture failed'
        assert result.stderr.count('name="Missing"') == 2, 'expected one diagnostic per layout/name, including threaded layout'
        assert result.stderr.count('[sound]') == 4, 'sound diagnostics must be deduplicated'
    print('ASan/UBSan: clean')


if __name__ == '__main__':
    main()
