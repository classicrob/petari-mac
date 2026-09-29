// Actor lights loaded by LightFunction must not carry stack contents into GX.
// Every Mario, Toad and file-select material lights both channels with spot
// attenuation. GX (and Aurora) evaluate dot(cosAtt, (1, c, c * c)) with
// c = dot(lightDir, dir) for any direction; a direction left as stack garbage
// can overflow c * c, and 0 * inf removes the light. The GX functions here are
// recording test doubles; the light layout is this test's own.
#include "Game/Map/LightDataHolder.hpp"
#include "Game/Map/LightFunction.hpp"
#include "Game/Map/LightPointCtrl.hpp"
#include "Game/Util/CameraUtil.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

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
// Fields of the test's light layout, one u32 each in GXLightObj::dummy.
enum { Color, A0, A1, A2, K0, K1, K2, PosX, PosY, PosZ, DirX, DirY, DirZ, FieldCount };

// 0x7f7f7f7f is 3.4e38: finite, and large enough that c * c overflows.
const u32 cPoison = 0x7f7f7f7f;

struct LoadedLight {
    bool loaded;
    u32 raw[FieldCount];
};
LoadedLight sLoaded[8];

void setF32(GXLightObj* obj, int field, f32 value) {
    std::memcpy(&obj->dummy[field], &value, sizeof(value));
}

f32 getF32(const LoadedLight& light, int field) {
    f32 value;
    std::memcpy(&value, &light.raw[field], sizeof(value));
    return value;
}

// Fills the stack below the caller with the poison, where the next calls' frames go.
__attribute__((noinline)) void poisonStack() {
    volatile u32 area[8192];
    for (u32 i = 0; i < 8192; i++) {
        area[i] = cPoison;
    }
}

// The GX spot/distance attenuation term (the Aurora and Dolphin formula), in f32.
f32 spotAttenuation(const LoadedLight& light, const Vec& lightDir) {
    const f32 cosine = std::fmax(0.0f, lightDir.x * getF32(light, DirX) + lightDir.y * getF32(light, DirY) + lightDir.z * getF32(light, DirZ));
    const f32 cosAttn = getF32(light, A0) + getF32(light, A1) * cosine + getF32(light, A2) * (cosine * cosine);
    const f32 distAttn = getF32(light, K0);
    return std::fmax(0.0f, cosAttn / distAttn);
}

bool allFieldsSet(const LoadedLight& light) {
    for (u32 value : light.raw) {
        if (value == cPoison) {
            return false;
        }
    }
    return true;
}

TPos3f sViewMtx;
}  // namespace

extern "C" {
void GXInitLightAttn(GXLightObj* obj, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2) {
    setF32(obj, A0, a0);
    setF32(obj, A1, a1);
    setF32(obj, A2, a2);
    setF32(obj, K0, k0);
    setF32(obj, K1, k1);
    setF32(obj, K2, k2);
}

void GXInitLightPos(GXLightObj* obj, f32 x, f32 y, f32 z) {
    setF32(obj, PosX, x);
    setF32(obj, PosY, y);
    setF32(obj, PosZ, z);
}

void GXInitLightColor(GXLightObj* obj, GXColor color) {
    std::memcpy(&obj->dummy[Color], &color, sizeof(color));
}

void GXInitLightDistAttn(GXLightObj* obj, f32, f32, GXDistAttnFn) {
    // GX_DA_OFF (the only function the checked lights use) is k = (1, 0, 0).
    setF32(obj, K0, 1.0f);
    setF32(obj, K1, 0.0f);
    setF32(obj, K2, 0.0f);
}

void GXInitLightSpot(GXLightObj* obj, f32, GXSpotFn) {
    // GX_SP_OFF is a = (1, 0, 0).
    setF32(obj, A0, 1.0f);
    setF32(obj, A1, 0.0f);
    setF32(obj, A2, 0.0f);
}

void GXLoadLightObjImm(const GXLightObj* obj, GXLightID id) {
    const int index = __builtin_ctz(static_cast< u32 >(id));
    sLoaded[index].loaded = true;
    std::memcpy(sLoaded[index].raw, obj->dummy, sizeof(sLoaded[index].raw));
}

void GXSetChanAmbColor(GXChannelID, GXColor) {
}
}

const TPos3f& MR::getCameraViewMtx() {
    return sViewMtx;
}

int main() {
    sViewMtx.identity();

    // The file-select player light from LightData.bcsv.
    ActorLightInfo info = {};
    info.mInfo0.mColor = {164, 163, 128, 0};
    info.mInfo0.mPos = {75000.0f, 100000.0f, 76400.0f};
    info.mInfo0.mIsFollowCamera = true;
    info.mInfo1.mColor = {68, 91, 127, 0};
    info.mInfo1.mPos = {-100000.0f, -65917.96875f, 3076.171875f};
    info.mInfo1.mIsFollowCamera = false;
    info.mAlpha2 = 145;
    info.mColor = {63, 63, 63, 129};

    poisonStack();
    LightFunction::loadActorLightInfo(&info);

    // Toward the camera, as for the eye's rim light at the view origin.
    const Vec toCamera = {0.0f, 0.0f, 1.0f};
    for (int i = 0; i < 3; i++) {
        CHECK(sLoaded[i].loaded);
        CHECK(allFieldsSet(sLoaded[i]));
        const f32 attn = spotAttenuation(sLoaded[i], toCamera);
        CHECK(std::isfinite(attn) && attn == 1.0f);
        if (!allFieldsSet(sLoaded[i])) {
            std::fprintf(stderr, "  light %d: direction %g %g %g, attenuation %g\n", i, getF32(sLoaded[i], DirX), getF32(sLoaded[i], DirY),
                         getF32(sLoaded[i], DirZ), attn);
        }
    }
    // Light 2 is the rim light: black with the Alpha2 intensity.
    GXColor rim;
    std::memcpy(&rim, &sLoaded[2].raw[Color], sizeof(rim));
    CHECK(rim.r == 0 && rim.g == 0 && rim.b == 0 && rim.a == 145);

    PointLightInfo point = {};
    point.mPos = {0.0f, 100.0f, 0.0f};
    point.mColor = {255, 200, 100, 255};
    point.mRefDistance = 1000.0f;
    point.mRefBrightness = 0.5f;
    point.mDistAttnFn = GX_DA_OFF;
    poisonStack();
    LightFunction::loadPointLightInfo(&point);
    CHECK(sLoaded[4].loaded && allFieldsSet(sLoaded[4]));
    CHECK(spotAttenuation(sLoaded[4], toCamera) == 1.0f);

    std::memset(sLoaded, 0, sizeof(sLoaded));
    poisonStack();
    LightFunction::loadAllLightWhite();
    bool whiteOk = true;
    for (const LoadedLight& light : sLoaded) {
        // Attenuation is never set here: it must be deterministic, not stack contents.
        whiteOk &= light.loaded && allFieldsSet(light);
    }
    CHECK(whiteOk);

    if (sFailures != 0) {
        std::fprintf(stderr, "%d of %d light check(s) failed\n", sFailures, sChecks);
        return 1;
    }
    std::printf("light function tests passed (%d checks)\n", sChecks);
    return 0;
}
