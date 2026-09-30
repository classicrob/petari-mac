// Audio boot smoke test: the game's own audio start-up on the extracted disc,
// before the full game reaches it.
//
// Runs the game code GameSystem runs for audio, in its order:
// HeapMemoryWatcher::createRootHeap and its heaps (the native 3 MiB audio solid
// heap), FileRipper::setup, JKRAram::create, the FileLoader thread,
// AudSystemWrapper::requestResourceForInitialize, and
// AudSystemWrapper::createAudioSystem on an OS thread of priority 14 (the
// game runs it through FunctionAsyncExecutor). The main thread then plays the
// role of GameSystem's frame loop: AudSystemWrapper::movement once per VI
// retrace, until the system-init waves are loaded, then loadStaticWaveData,
// then one system SE through AudSystem::startSound.
//
// Output: the real JASAudioThread, JAudio2 DSP host code, native DSP device and
// AI run unmodified; this test is the audio device. It pulls
// Audio::pull() output in fixed amounts per frame (32000/60 frames; less only
// when the DMA engine waits for a late block), so the consumption is nearly
// deterministic, and writes it to a WAV file under $TMPDIR.
//
// Usage: audio_boot_tests --disc <extracted disc root> [--seconds N]

#include "Game/AudioLib/AudSoundNameConverter.hpp"
#include "Game/AudioLib/AudSystem.hpp"
#include "Game/System/AudSystemWrapper.hpp"
#include "Game/System/FileLoader.hpp"
#include "Game/System/FileRipper.hpp"
#include "Game/System/GameSystem.hpp"
#include "Game/System/GameSystemObjHolder.hpp"
#include "Game/System/Language.hpp"
#include "Game/System/HeapMemoryWatcher.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include "Game/Util/SingletonHolder.hpp"
#include <JSystem/JAudio2/JAISoundHandles.hpp>
#include <JSystem/JAudio2/JASAiCtrl.hpp>
#include <JSystem/JAudio2/JASWaveArcLoader.hpp>
#include <JSystem/JAudio2/JASWaveInfo.hpp>
#include <JSystem/JAudio2/JAUSectionHeap.hpp>
#include "Game/AudioLib/AudSceneMgr.hpp"
#include "Game/AudioLib/AudBgm.hpp"
#include "Game/AudioLib/AudWrap.hpp"
#include "Game/RhythmLib/AudChordInfo.hpp"
#include <JSystem/JAudio2/JAIStream.hpp>
#include <JSystem/JAudio2/JASAramStream.hpp>
#include <JSystem/JKernel/JKRAram.hpp>
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <JSystem/JKernel/JKRSolidHeap.hpp>
#include <revolution/os.h>
#include <revolution/vi.h>

#include <petari/host_allocation.hpp>
#include <petari/host_image_heap.hpp>
#include <petari/platform/audio.hpp>
#include <petari/platform/crash.hpp>
#include <petari/platform/dvd.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

extern "C" void OSInit();

// Language-state scaffold storage (see main). SingletonHolder<GameSystem>'s
// instance pointer is private; this explicit specialization of the member
// points it at zeroed storage for the whole program.
alignas(16) static unsigned char gGameSystemStorage[sizeof(GameSystem)] = {};
alignas(16) static unsigned char gObjHolderStorage[sizeof(GameSystemObjHolder)] = {};
template <>
GameSystem* SingletonHolder< GameSystem >::sInstance = reinterpret_cast<GameSystem*>(gGameSystemStorage);

// HeapMemoryWatcher registers the model-loader resolver during audio heap setup.
// This audio-only executable has no J3D loader; retain its registration as scaffold
// state without pulling in the renderer. No audio allocation uses this resolver.
static PetariNative::J3D::HostImageHeapResolver gModelHeapResolver = nullptr;
void PetariNative::J3D::setHostImageHeapResolver(HostImageHeapResolver resolver) {
    gModelHeapResolver = resolver;
}

namespace PAudio = PetariNative::Platform::Audio;
namespace PDVD = PetariNative::Platform::DVD;

