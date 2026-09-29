// Where the J3D loaders put host-layout copies (petari/host_image_heap.hpp): the current heap
// by default, the resolver's heap when it names one, and no copy for sources that are already
// host images. The source resource is never modified.
//
// Usage: petari_host_image_heap_tests --assets GAME_FILES_DIR
// Links: petari_j3d, native/tests/heap_diagnostics.cpp, native/gx/legacy_commands.cpp.
#include "archive.hpp"
#include <JSystem/J3DGraphAnimator/J3DAnimation.hpp>
#include <JSystem/J3DGraphAnimator/J3DModelData.hpp>
#include <JSystem/J3DGraphLoader/J3DAnmLoader.hpp>
#include <JSystem/J3DGraphLoader/J3DModelLoader.hpp>
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <JSystem/JKernel/JKRSolidHeap.hpp>
#include <petari/host_allocation.hpp>
#include <petari/host_image_heap.hpp>
#include <petari/j3d_animation.hpp>
#include <petari/j3d_model.hpp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
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
using Buffer = std::vector< std::uint8_t >;

const std::uint32_t cHeapSize = 8 * 1024 * 1024;

// The resolver under test: answers sTarget for sources in [sBegin, sEnd), null otherwise,
// and records its calls.
JKRHeap* sTarget;
std::uintptr_t sBegin;
std::uintptr_t sEnd;
int sCalls;
const void* sLastSource;

JKRHeap* resolver(const void* pSource) {
    sCalls++;
    sLastSource = pSource;
    const std::uintptr_t address = reinterpret_cast< std::uintptr_t >(pSource);
    return address >= sBegin && address < sEnd ? sTarget : nullptr;
}

// Bytes in use relative to an empty heap (only differences are compared).
u32 used(JKRHeap* pHeap) {
    return cHeapSize - static_cast< u32 >(pHeap->getTotalFreeSize());
}

// A current heap and a sibling companion, solid like the file cache: used sizes are exact.
struct Heaps {
    JKRSolidHeap* current;
    JKRSolidHeap* companion;
    JKRHeap* previous;

    Heaps() {
        current = JKRSolidHeap::create(cHeapSize, JKRHeap::sRootHeap, false);
        companion = JKRSolidHeap::create(cHeapSize, JKRHeap::sRootHeap, false);
        previous = current->becomeCurrentHeap();
    }

    ~Heaps() {
        previous->becomeCurrentHeap();
        JKRHeap::destroy(companion);
        JKRHeap::destroy(current);
    }
};

enum class Kind { Model, Animation };

bool load(Kind kind, const void* pData) {
    if (kind == Kind::Model) {
        return J3DModelLoaderDataBase::loadBinaryDisplayList(pData, J3DMLF_Material_UseIndirect | J3DMLF_UseUniqueMaterials) != nullptr;
    }
    return J3DAnmLoaderDataBase::load(pData) != nullptr;
}

// Heap use of one load under a resolver setting: current and companion bytes, resolver calls.
struct Use {
    u32 current;
    u32 companion;
    int calls;
    bool loaded;
};

enum class Mode { None, Companion, Null };

Use measureImage(Kind kind, const u8* pData, Mode mode);

Use measure(Kind kind, const Buffer& source, Mode mode) {
    sBegin = reinterpret_cast< std::uintptr_t >(source.data());
    sEnd = sBegin + source.size();
    return measureImage(kind, source.data(), mode);
}

// sBegin/sEnd: the range the resolver routes to the companion (set by the caller).
Use measureImage(Kind kind, const u8* pData, Mode mode) {
    Heaps heaps;
    sTarget = mode == Mode::Companion ? heaps.companion : nullptr;
    sCalls = 0;
    sLastSource = nullptr;
    PetariNative::J3D::setHostImageHeapResolver(mode == Mode::None ? nullptr : resolver);
    const u32 current = used(heaps.current);
    const u32 companion = used(heaps.companion);
    Use use;
    use.loaded = load(kind, pData);
    use.current = used(heaps.current) - current;
    use.companion = used(heaps.companion) - companion;
    use.calls = sCalls;
    PetariNative::J3D::setHostImageHeapResolver(nullptr);
    return use;
}

// A new solid heap's data starts 16-aligned: its first 32-aligned allocation may pad once.
const u32 cFirstAlignmentPad = 31;

