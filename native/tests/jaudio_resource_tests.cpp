// Native JAudio2 resource parsing on real disc data: the audio archive
// (AudioRes/SMR.szs) through JAUAudioArcInterpreter, wave systems, banks, the
// sound and name tables, the sequence collection, the stream file table, and
// every sound animation (.bas) in the disc's RARC archives.
// Usage: jaudio_resource_tests --assets <files dir>
#include "JSystem/JAudio2/JASBNKParser.hpp"
#include "JSystem/JAudio2/JASBasicBank.hpp"
#include "JSystem/JAudio2/JASBasicInst.hpp"
#include "JSystem/JAudio2/JASSimpleWaveBank.hpp"
#include "JSystem/JAudio2/JASWSParser.hpp"
#include "JSystem/JAudio2/JASWaveInfo.hpp"
#include "JSystem/JAudio2/JAUAudioArcInterpreter.hpp"
#include "JSystem/JAudio2/JAUSeqCollection.hpp"
#include "JSystem/JAudio2/JAUSoundAnimator.hpp"
#include "JSystem/JAudio2/JAUSoundTable.hpp"
#include "JSystem/JAudio2/JAUStreamFileTable.hpp"
#include "JSystem/JKernel/JKRExpHeap.hpp"
#include <archive.hpp>
#include <petari/endian.hpp>
#include <petari/host_allocation.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

extern "C" void OSInit();

static int sFailures;
static int sChecks;

#define CHECK(expr)                                                                                                                                  \
    do {                                                                                                                                             \
        sChecks++;                                                                                                                                   \
        if (!(expr)) {                                                                                                                               \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr);                                                          \
            sFailures++;                                                                                                                             \
        }                                                                                                                                            \
    } while (0)

namespace {
using PetariNative::readU32BE;

std::vector< u8 > readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::vector< u8 > bytes((std::istreambuf_iterator< char >(stream)), std::istreambuf_iterator< char >());
    if (bytes.size() >= 4 && (std::memcmp(bytes.data(), "Yaz0", 4) == 0 || std::memcmp(bytes.data(), "Yay0", 4) == 0)) {
        JKRHeap::HostAllocationScope host;
        bytes = PetariNative::Resource::decompress({bytes.data(), bytes.size()});
    }
    return bytes;
}

struct Collector : public JAUAudioArcInterpreter {
    std::vector< const void* > waveSystems;
    std::vector< const void* > banks;
    const void* soundTable = nullptr;
    const void* nameTable = nullptr;
    const void* seqCollection = nullptr;
    u32 seqCollectionSize = 0;
    const void* streamTable = nullptr;
    int sequences = 0;

    void readWS(u32, void const* data, u32) {
        waveSystems.push_back(data);
    }
    void readBNK(u32, void const* data) {
        banks.push_back(data);
    }
    void readBSC(void const* data, u32 size) {
        seqCollection = data;
        seqCollectionSize = size;
    }
    void readBST(void const* data, u32) {
        soundTable = data;
    }
    void readBSTN(void const* data, u32) {
        nameTable = data;
    }
    void readBMS(u32, void const*, u32) {
        sequences++;
    }
    void readBMS_fromArchive(u32) {
        sequences++;
    }
    void newVoiceBank(u32, u32) {
    }
    void newDynamicSeqBlock(u32) {
    }
    void readBSFT(void const* data) {
        streamTable = data;
    }
    void readMaxSeCategory(int, int, int) {
    }
    void beginBNKList(u32, u32) {
    }
    void endBNKList() {
    }
};

bool checkWaveInfo(const JASWaveInfo& info) {
    bool ok = info.mFormat <= 3 && info.mSampleRate >= 4000.0f && info.mSampleRate <= 48000.0f && info.mSampleCount > 0 && info.mAWLength > 0 &&
              info.mAWStartOffs >= 0;
    if (info.mLoopFlags != 0) {
        ok &= info.mSampleLoopStart >= 0 && info.mSampleLoopStart < info.mSampleLoopEnd && info.mSampleLoopEnd <= info.mSampleCount + 1;
    }
    return ok;
}

