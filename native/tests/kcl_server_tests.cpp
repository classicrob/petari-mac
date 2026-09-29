// KCollisionServer tests for the native build: the game's collision server
// (src/Game/Map/KCollision.cpp) queried on real and generated collision.
//
// - Generated collision, laid out as DynamicCollisionObj::createCollision builds it
//   (AreaPolygon: 4 positions, 2 triangles): separately allocated sections, a single octree
//   leaf word 0x80000002 with -1 block shifts, and the explicit triangle count. It goes
//   through the typed KCollisionServer::initWithFile path, then triangle count, farthest
//   vertex distance and arrow/point queries.
// - With --assets FILES: disc .kcl resources converted as ResourceHolder does (KCL::
//   convertInPlace), registered through setData, then an arrow through each sampled
//   triangle's centroid along its normal must hit a triangle. This walks the real octree.
//
// Usage: petari_kcl_server_tests [--assets GAME_FILES_DIR]
// Links: src/Game/Map/KCollision.cpp, src/Game/Util/JMapInfo.cpp, src/Game/Util/MathUtil.cpp,
// native/resource (kcl_collision, archive), petari_platform_os (OSPanic). The collision
// director and camera-code hooks KCollision.cpp references are defined below as aborting
// stubs; they are reached only for triangles with attribute data, which these tests omit.
#include "archive.hpp"
#include "Game/Map/KCollision.hpp"
#include <petari/kcl_collision.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

class CollisionCode;
class CollisionDirector;

namespace MR {
    [[noreturn]] static void unreachable(const char* pName) {
        std::fprintf(stderr, "kcl_server_tests: unexpected call to %s\n", pName);
        std::abort();
    }

    CollisionDirector* getCollisionDirector() {
        unreachable("MR::getCollisionDirector");
    }

    void registerCameraCode(u32) {
        unreachable("MR::registerCameraCode");
    }
}  // namespace MR

class CollisionCode {
public:
    u32 getCameraID(const JMapInfoIter&);
};

u32 CollisionCode::getCameraID(const JMapInfoIter&) {
    MR::unreachable("CollisionCode::getCameraID");
}

using Buffer = std::vector< std::uint8_t >;

static int sFailures = 0;
static void check(bool condition, const char* text) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", text);
        ++sFailures;
    }
}