void testRouting(const char* what, Kind kind, const Buffer& source, u32 fileSize) {
    std::unique_ptr< Buffer > original;
    {
        PetariNative::HostAllocationScope host;
        original.reset(new Buffer(source));
    }
    const Use none = measure(kind, source, Mode::None);
    const Use routed = measure(kind, source, Mode::Companion);
    const Use declined = measure(kind, source, Mode::Null);
    std::printf("%s (%u bytes): no resolver current +%u; companion resolver current +%u companion +%u; null resolver current +%u\n", what, fileSize,
                none.current, routed.current, routed.companion, declined.current);

    CHECK(none.loaded && routed.loaded && declined.loaded);
    // Default: the copy and the objects on the current heap.
    CHECK(none.companion == 0 && none.current >= fileSize);
    // Resolver: the copy (plus at most the heap's first alignment pad) moves to the
    // companion, and exactly the copy's bytes leave the current heap; called once.
    CHECK(routed.companion >= fileSize && routed.companion - fileSize <= cFirstAlignmentPad);
    CHECK(none.current > routed.current && none.current - routed.current >= fileSize &&
          none.current - routed.current - fileSize <= cFirstAlignmentPad);
    CHECK(routed.calls == 1 && sLastSource == source.data());
    // A resolver answering null keeps today's current-heap behavior.
    CHECK(declined.current == none.current && declined.companion == 0 && declined.calls == 1);
    // The source resource is not modified.
    CHECK(source == *original);
    PetariNative::HostAllocationScope host;
    original.reset();
}

// A source that is already a host image is used in place: no copy, no resolver call.
// The image lives in a JKR heap, as game images do (J3D maps texture data to arena addresses).
void testHostPassthrough(const char* what, Kind kind, const Buffer& source) {
    const u32 size = static_cast< u32 >(source.size());
    JKRSolidHeap* imageHeap = JKRSolidHeap::create(size + 0x100, JKRHeap::sRootHeap, false);
    u8* image = new (imageHeap, 0x20) u8[size];
    const char* pError = kind == Kind::Model ? PetariNative::J3D::makeHostModelImage(source.data(), size, image)
                                             : PetariNative::J3D::makeHostAnimImage(source.data(), size, image);
    CHECK(pError == nullptr);
    const Use converted = measure(kind, source, Mode::Companion);
    const Use host = measureImage(kind, image, Mode::Companion);
    std::printf("%s host image: current +%u companion +%u, resolver calls %d\n", what, host.current, host.companion, host.calls);
    CHECK(host.loaded);
    CHECK(host.calls == 0 && host.companion == 0);
    CHECK(host.current == converted.current);
    JKRHeap::destroy(imageHeap);
}

using Files = std::vector< std::pair< std::string, Buffer > >;

// Allocated under the host policy; the caller frees it under the host policy too.
Files* readArchive(const std::filesystem::path& path) {
    PetariNative::HostAllocationScope host;
    std::ifstream stream(path, std::ios::binary);
    Buffer bytes{std::istreambuf_iterator< char >(stream), {}};
    const auto archive = PetariNative::Resource::Archive::parse({bytes.data(), bytes.size()});
    Files* files = new Files;
    for (std::size_t i = 0; i < archive.entries().size(); i++) {
        if (!archive.entries()[i].isDirectory()) {
            files->emplace_back(archive.entries()[i].name, archive.resourceData(i));
        }
    }
    return files;
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
        std::puts("host image heap tests skipped (no --assets)");
        return 0;
    }
    OSInit();
    JKRExpHeap::createRoot(1, false);

    std::unique_ptr< Files > files(readArchive(std::filesystem::path(assets) / "ObjectData" / "FileSelectDataMario.arc"));
    const Buffer* bdl = nullptr;
    const Buffer* btp = nullptr;
    for (const auto& [name, data] : *files) {
        if (name == "fileselectdatamario.bdl") {
            bdl = &data;
        } else if (name == "wait.btp") {
            btp = &data;
        }
    }
    CHECK(bdl != nullptr && btp != nullptr);
    if (bdl != nullptr && btp != nullptr) {
        testRouting("model", Kind::Model, *bdl, PetariNative::J3D::modelFileSize(bdl->data()));
        testRouting("animation", Kind::Animation, *btp, PetariNative::J3D::animFileSize(btp->data()));
        testHostPassthrough("model", Kind::Model, *bdl);
        testHostPassthrough("animation", Kind::Animation, *btp);
    }
    CHECK(PetariNative::J3D::resolveHostImageHeap(bdl != nullptr ? bdl->data() : nullptr) == nullptr);
    CHECK(JKRHeap::sRootHeap->check());
    {
        PetariNative::HostAllocationScope host;
        files.reset();
    }

    if (sFailures != 0) {
        std::fprintf(stderr, "%d of %d host image heap check(s) failed\n", sFailures, sChecks);
        return 1;
    }
    std::printf("host image heap tests passed (%d checks)\n", sChecks);
    return 0;
}