namespace {

u32 heapSize(JKRHeap* heap) {
    return static_cast<u32>(static_cast<u8*>(heap->getEndAddr()) - static_cast<u8*>(heap->getStartAddr()));
}

int sChecks = 0;
int sFailures = 0;
void check(bool condition, const char* label) {
    ++sChecks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        ++sFailures;
    }
}

AudSystemWrapper* gWrapper = nullptr;
volatile bool gCreated = false;

void* createAudioSystemThread(void*) {
    gWrapper->createAudioSystem();
    gCreated = true;
    return nullptr;
}

std::vector<std::int16_t> gCapture;
double gFrameRemainder = 0;

// One game frame: the audio device consumes 1/60 s of output, the game runs
// AudSystemWrapper::movement, and the frame ends at the next retrace.
void frame() {
    const double exact = PAudio::outputRate() / 60.0 + gFrameRemainder;
    const std::size_t frames = static_cast<std::size_t>(exact);
    gFrameRemainder = exact - frames;
    const std::size_t at = gCapture.size();
    {
        // The capture is the test's own host memory, not the game heap.
        PetariNative::HostAllocationScope hostAllocations;
        gCapture.resize(at + frames * 2);
    }
    // A short pull: the DMA engine is waiting for the game's next block (the
    // audio thread has not run since the last block start); keep what came.
    const std::size_t got = PAudio::pull(gCapture.data() + at, frames);
    if (got < frames) {
        PetariNative::HostAllocationScope hostAllocations;
        gCapture.resize(at + got * 2);
    }
    // GameSystemObjHolder::update, then updateAudioSystem.
    gWrapper->updateRhythm();
    gWrapper->movement();
    VIWaitForRetrace();
}

template <class Done>
int runFramesUntil(Done done, int maxFrames) {
    for (int i = 0; i < maxFrames; ++i) {
        if (done()) {
            return i;
        }
        frame();
    }
    return done() ? maxFrames : -1;
}

double rms(std::size_t fromFrame, std::size_t toFrame, int channel) {
    double sum = 0;
    std::size_t n = 0;
    for (std::size_t i = fromFrame; i < toFrame && i * 2 + 1 < gCapture.size(); ++i) {
        const double v = gCapture[i * 2 + channel];
        sum += v * v;
        ++n;
    }
    return n ? std::sqrt(sum / n) : 0;
}

// Why a wave bank is not loaded: per arc, the disc entry its file name
// resolved to (setFileName), the load status (0 idle, 1 loading, 2 loaded),
// and the size.
void dumpWaveBank(u32 bank) {
    if (gWrapper->mAudSystem == nullptr || gWrapper->mAudSystem->mSceneMgr == nullptr) {
        std::fprintf(stderr, "no scene manager\n");
        return;
    }
    JAUSectionHeap* heap = gWrapper->mAudSystem->mSceneMgr->mSectionHeap;
    JASWaveBank* waveBank = heap->getWaveBankTable().getWaveBank(bank);
    if (waveBank == nullptr) {
        std::fprintf(stderr, "wave bank %u: not registered\n", bank);
        return;
    }
    std::fprintf(stderr, "wave bank %u: %u arcs\n", bank, waveBank->getArcCount());
    for (u32 i = 0; i < waveBank->getArcCount(); ++i) {
        JASWaveArc* arc = waveBank->getWaveArc(i);
        if (arc == nullptr) {
            continue;
        }
        std::fprintf(stderr, "wave bank %u arc %u: entry %d status %d size %u\n", bank, i, arc->mEntryNum,
                     static_cast<int>(arc->mStatus), arc->mFileLength);
    }
}

void writeWav(const std::filesystem::path& path, std::uint32_t rate) {
    std::ofstream out(path, std::ios::binary);
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(gCapture.size() * 2);
    auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4);
    u32(36 + dataBytes);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(2);
    u32(rate);
    u32(rate * 4);
    u16(4);
    u16(16);
    out.write("data", 4);
    u32(dataBytes);
    out.write(reinterpret_cast<const char*>(gCapture.data()), dataBytes);  // little-endian host, L,R
}

}  // namespace