static TVec3f cross(const TVec3f& a, const TVec3f& b) {
    return TVec3f(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

static TVec3f normalized(const TVec3f& v) {
    const f32 length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return length > 0.0f ? TVec3f(v.x / length, v.y / length, v.z / length) : TVec3f(0.0f, 0.0f, 0.0f);
}

// Generated collision with DynamicCollisionObj's layout: positions, 4 normals per triangle
// (face, then the three edge normals), prism i + 1 per triangle, and the octree as a u16
// array whose first word is the leaf 0x80000002 and whose list counts down to 0.
struct GeneratedCollision {
    std::vector< TVec3f > mPositions;
    std::vector< TVec3f > mNormals;
    std::vector< KC_PrismData > mPrisms;
    std::vector< u32 > mOctreeWords;
    KCLFile mFile = {};

    GeneratedCollision(const std::vector< TVec3f >& rVertices, const std::vector< u16 >& rIndices) {
        const u16 triangleNum = static_cast< u16 >(rIndices.size() / 3);
        mPositions.resize(triangleNum);
        mNormals.resize(triangleNum * 4);
        mPrisms.resize(triangleNum + 1);
        // u16[triangleNum + 3] as in createCollision, in 4-byte storage so the word read is aligned.
        mOctreeWords.resize((triangleNum + 3 + 1) / 2);
        u16* pList = reinterpret_cast< u16* >(mOctreeWords.data());
        *reinterpret_cast< s32* >(pList) = static_cast< s32 >(0x80000002);
        s32 count = triangleNum;
        for (s32 i = 0; i <= triangleNum; i++) {
            pList[i + 2] = static_cast< u16 >(count--);
        }

        // DynamicCollisionObj::updateTriangle.
        for (u16 i = 0; i < triangleNum; i++) {
            const TVec3f v1 = rVertices[rIndices[i * 3]], v2 = rVertices[rIndices[i * 3 + 1]], v3 = rVertices[rIndices[i * 3 + 2]];
            TVec3f a(v3.x - v1.x, v3.y - v1.y, v3.z - v1.z);
            const TVec3f saveForLater = a;
            TVec3f b(v2.x - v1.x, v2.y - v1.y, v2.z - v1.z);
            TVec3f c(v3.x - v2.x, v3.y - v2.y, v3.z - v2.z);
            a = normalized(a);
            b = normalized(b);
            c = normalized(c);
            const TVec3f face = normalized(cross(b, a));
            const TVec3f edge0 = normalized(cross(TVec3f(-a.x, -a.y, -a.z), face));
            const TVec3f edge1 = normalized(cross(b, face));
            const TVec3f edge2 = normalized(cross(c, face));
            mPositions[i] = v1;
            mNormals[i * 4 + 0] = face;
            mNormals[i * 4 + 1] = edge0;
            mNormals[i * 4 + 2] = edge1;
            mNormals[i * 4 + 3] = edge2;

            // vecKillElement(saveForLater, c): remove the component along c.
            const f32 along = saveForLater.x * c.x + saveForLater.y * c.y + saveForLater.z * c.z;
            const TVec3f height(saveForLater.x - c.x * along, saveForLater.y - c.y * along, saveForLater.z - c.z * along);
            KC_PrismData& rPrism = mPrisms[i + 1];
            rPrism.mHeight = std::sqrt(height.x * height.x + height.y * height.y + height.z * height.z);
            rPrism.mPositionIndex = i;
            rPrism.mNormalIndex = static_cast< u16 >(i * 4);
            rPrism.mEdgeIndices[0] = static_cast< u16 >(i * 4 + 1);
            rPrism.mEdgeIndices[1] = static_cast< u16 >(i * 4 + 2);
            rPrism.mEdgeIndices[2] = static_cast< u16 >(i * 4 + 3);
            rPrism.mAttribute = static_cast< u16 >(i + 1);
        }

        // DynamicCollisionObj::updateCollisionHeader.
        TVec3f min = rVertices[0], max = rVertices[0];
        for (const TVec3f& v : rVertices) {
            min.set(std::min(min.x, v.x), std::min(min.y, v.y), std::min(min.z, v.z));
            max.set(std::max(max.x, v.x), std::max(max.y, v.y), std::max(max.z, v.z));
        }
        u32 masks[3] = {static_cast< u32 >(static_cast< s32 >(max.x - min.x)), static_cast< u32 >(static_cast< s32 >(max.y - min.y)),
                        static_cast< u32 >(static_cast< s32 >(max.z - min.z))};
        u32 maxEntropy = 0;
        for (u32& rMask : masks) {
            if (rMask == 0) {
                rMask = 1;
            }
            u32 bitSel = 0x80000000, maskSel = 0xFFFFFFFF;
            for (u32 bit = 0; bit < 32; bit++, bitSel >>= 1, maskSel >>= 1) {
                if (rMask & bitSel) {
                    rMask = ~maskSel;
                    maxEntropy = std::max(maxEntropy, bit);
                    break;
                }
            }
        }

        mFile.mPos = mPositions.data();
        mFile.mNorms = mNormals.data();
        mFile.mPrisms = mPrisms.data();
        mFile.mOctree = mOctreeWords.data();
        mFile.mThickness = 40.0f;
        mFile.mMin = min;
        mFile.mXMask = static_cast< s32 >(masks[0]);
        mFile.mYMask = static_cast< s32 >(masks[1]);
        mFile.mZMask = static_cast< s32 >(masks[2]);
        mFile.mBlockWidthShift = static_cast< s32 >(33 - maxEntropy);
        mFile.mBlockXShift = -1;
        mFile.mBlockXYShift = -1;
        mFile.mTriangleNum = triangleNum;
    }
};

static KC_PrismData* arrowHit(const KCollisionServer& rServer, const TVec3f& rFrom, const TVec3f& rTo) {
    f32 dists[16];
    u8 flags[16];
    KC_PrismData* prisms[16];
    u32 count = 0;
    const TVec3f dir(rTo.x - rFrom.x, rTo.y - rFrom.y, rTo.z - rFrom.z);
    rServer.checkArrow(rFrom, dir, dists, flags, &count, prisms, 16);
    return count != 0 ? prisms[0] : nullptr;
}

static void testGenerated() {
    // AreaPolygon's quad: a 1000 x 1000 floor at y = 0, two triangles, facing up.
    const std::vector< TVec3f > vertices = {TVec3f(0.0f, 0.0f, 0.0f), TVec3f(0.0f, 0.0f, 1000.0f), TVec3f(1000.0f, 0.0f, 1000.0f),
                                            TVec3f(1000.0f, 0.0f, 0.0f)};
    GeneratedCollision generated(vertices, {0, 1, 2, 0, 2, 3});

    check(*reinterpret_cast< const s32* >(generated.mOctreeWords.data()) == static_cast< s32 >(0x80000002),
          "generated octree root is the host-order leaf word");

    KCollisionServer server;
    server.initWithFile(&generated.mFile, nullptr);
    check(server.mFile == &generated.mFile, "initWithFile uses the generated descriptor itself");
    check(server.getTriangleNum() == 2, "generated collision reports its explicit triangle count");

    // No attribute map: calcFarthestVertexDistance skips camera codes and reports it.
    check(!server.calcFarthestVertexDistance() && std::fabs(server.mMaxVertexDistance - std::sqrt(2.0f) * 1000.0f) < 1.0f,
          "farthest vertex distance over exactly the generated triangles");

    KC_PrismData* pHit = arrowHit(server, TVec3f(250.0f, 100.0f, 750.0f), TVec3f(250.0f, -100.0f, 750.0f));
    check(pHit == &generated.mPrisms[1], "downward arrow hits the first generated triangle");
    pHit = arrowHit(server, TVec3f(750.0f, 100.0f, 250.0f), TVec3f(750.0f, -100.0f, 250.0f));
    check(pHit == &generated.mPrisms[2], "downward arrow hits the second generated triangle");
    check(arrowHit(server, TVec3f(1500.0f, 100.0f, 500.0f), TVec3f(1500.0f, -100.0f, 500.0f)) == nullptr, "arrow outside the floor misses");

    // Moving the collision afterwards (syncCollision rewrites the same descriptor) is seen.
    for (TVec3f& rPos : generated.mPositions) {
        rPos.y += 500.0f;
    }
    generated.mFile.mMin.y += 500.0f;
    pHit = arrowHit(server, TVec3f(250.0f, 600.0f, 750.0f), TVec3f(250.0f, 400.0f, 750.0f));
    check(pHit == &generated.mPrisms[1], "server sees later updates to the generated descriptor");

    // The Wii's two u16 halves (0x8000, 2) read back as 0x00028000 on little-endian, a child
    // offset rather than a leaf; the native builder must store the word.
    u16 halves[2] = {0x8000, 2};
    s32 word;
    std::memcpy(&word, halves, sizeof(word));
    check(word != static_cast< s32 >(0x80000002), "the Wii halves are not the leaf word on this host (reason for the native store)");
}

static bool readFile(const std::filesystem::path& path, Buffer* pOut) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    pOut->assign(std::istreambuf_iterator< char >(file), std::istreambuf_iterator< char >());
    return true;
}

// Every sampled triangle of a converted resource must be found by an arrow through its
// centroid along its face normal.
static void checkResource(Buffer& rData, const std::string& rWhere, std::size_t* pFiles, std::size_t* pArrows, std::size_t* pMisses) {
    if (const char* pError = PetariNative::KCL::convertInPlace(rData.data(), static_cast< u32 >(rData.size()))) {
        std::fprintf(stderr, "FAIL: %s: %s\n", rWhere.c_str(), pError);
        ++sFailures;
        return;
    }

    KCollisionServer server;
    server.init(rData.data(), nullptr);
    const s32 triangleNum = server.getTriangleNum();
    const s32 expected = static_cast< s32 >((PetariNative::KCL::octreeOffset(rData.data()) - PetariNative::KCL::prismOffset(rData.data())) /
                                            sizeof(KC_PrismData)) -
                         1;
    if (triangleNum != expected || triangleNum <= 0) {
        std::fprintf(stderr, "FAIL: %s: triangle count %d, expected %d\n", rWhere.c_str(), triangleNum, expected);
        ++sFailures;
        return;
    }

    const s32 step = std::max(1, triangleNum / 200);
    std::size_t misses = 0;
    for (s32 i = 0; i < triangleNum; i += step) {
        KC_PrismData* pPrism = server.getPrismData(i);
        if (pPrism->mHeight <= 0.0f || server.isNearParallelNormal(pPrism)) {
            continue;
        }
        const TVec3f p0 = server.getPos(pPrism, 0), p1 = server.getPos(pPrism, 1), p2 = server.getPos(pPrism, 2);
        const TVec3f centroid((p0.x + p1.x + p2.x) / 3.0f, (p0.y + p1.y + p2.y) / 3.0f, (p0.z + p1.z + p2.z) / 3.0f);
        const TVec3f& rNormal = *server.getFaceNormal(pPrism);
        const TVec3f from(centroid.x + rNormal.x * 20.0f, centroid.y + rNormal.y * 20.0f, centroid.z + rNormal.z * 20.0f);
        const TVec3f to(centroid.x - rNormal.x * 20.0f, centroid.y - rNormal.y * 20.0f, centroid.z - rNormal.z * 20.0f);
        ++*pArrows;
        if (arrowHit(server, from, to) == nullptr) {
            misses++;
        }
    }
    *pMisses += misses;
    ++*pFiles;
}

static void testAssets(const std::filesystem::path& filesRoot) {
    namespace fs = std::filesystem;
    std::size_t files = 0, arrows = 0, misses = 0;
    for (const char* pDir : {"ObjectData", "StageData"}) {
        if (!fs::is_directory(filesRoot / pDir)) {
            continue;
        }
        for (const fs::directory_entry& entry : fs::recursive_directory_iterator(filesRoot / pDir)) {
            if (files >= 150 || entry.path().extension() != ".arc") {
                continue;
            }
            Buffer bytes;
            if (!readFile(entry.path(), &bytes)) {
                continue;
            }
            PetariNative::Resource::Archive archive;
            try {
                archive = PetariNative::Resource::Archive::parse({bytes.data(), bytes.size()});
            } catch (const std::exception&) {
                continue;
            }
            for (std::size_t i = 0; i < archive.entries().size() && files < 150; i++) {
                const auto& rEntry = archive.entries()[i];
                if (rEntry.isDirectory() || rEntry.name.size() < 4 || rEntry.name.substr(rEntry.name.size() - 4) != ".kcl") {
                    continue;
                }
                Buffer data = archive.resourceData(i);
                checkResource(data, entry.path().filename().string() + ":" + rEntry.name, &files, &arrows, &misses);
            }
        }
    }

    std::printf("KCollisionServer: %zu disc KCL files, %zu centroid arrows, %zu misses\n", files, arrows, misses);
    check(files > 0 && arrows > 0, "no disc KCL resources queried");
    // Centroid arrows along the normal of a non-degenerate triangle always cross it; allow a
    // tiny fraction for sliver triangles whose centroid is within float error of an edge.
    check(misses * 1000 <= arrows, "centroid arrows hit their triangles");
}

int main(int argc, char** argv) {
    testGenerated();
    if (argc == 3 && std::strcmp(argv[1], "--assets") == 0) {
        testAssets(argv[2]);
    } else if (argc != 1) {
        std::fprintf(stderr, "Usage: %s [--assets GAME_FILES_DIR]\n", argv[0]);
        return 2;
    }
    if (sFailures != 0) {
        std::fprintf(stderr, "%d collision check(s) failed\n", sFailures);
        return 1;
    }
    std::puts("KCollisionServer tests passed");
    return 0;
}
