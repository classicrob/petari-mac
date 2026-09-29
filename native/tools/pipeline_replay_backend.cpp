#include "pipeline_replay.hpp"
#include <petari/host_allocation.hpp>
#include "gx/gx.hpp"
#include "gx/pipeline.hpp"
#include "gx/regs.hpp"
#include "gx/fifo.hpp"
#include "dolphin/gx/__gx.h"
#include "pipeline-replay-init.inc"
#include "gfx/hash.hpp"
#include <dolphin/gx/GXAurora.h>
#include <unordered_set>
#include <span>
#include <stdexcept>
#include <vector>
#include <cstdio>
using namespace aurora;
using namespace aurora::gx;
using Buffer = std::vector<uint8_t>;
sqlite3* database;
static std::unordered_set<uint64_t> checked;
size_t models, pairs, draws, variants;
std::string resource;
unsigned materialIndex, shapeIndex;
void require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
void sql(const char* text) { require(sqlite3_exec(database, text, nullptr, nullptr, nullptr) == SQLITE_OK, sqlite3_errmsg(database)); }

static void emit(GXPrimitive prim, GXVtxFmt format) {
    PetariNative::HostAllocationScope host;
    ++draws;
    for (auto pixel : {GX_PF_RGB8_Z24, GX_PF_RGBA6_Z24}) {
        g_gxState.pixelFmt = pixel;
        PipelineConfig config{};
        populate_pipeline_config(config, prim, format);
        const auto hash = xxh3_hash(config, static_cast<HashType>(gfx::ShaderType::GX));
        if (checked.insert(hash).second) {
            std::fprintf(stderr, "[replay config] resource=%s material=%u shape=%u hash=%016llx\n",
                         resource.c_str(), materialIndex, shapeIndex, static_cast<unsigned long long>(hash));
            for (auto mode : {DstAlphaMode::None, DstAlphaMode::Replace, DstAlphaMode::DualSource}) {
                for (bool normal : {false, true}) {
                    if (mode == DstAlphaMode::DualSource && normal) continue;
                    const auto source = build_shader_source(config.shaderConfig, mode, normal ? 1u : UINT32_MAX);
                    require(!source.empty(), "empty generated shader");
                    ++variants;
                }
            }
            sqlite3_stmt* insert = nullptr;
            require(sqlite3_prepare_v2(database, "INSERT OR IGNORE INTO pipeline_cache VALUES(1,?,?,?,?,0)", -1, &insert, nullptr) == SQLITE_OK, sqlite3_errmsg(database));
            sqlite3_bind_int64(insert, 1, static_cast<sqlite3_int64>(hash));
            sqlite3_bind_int(insert, 2, GXPipelineConfigVersion);
            sqlite3_bind_int(insert, 3, sizeof(config));
            sqlite3_bind_blob(insert, 4, &config, sizeof(config), SQLITE_TRANSIENT);
            const int result = sqlite3_step(insert);
            sqlite3_finalize(insert);
            require(result == SQLITE_DONE, sqlite3_errmsg(database));
        }
        sqlite3_stmt* insert = nullptr;
        require(sqlite3_prepare_v2(database, "INSERT OR IGNORE INTO replay_sources VALUES(?,?,?,?)", -1, &insert, nullptr) == SQLITE_OK, sqlite3_errmsg(database));
        sqlite3_bind_text(insert, 1, resource.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(insert, 2, materialIndex);
        sqlite3_bind_int(insert, 3, shapeIndex);
        sqlite3_bind_int64(insert, 4, static_cast<sqlite3_int64>(hash));
        const int result = sqlite3_step(insert);
        sqlite3_finalize(insert);
        require(result == SQLITE_DONE, sqlite3_errmsg(database));
    }
}

// Decode the exact register payload with Aurora's runtime register handlers.
// Vertex bytes and matrix values do not affect shader specialization; validate
// their spans without fetching indexed geometry or invoking any GPU draw path.
void replay(const void* memory, size_t size, bool allowDraw) {
    PetariNative::HostAllocationScope host;
    auto data = std::span(static_cast<const uint8_t*>(memory), size);
    size_t pos = 0;
    const auto take = [&](size_t count) {
        require(count <= data.size() - pos, "truncated display list");
        auto value = data.subspan(pos, count); pos += count; return value;
    };
    const auto number = [&](size_t bytes) {
        uint64_t value = 0;
        for (auto byte : take(bytes)) value = (value << 8) | byte;
        return value;
    };
    while (pos < size) {
        const auto command = number(1);
        if (command == 0 || command == 0x48) continue;
        if (command == 0x61) {
            const auto value = number(4);
            // Reject side-effect commands rather than submitting GPU work offline.
            const auto reg = value >> 24;
            require(reg != 0x52 && reg != 0x45 && reg != 0x47 && reg != 0x48, "unexpected GPU side effect in material list");
            fifo::handle_bp(value);
        } else if (command == 0x08) {
            const auto address = number(1);
            fifo::handle_cp(address, number(4));
        } else if (command == 0x10) {
            const auto header = number(4);
            fifo::handle_xf(header & 0xffff, take(((header >> 16) + 1) * 4));
        } else if (command >= 0x20 && command <= 0x38 && (command & 7) == 0) {
            take(4); // indexed position/normal/texture matrix values, not pipeline state
        } else if (command == GX_AURORA) {
            const auto sub = number(2);
            if (sub == GX_AURORA_LOAD_TEXOBJ) { take(34); continue; }
            if (sub == GX_AURORA_LOAD_TLUT) { take(23); continue; }
            require(sub >= GX_AURORA_LOAD_ARRAYBASE && sub <= GX_AURORA_LOAD_ARRAYBASE + 15,
                    "unsupported Aurora command in J3D list");
            const auto index = sub - GX_AURORA_LOAD_ARRAYBASE + GX_VA_POS;
            require(index < g_gxState.arrays.size(), "array index out of range");
            auto& array = g_gxState.arrays[index];
            array.data = reinterpret_cast<const void*>(number(8));
            array.size = number(4); array.le = number(1) == 1;
        } else if (command >= 0x80 && command <= 0xbf && (command & 0xf8) != 0x88) {
            require(allowDraw, "draw in material state list");
            const auto count = number(2);
            const auto primitive = static_cast<GXPrimitive>(command & 0xf8);
            const auto format = static_cast<GXVtxFmt>(command & 7);
            PipelineConfig config{};
            populate_pipeline_config(config, primitive, format);
            take(count * config.shaderConfig.vtxStride);
            if (count) emit(primitive, format);
        } else {
            std::fprintf(stderr, "command=%02llx offset=%zu\n", static_cast<unsigned long long>(command), pos - 1);
            throw std::runtime_error("unsupported display list command");
        }
    }
}

size_t config_count() { return checked.size(); }
void reset() {
    PetariNative::HostAllocationScope host;
    g_gxState = {};
    g_gxState.bpRegCache[0xfe] = 0xffffff;
    g_gxState.dstAlpha = UINT32_MAX;
}

void record_begin(void* data, size_t size) {
    PetariNative::HostAllocationScope host;
    reset_shadow();
    fifo::begin_display_list(static_cast<uint8_t*>(data), size);
}
size_t record_end() { if (__gx->dirtyState) __GXSetDirtyState(); return fifo::end_display_list(); }
void emit_quad() { emit(GX_QUADS, GX_VTXFMT0); }