int main(int argc, char** argv) {
    const char* disc = nullptr;
    int seconds = 3;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--disc") == 0) {
            disc = argv[i + 1];
        } else if (std::strcmp(argv[i], "--seconds") == 0) {
            seconds = std::max(1, std::atoi(argv[i + 1]));
        }
    }
    if (disc == nullptr || !std::filesystem::exists(std::filesystem::path(disc) / "files")) {
        std::puts("audio boot tests skipped (no --disc with files/)");
        return 0;
    }

    const std::filesystem::path tmp = std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp";
    std::filesystem::create_directories(tmp / "petari_audio_boot_crash");
    PetariNative::Platform::Crash::install(tmp / "petari_audio_boot_crash");

    std::string error;
    if (!PDVD::mount({disc}, &error)) {
        std::fprintf(stderr, "cannot mount %s: %s\n", disc, error.c_str());
        return 1;
    }

    // petari_game_main's order (GameSystem.cpp), audio-relevant steps only.
    OSInit();
    PetariNative::setGameAllocationThread(true);
    DVDInit();
    VIInit();
    // SCAFFOLD, not game code: SMG's language-aware loads (MR::loadAsync...,
    // JASWaveArc::setFileName in OverwriteJAudio.cpp) read
    // SingletonHolder<GameSystem>::get()->mObjHolder->mLanguage. The real
    // GameSystem and GameSystemObjHolder bring the scene graph with them, so
    // this test provides zeroed storage for both carrying only mLanguage,
    // set to what GameSystemObjHolder's constructor stores. Nothing else in
    // them is read on the audio path; a stray read would fault on a null.
    GameSystem* gameSystem = SingletonHolder< GameSystem >::get();
    gameSystem->mObjHolder = reinterpret_cast<GameSystemObjHolder*>(gObjHolderStorage);
    gameSystem->mObjHolder->mLanguage = MR::getDecidedLanguageFromIPL();

    HeapMemoryWatcher::createRootHeap();
    SingletonHolder< HeapMemoryWatcher >::init();
    HeapMemoryWatcher* heaps = SingletonHolder< HeapMemoryWatcher >::get();
    check(gModelHeapResolver != nullptr, "heap setup registers the model-heap resolver");
    heaps->setCurrentHeapToStationedHeap();
    FileRipper::setup(0x20000, MR::getStationedHeapNapa());
    // GameSystem::init and GameSystemObjHolder.
    JKRAram::create(0xE00000, 0xFFFFFFFF, 8, 7, 3);
    SingletonHolder< FileLoader >::init();
    JKRSolidHeap* audioHeap = heaps->getAudSystemHeap();
    check(audioHeap != nullptr, "HeapMemoryWatcher created the audio solid heap");
    if (audioHeap == nullptr) {
        return 1;
    }
    std::printf("audio solid heap: %u bytes usable (created with 0x300000)\n", static_cast<unsigned>(heapSize(audioHeap)));
    check(heapSize(audioHeap) > 0x2F0000, "the native 3 MiB audio solid heap");
    gWrapper = new AudSystemWrapper(audioHeap, MR::getStationedHeapNapa());
    gWrapper->requestResourceForInitialize();

    // GameSystem::exeInitializeAudio: createAudioSystem asynchronously at
    // priority 14 while the frame loop runs.
    static OSThread thread;
    static u8 stack[0x10000] __attribute__((aligned(32)));
    OSCreateThread(&thread, createAudioSystemThread, nullptr, stack + sizeof(stack), sizeof(stack), 14, 0);
    OSResumeThread(&thread);
    const u32 subFramesBefore = JASDriver::getSubFrameCounter();
    const int initFrames = runFramesUntil([] { return gCreated && gWrapper->isLoadDoneWaveDataAtSystemInit(); }, 60 * 20);
    check(initFrames >= 0, "createAudioSystem finishes and the system-init waves load (GameSystemInitializeAudio)");
    if (initFrames < 0) {
        std::fprintf(stderr, "created=%d\n", gCreated ? 1 : 0);
        dumpWaveBank(7);
        return 1;
    }
    OSJoinThread(&thread, nullptr);
    std::printf("audio system initialised after %d frames; audio heap free %u of %u bytes\n", initFrames,
                static_cast<unsigned>(audioHeap->getFreeSize()), static_cast<unsigned>(heapSize(audioHeap)));
    // JASKernel::setupRootHeap takes what AudNewAudSystem leaves of the solid
    // heap, so 0 free here is normal; exhaustion would have panicked
    // (HeapMemoryWatcher's error handler) during createAudioSystem.

    gWrapper->loadStaticWaveData();
    const int staticFrames = runFramesUntil([] { return gWrapper->isLoadDoneStaticWaveData(); }, 60 * 30);
    check(staticFrames >= 0, "static wave data loads");
    if (staticFrames < 0) {
        for (u32 bank : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 8u, 11u}) {
            dumpWaveBank(bank);
        }
    }
    std::printf("static wave data loaded after %d more frames\n", staticFrames);

    const u32 subFrames = JASDriver::getSubFrameCounter() - subFramesBefore;
    check(subFrames > 0, "JASAudioThread runs DSP subframes from AI DMA interrupts");
    check(PAudio::outputRate() == 32000, "JAudio2 selects 32 kHz output");

    // One real system SE, as MR::startSystemSE resolves it.
    const JAISoundID id = AudSingletonHolder< AudSoundNameConverter >::get()->getSoundID("SE_SY_COIN");
    const std::size_t seStart = gCapture.size() / 2;
    const double silenceL = rms(seStart > 32000 ? seStart - 32000 : 0, seStart, 0);
    const double silenceR = rms(seStart > 32000 ? seStart - 32000 : 0, seStart, 1);
    JAISoundHandle handle;
    const bool started = gWrapper->mAudSystem->startSound(id, &handle, nullptr);
    check(started, "AudSystem::startSound(SE_SY_COIN)");
    runFramesUntil([] { return false; }, 60 * seconds);
    const std::size_t seEnd = gCapture.size() / 2;
    const double seL = rms(seStart, seEnd, 0);
    const double seR = rms(seStart, seEnd, 1);
    std::printf("RMS before SE L=%.1f R=%.1f; with SE L=%.1f R=%.1f; %u DSP subframes\n", silenceL, silenceR, seL, seR,
                JASDriver::getSubFrameCounter() - subFramesBefore);
    check(seL > 50.0 || seR > 50.0, "the SE is audible in the captured output");
    check(seL > silenceL * 4 || seR > silenceR * 4, "output rises when the SE plays");

    // The title BGM stream, as TitleSequenceProduct::exeBgmPrepare and
    // MR::startStageBGM("STM_TITLE", true) / MR::unlockStageBGM run it:
    // AudBgmMgr -> JAIStreamMgr -> JASAramStream (header, first blocks, and
    // ARAM ring through JASDvd/the stream load thread), then playback.
    const JAISoundID bgmId = AudSingletonHolder< AudSoundNameConverter >::get()->getSoundID("STM_TITLE");
    JAISoundHandle* bgmHandle = AudWrap::startStageBgm(bgmId, true);
    check(bgmHandle != nullptr, "AudWrap::startStageBgm(STM_TITLE, locked)");
    AudBgm* stageBgm = AudWrap::getStageBgm();
    const int prepareFrames = runFramesUntil([&] { return stageBgm->isPreparedPlay(); }, 60 * 10);
    check(prepareFrames >= 0, "the title BGM stream prepares (header and first blocks in ARAM)");
    std::printf("STM_TITLE prepared after %d frames\n", prepareFrames);
    JAISound* bgmSound = stageBgm->getHandle() != nullptr ? stageBgm->getHandle()->getSound() : nullptr;
    JAIStream* stream = bgmSound != nullptr ? bgmSound->asStream() : nullptr;
    check(stream != nullptr, "the stage BGM is a JAIStream");
    if (stream != nullptr) {
        const JASAramStream& aram = stream->inner_.aramStream;
        std::printf("STM_TITLE stream: %u channels, format %u, loop %d (%u..%u), block size %u, %u samples/block\n",
                    aram.mChannelNum, aram._158, aram.mLoop ? 1 : 0, aram.mLoopStart, aram.mLoopEnd,
                    JASAramStream::getBlockSize(), aram.getBlockSamples());
        check(aram.mChannelNum >= 1 && aram.mChannelNum <= 6, "stream header channel count is sane (1..6)");
        check(aram._158 <= 1, "stream format is ADPCM or PCM16");
        check(aram.mLoopEnd == 0 || aram.mLoopStart < aram.mLoopEnd, "loop range is ordered");
    }
    const std::size_t bgmStart = gCapture.size() / 2;
    stageBgm->playAfterPrepared();  // MR::unlockStageBGM
    runFramesUntil([] { return false; }, 60 * seconds);
    const std::size_t bgmEnd = gCapture.size() / 2;
    const double bgmL = rms(bgmStart, bgmEnd, 0);
    const double bgmR = rms(bgmStart, bgmEnd, 1);
    std::printf("STM_TITLE RMS L=%.1f R=%.1f over %.1f s\n", bgmL, bgmR, (bgmEnd - bgmStart) / 32000.0);
    check(bgmL > 100.0 && bgmR > 100.0, "the title BGM plays on both channels");

    // Title -> file select, as the game runs it: A+B in TitleSequenceProduct
    // (MR::stopStageBGM(75), SE_SY_GAME_START), then FileSelector::exeTitleEnd
    // (MR::startStageBGM("MBGM_FILE_SELECT", false)): a sequence BGM whose
    // rhythm/chord data (AudChordTable::setChordTableResource) loads on start.
    stageBgm->stop(75);
    JAISoundHandle startSe;
    check(gWrapper->mAudSystem->startSound(AudSingletonHolder< AudSoundNameConverter >::get()->getSoundID("SE_SY_GAME_START"),
                                           &startSe, nullptr),
          "SE_SY_GAME_START");
    runFramesUntil([] { return false; }, 90);  // the fade-out and the decide sequence
    const JAISoundID fileSelectId = AudSingletonHolder< AudSoundNameConverter >::get()->getSoundID("MBGM_FILE_SELECT");
    std::printf("MBGM_FILE_SELECT sound id %08x\n", static_cast<unsigned>(static_cast<u32>(fileSelectId)));
    JAISoundHandle* fileSelectHandle = AudWrap::startStageBgm(fileSelectId, false);
    check(fileSelectHandle != nullptr, "AudWrap::startStageBgm(MBGM_FILE_SELECT, unlocked)");
    const std::size_t fsStart = gCapture.size() / 2;
    runFramesUntil([] { return false; }, 60 * seconds);
    const std::size_t fsEnd = gCapture.size() / 2;
    const double fsL = rms(fsStart + 16000, fsEnd, 0);  // skip the first half second of fade-in
    const double fsR = rms(fsStart + 16000, fsEnd, 1);
    std::printf("MBGM_FILE_SELECT RMS L=%.1f R=%.1f over %.1f s\n", fsL, fsR, (fsEnd - fsStart) / 32000.0);
    check(fsL > 100.0 || fsR > 100.0, "the file-select sequence BGM plays");
    AudChordInfo* chords = gWrapper->mAudSystem->getChordInfo();
    check(chords != nullptr && chords->isAvailable(), "the file-select BGM's chord table loads (AudChordTable::setChordTableResource)");
    if (chords != nullptr) {
        std::printf("chord info: table %d, %d chords, %d scales\n", chords->mTableId, static_cast<int>(chords->mTable.mChordCount),
                    static_cast<int>(chords->mTable.mScaleCount));
        check(chords->mTable.mChordCount > 0 && chords->mTable.mChordCount < 256 && chords->mTable.mScaleCount > 0 &&
                  chords->mTable.mScaleCount < 256,
              "chord table counts are sane");
    }

    // Exercise the same fades and pause state transitions as the game's pause menu.
    // Measure after each fade so a valid fade-out is not mistaken for a gap.
    for (int cycle = 0; cycle < 3; ++cycle) {
        gWrapper->mAudSystem->enterPauseMenu();
        runFramesUntil([] { return false; }, 60);
        check(gWrapper->mAudSystem->mIsPaused, "pause menu pauses the BGM after its fade");
        const std::size_t pausedStart = gCapture.size() / 2;
        runFramesUntil([] { return false; }, 60);
        const std::size_t pausedEnd = gCapture.size() / 2;
        const double paused = std::max(rms(pausedStart, pausedEnd, 0), rms(pausedStart, pausedEnd, 1));
        check(paused < 2.0, "settled pause output is silent");
        gWrapper->mAudSystem->exitPauseMenu();
        runFramesUntil([] { return false; }, 60);
        check(!gWrapper->mAudSystem->mIsPaused && !gWrapper->mAudSystem->isPauseMenuActive(),
              "leaving pause restores active playback");
        const std::size_t resumedStart = gCapture.size() / 2;
        runFramesUntil([] { return false; }, 120);
        const std::size_t resumedEnd = gCapture.size() / 2;
        const double resumed = std::max(rms(resumedStart, resumedEnd, 0), rms(resumedStart, resumedEnd, 1));
        check(resumed > 100.0, "BGM remains audible after repeated pause/resume");
        std::printf("pause cycle %d: settled RMS %.2f, resumed RMS %.1f\n", cycle + 1, paused, resumed);
    }

    // Isolate the star pickup sound and the actual Power/Grand Star fanfares;
    // background BGM must not satisfy their output checks accidentally.
    AudWrap::getStageBgm()->stop(2);
    runFramesUntil([] { return false; }, 120);
    JAISoundHandle starSe;
    const auto starId = AudSingletonHolder< AudSoundNameConverter >::get()->getSoundID("SE_SY_STAR_GET");
    check(gWrapper->mAudSystem->startSound(starId, &starSe, nullptr), "SE_SY_STAR_GET starts");
    const std::size_t starStart = gCapture.size() / 2;
    runFramesUntil([] { return false; }, 180);
    const std::size_t starEnd = gCapture.size() / 2;
    check(std::max(rms(starStart, starEnd, 0), rms(starStart, starEnd, 1)) > 50.0,
          "isolated star pickup sound is audible");
    if (starSe.getSound() != nullptr) starSe->stop(0);
    for (const char* name : {"BGM_CLEAR", "BGM_GRAND_STAR_GET", "BGM_GRAND_STAR_GET_2"}) {
        runFramesUntil([] { return false; }, 120);
        const auto id = AudSingletonHolder< AudSoundNameConverter >::get()->getSoundID(name);
        check(AudWrap::startSubBgm(id, false) != nullptr, "star fanfare starts");
        const std::size_t start = gCapture.size() / 2;
        runFramesUntil([] { return false; }, 240);
        const std::size_t end = gCapture.size() / 2;
        const double level = std::max(rms(start, end, 0), rms(start, end, 1));
        check(level > 100.0, "isolated star fanfare is audible");
        std::printf("%s: RMS %.1f, capture frames %zu..%zu\n", name, level, start, end);
        AudWrap::getSubBgm()->stop(0);
    }

    const std::filesystem::path wav = tmp / "petari_audio_boot.wav";
    writeWav(wav, PAudio::outputRate());
    std::printf("captured %zu frames to %s\n", gCapture.size() / 2, wav.c_str());

    if (sFailures != 0) {
        std::fprintf(stderr, "%d of %d audio boot check(s) failed\n", sFailures, sChecks);
        return 1;
    }
    std::printf("audio boot tests passed (%d checks)\n", sChecks);
    std::fflush(stdout);
    std::_Exit(0);  // the game never tears audio down; threads keep running
}