// Oscillator tables are host order after parsing: every point's mode is a
// small value and the table ends with a terminator (mode > 10).
bool checkOscTable(const JASOscillator::Point* point) {
    for (int i = 0; i < 64; i++, point++) {
        if (point->_0 > 10) {
            return point->_0 <= 15;
        }
        if (point->_0 < 0) {
            return false;
        }
    }
    return false;
}

void testAudioArchive(const std::filesystem::path& files, JKRHeap* heap, JAUSoundTable* soundTable) {
    std::vector< u8 > archive = readFile(files / "AudioRes/SMR.szs");
    static std::vector< u8 > sArchive;
    sArchive = archive;
    Collector collector;
    CHECK(collector.parse(sArchive.data()));
    std::printf("SMR: %zu wave systems, %zu banks, %d sequences, table %s, names %s, collection %s, streams %s\n", collector.waveSystems.size(),
                collector.banks.size(), collector.sequences, collector.soundTable ? "yes" : "no", collector.nameTable ? "yes" : "no",
                collector.seqCollection ? "yes" : "no", collector.streamTable ? "yes" : "no");
    CHECK(!collector.waveSystems.empty() && !collector.banks.empty());

    int waves = 0;
    bool wavesOk = true;
    for (const void* data : collector.waveSystems) {
        JASWaveBank* bank = JASWSParser::createWaveBank(data, heap);
        bool simple = JASWSParser::getGroupCount(data) == 1;
        wavesOk &= bank != nullptr;
        for (u32 id = 0; bank != nullptr && id < 0x1000; id++) {
            JASWaveHandle* handle = bank->getWaveHandle(id);
            // A simple bank's table covers every id up to the largest one; ids the
            // wave system does not list keep a default entry with no heap (as on Wii).
            if (handle != nullptr && simple && static_cast< JASSimpleWaveBank::TWaveHandle* >(handle)->mHeap == nullptr) {
                continue;
            }
            if (handle != nullptr) {
                const JASWaveInfo& info = *handle->getWaveInfo();
                if (!checkWaveInfo(info)) {
                    std::fprintf(stderr, "  wave %u: format %u rate %g count %d aw %d+%d loop %u %d..%d\n", id, info.mFormat, info.mSampleRate,
                                 info.mSampleCount, info.mAWStartOffs, info.mAWLength, info.mLoopFlags, info.mSampleLoopStart, info.mSampleLoopEnd);
                    wavesOk = false;
                }
                waves++;
            }
        }
    }
    std::printf("waves: %d\n", waves);
    CHECK(wavesOk && waves > 100);

    int insts = 0;
    int oscTables = 0;
    bool banksOk = true;
    for (const void* data : collector.banks) {
        JASBasicBank* bank = JASBNKParser::createBasicBank(data, heap);
        banksOk &= bank != nullptr;
        for (int i = 0; bank != nullptr && i < 0xF0; i++) {
            JASInst* inst = bank->getInst(i);
            if (inst == nullptr) {
                continue;
            }
            insts++;
            if (inst->getType() != 'BSIC') {
                continue;
            }
            const JASBasicInst* basic = static_cast< const JASBasicInst* >(inst);
            banksOk &= std::isfinite(basic->mVolume) && std::isfinite(basic->mPitch) && basic->getKeyRegionCount() <= 128;
            for (int j = 0; j < OSC_MAX; j++) {
                const JASOscillator::Data* osc = basic->mOscillatorData[j];
                if (osc != nullptr && osc->mTable != nullptr) {
                    banksOk &= checkOscTable(osc->mTable);
                    oscTables++;
                }
            }
        }
    }
    std::printf("instruments: %d, oscillator tables: %d\n", insts, oscTables);
    // Native heap usage for the parsed objects (measured by the parsers themselves).
    std::printf("native heap for parsed audio: wave banks %u bytes, banks %u bytes\n", JASWSParser::sUsedHeapSize, JASBNKParser::sUsedHeapSize);
    CHECK(banksOk && insts > 0 && oscTables > 0);

    // Sound table: every item is reachable and has a known type.
    soundTable->init(collector.soundTable);
    CHECK(soundTable->isValid());
    int items = 0;
    bool tableOk = true;
    for (int section = 0; section < 256; section++) {
        JAUSoundTableSection* sectionData = soundTable->mTable.getSection(section);
        int groups = sectionData != nullptr ? static_cast< int >(sectionData->mNumGroups) : 0;
        for (int group = 0; group < groups; group++) {
            JAUSoundTableGroup* groupData = soundTable->mTable.getGroup(sectionData, group);
            int count = groupData != nullptr ? static_cast< int >(groupData->mNumItems) : 0;
            for (int wave = 0; wave < count; wave++) {
                JAISoundID id(section, group, wave);
                u8 type = soundTable->getTypeID(id);
                JAUSoundTableItem* item = soundTable->getData(id);
                tableOk &= item != nullptr && (type & 0xF0) >= 0x40 && (type & 0xF0) <= 0x70;
                items++;
            }
        }
    }
    std::printf("sound table items: %d\n", items);
    CHECK(tableOk && items > 1000);

    // Name table: every sound in the sound table has a printable name.
    static JAUSoundNameTable names(true);
    names.init(collector.nameTable);
    int named = 0;
    bool namesOk = true;
    for (int section = 0; section < 256; section++) {
        JAUSoundTableSection* sectionData = soundTable->mTable.getSection(section);
        int groups = sectionData != nullptr ? static_cast< int >(sectionData->mNumGroups) : 0;
        for (int group = 0; group < groups; group++) {
            JAUSoundTableGroup* groupData = soundTable->mTable.getGroup(sectionData, group);
            int count = groupData != nullptr ? static_cast< int >(groupData->mNumItems) : 0;
            for (int wave = 0; wave < count; wave++) {
                const char* name = names.getName(JAISoundID(section, group, wave));
                bool printable = name != nullptr && name[0] != 0;
                for (const char* c = name; printable && *c; c++) {
                    printable = *c >= 0x20 && *c < 0x7F;
                }
                namesOk &= printable;
                named++;
            }
        }
    }
    std::printf("sound names: %d\n", named);
    CHECK(namesOk && named == items);

    // Sequence collection offsets stay inside the collection.
    JAUSeqCollection collection;
    collection.init(collector.seqCollection);
    CHECK(collection.isValid());
    int seqs = 0;
    bool seqOk = collection.isValid();
    for (int group = 0; seqOk && group < collection.mGroupNum; group++) {
        for (int wave = 0; wave < 0x1000; wave++) {
            JAISeqData seqData(nullptr, 0);
            if (!collection.getSeqData(group, wave, &seqData)) {
                break;
            }
            seqOk &= seqData.offset < collection.mFileSize;
            seqs++;
        }
    }
    std::printf("sequence collection: %d groups, %d sequences\n", seqOk ? collection.mGroupNum : -1, seqs);
    CHECK(seqOk && seqs > 0);

    // Stream file paths name real files on the disc. SMG's audio archive has no
    // stream file table (its streams are resolved by game code), so this only
    // runs for archives that include one.
    if (collector.streamTable != nullptr) {
        JAUStreamFileTable streams;
        streams.init(collector.streamTable);
        CHECK(streams.isValid());
        int present = 0;
        for (u32 i = 0; streams.isValid() && i < streams.getNumFiles(); i++) {
            std::string path = streams.getFilePath(i);
            if (!path.empty() && path[0] == '/') {
                path.erase(0, 1);
            }
            present += std::filesystem::exists(files / path) ? 1 : 0;
        }
        std::printf("stream files: %d of %u present\n", present, streams.getNumFiles());
        CHECK(present == static_cast< int >(streams.getNumFiles()));
    }
}

