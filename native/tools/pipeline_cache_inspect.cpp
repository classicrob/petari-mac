// CPU-only inspection of serialized GX configurations; does not initialize a GPU.
#include "gx/gx.hpp"
#include "gx/pipeline.hpp"
#include "gx/shader_info.hpp"
#include "gfx/resources.hpp"
#include "gfx/hash.hpp"
#include <sqlite3.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
#include <unordered_set>
#include <string_view>

struct Row {
    unsigned long long key;
    unsigned tev, indirect;
    size_t bytes, lines;
    double sourceMs;
};

int main(int argc, char** argv) {
    aurora::gfx::detail::resources().limits.minUniformBufferOffsetAlignment = 256;
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: petari_pipeline_cache_inspect CACHE.db [WGSL_OUTPUT_DIRECTORY]\n");
        return 2;
    }
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(argv[1], &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        std::fprintf(stderr, "Cannot open pipeline cache: %s\n", db ? sqlite3_errmsg(db) : "allocation failed");
        if (db) sqlite3_close(db);
        return 2;
    }
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT config FROM pipeline_cache WHERE type=1 AND config_version=?", -1,
                          &statement, nullptr) != SQLITE_OK) {
        std::fprintf(stderr, "%s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return 2;
    }
    sqlite3_bind_int(statement, 1, aurora::gx::GXPipelineConfigVersion);
    if (argc == 3) std::filesystem::create_directories(argv[2]);
    std::vector<Row> rows;
    size_t variants = 0;
    std::unordered_set<std::string> uniqueSources;
    std::unordered_set<std::string_view> uniqueDefaultSources;
    unsigned invalid = 0;
    int result;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
        aurora::gx::PipelineConfig config{};
        if (sqlite3_column_bytes(statement, 0) != sizeof(config)) {
            ++invalid;
            continue;
        }
        std::memcpy(&config, sqlite3_column_blob(statement, 0), sizeof(config));
        const auto key = aurora::xxh3_hash(config, static_cast<aurora::HashType>(aurora::gfx::ShaderType::GX));
        const auto start = std::chrono::steady_clock::now();
        const auto info = aurora::gx::build_shader_info(config.shaderConfig);
        if (!info.uniformSize || info.uniformSize > aurora::gx::MaxUniformSize) ++invalid;
        const auto source = aurora::gx::build_shader_source(config.shaderConfig, aurora::gx::DstAlphaMode::None);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        const auto sourceIt = uniqueSources.insert(source).first;
        uniqueDefaultSources.insert(*sourceIt);
        ++variants;
        for (auto mode : {aurora::gx::DstAlphaMode::None, aurora::gx::DstAlphaMode::Replace, aurora::gx::DstAlphaMode::DualSource}) {
            for (const auto normal : {UINT32_MAX, 1u}) {
                if ((mode == aurora::gx::DstAlphaMode::None && normal == UINT32_MAX) ||
                    (mode == aurora::gx::DstAlphaMode::DualSource && normal != UINT32_MAX)) continue;
                const auto variantStart = std::chrono::steady_clock::now();
                const auto variant = aurora::gx::build_shader_source(config.shaderConfig, mode, normal);
                ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - variantStart).count();
                uniqueSources.insert(variant);
                if (variant.empty()) ++invalid;
                ++variants;
            }
        }
        rows.push_back({key, config.shaderConfig.tevStageCount, config.shaderConfig.numIndStages,
                        source.size(), static_cast<size_t>(std::count(source.begin(), source.end(), '\n')), ms});
        if (argc == 3) {
            char name[40];
            std::snprintf(name, sizeof(name), "%016llx.wgsl", static_cast<unsigned long long>(key));
            std::ofstream out(std::filesystem::path(argv[2]) / name, std::ios::binary);
            out << source;
            if (!out) { std::fprintf(stderr, "Cannot write %s\n", name); ++invalid; }
        }
    }
    sqlite3_finalize(statement);
    sqlite3_close(db);
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.bytes > b.bytes; });
    std::puts("config,wgsl_bytes,wgsl_lines,tev_stages,indirect_stages,source_ms");
    for (const auto& row : rows)
        std::printf("%016llx,%zu,%zu,%u,%u,%.3f\n", row.key, row.bytes, row.lines, row.tev, row.indirect, row.sourceMs);
    std::fprintf(stderr, "Inspected %zu GX configs; invalid=%u; shader_variants=%zu (five legal output variants). "
                         "Source-generation durations are not Metal compile timings.\n", rows.size(), invalid, variants);
    std::fprintf(stderr, "Exact WGSL source reuse: unique_default_sources=%zu unique_all_variant_sources=%zu\n",
                 uniqueDefaultSources.size(), uniqueSources.size());
    return invalid || result != SQLITE_DONE ? 1 : 0;
}