// Sound animations reference sounds in the sound table.
void testSoundAnimations(const std::filesystem::path& files, JAUSoundTable* soundTable) {
    int animations = 0;
    int sounds = 0;
    int dangling = 0;
    for (const auto& item : std::filesystem::recursive_directory_iterator(files)) {
        if (!item.is_regular_file() || item.path().extension() != ".arc") {
            continue;
        }
        std::vector< u8 > bytes = readFile(item.path());
        if (bytes.size() < 4 || std::memcmp(bytes.data(), "RARC", 4) != 0) {
            continue;
        }
        JKRHeap::HostAllocationScope host;
        PetariNative::Resource::Archive archive = PetariNative::Resource::Archive::parse({bytes.data(), bytes.size()});
        for (size_t i = 0; i < archive.entries().size(); i++) {
            const auto& entry = archive.entries()[i];
            if (entry.isDirectory() || entry.name.size() < 5 || entry.name.compare(entry.name.size() - 4, 4, ".bas") != 0) {
                continue;
            }
            std::vector< u8 > data = archive.resourceData(i);
            const JAUSoundAnimation* animation = reinterpret_cast< const JAUSoundAnimation* >(data.data());
            bool ok = data.size() >= 8 && animation->mControlSlot == 0 &&
                      8 + animation->getNumSounds() * sizeof(JAUSoundAnimationSound) <= data.size();
            for (u16 s = 0; ok && s < animation->getNumSounds(); s++) {
                const JAUSoundAnimationSound* sound = animation->getSound(s);
                ok &= std::isfinite(sound->mNoteOnTime) && std::isfinite(sound->mNoteOffTime) && std::isfinite(sound->mBasePitch);
                // A few records name sounds that are absent from the sound table
                // (the game ignores them); they are counted, not failed.
                if (soundTable->getData(sound->getSoundID()) == nullptr) {
                    dangling++;
                }
                sounds++;
            }
            CHECK(ok);
            if (!ok) {
                std::fprintf(stderr, "  bad sound animation %s in %s (control %u, sounds %u, size %zu)\n", entry.name.c_str(), item.path().c_str(),
                             static_cast< u32 >(animation->mControlSlot), animation->getNumSounds(), data.size());
                for (u16 s = 0; s < animation->getNumSounds() && 8 + (s + 1) * sizeof(JAUSoundAnimationSound) <= data.size(); s++) {
                    const JAUSoundAnimationSound* sound = animation->getSound(s);
                    std::fprintf(stderr, "    sound %08x on %g off %g table %p\n", static_cast< u32 >(sound->getSoundID()),
                                 static_cast< f32 >(sound->mNoteOnTime), static_cast< f32 >(sound->mNoteOffTime),
                                 static_cast< void* >(soundTable->getData(sound->getSoundID())));
                }
            }
            animations++;
        }
    }
    std::printf("sound animations: %d files, %d sounds, %d not in the sound table\n", animations, sounds, dangling);
    CHECK(animations > 0 && sounds > 0 && dangling * 20 < sounds);
}
}  // namespace

int main(int argc, char** argv) {
    const char* assets = nullptr;
    for (int i = 1; i + 1 < argc; i++) {
        if (std::strcmp(argv[i], "--assets") == 0) {
            assets = argv[i + 1];
        }
    }
    if (assets == nullptr) {
        std::puts("jaudio resource tests skipped (no --assets)");
        return 0;
    }

    OSInit();
    PetariNative::setGameAllocationThread(true);
    JKRExpHeap* root = JKRExpHeap::createRoot(1, false);
    root->becomeCurrentHeap();
    root->becomeSystemHeap();
    JKRExpHeap* heap = JKRExpHeap::create(96 * 1024 * 1024, root, false);
    heap->becomeCurrentHeap();

    static JAUSoundTable soundTable(true);
    testAudioArchive(assets, heap, &soundTable);
    testSoundAnimations(assets, &soundTable);

    if (sFailures != 0) {
        std::fprintf(stderr, "%d of %d jaudio check(s) failed\n", sFailures, sChecks);
        return 1;
    }
    std::printf("jaudio resource tests passed (%d checks)\n", sChecks);
    return 0;
}
