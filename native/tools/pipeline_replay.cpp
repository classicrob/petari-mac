// Offline J3D display-list replay using the native loader and Aurora config builder.
#include "archive.hpp"
#include <petari/boot.hpp>
#include <petari/host_allocation.hpp>
#include <JSystem/J3DGraphAnimator/J3DModelData.hpp>
#include <JSystem/J3DGraphBase/J3DMaterial.hpp>
#include <JSystem/J3DGraphBase/J3DShape.hpp>
#include <JSystem/J3DGraphBase/J3DShapeDraw.hpp>
#include <JSystem/J3DGraphBase/J3DPacket.hpp>
#include <JSystem/J3DGraphLoader/J3DModelLoader.hpp>
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <JSystem/JUtility/JUTNameTab.hpp>
#include <filesystem>
#include <sqlite3.h>
#include <fstream>
#include <unordered_set>
#include <stdexcept>
#include <span>
#include <string>
#include <vector>
#include <cstdio>

#include "pipeline_replay.hpp"
#include "pipeline-replay-model.inc"
using Buffer = std::vector<uint8_t>;
void replay_layout(std::vector<uint8_t>, const PetariNative::Resource::Archive&);

static PetariNative::Resource::Archive read_archive(const std::filesystem::path& path) {
    PetariNative::HostAllocationScope host;
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "cannot open archive");
    Buffer bytes{std::istreambuf_iterator<char>(input), {}};
    return PetariNative::Resource::Archive::parse({bytes.data(), bytes.size()});
}

static void model(const Buffer& bytes, const Buffer* replacement = nullptr) {
    auto* heap = JKRExpHeap::create(48 * 1024 * 1024, JKRHeap::sRootHeap, false);
    require(heap != nullptr, "cannot allocate model heap");
    auto* previous = heap->becomeCurrentHeap();
    const bool binary = std::memcmp(bytes.data() + 4, "bdl", 3) == 0;
    auto* data = binary ? J3DModelLoaderDataBase::loadBinaryDisplayList(bytes.data(), J3DMLF_Material_UseIndirect | J3DMLF_UseUniqueMaterials)
                        : J3DModelLoaderDataBase::load(bytes.data(), J3DMLF_Material_PE_Full | J3DMLF_Material_UseIndirect | J3DMLF_UseUniqueMaterials | J3DMLF_21);
    require(data != nullptr, "J3D loader returned null");
    if (binary) {
        ReplayModel::initEnvelopeAndEnvMapOrProjMapModelData(data);
        if (ReplayModel::isUseFur(data) || resource == "marioshadow.bdl") ReplayModel::downFracVtx(data);
    }
    J3DMaterialTable* replacementTable = replacement ? J3DModelLoaderDataBase::loadMaterialTable(replacement->data()) : nullptr;
    require(!replacement || replacementTable, "cannot load replacement material table");
    j3dSys.setTexture(replacementTable ? replacementTable->getTexture() : data->getTexture());
    Buffer defaults;
    {
        PetariNative::HostAllocationScope host;
        defaults.resize(65536);
        record_begin(defaults.data(), defaults.size());
        j3dSys.reinitGX();
        defaults.resize(record_end());
    }
    for (shapeIndex = 0; shapeIndex < data->getShapeNum(); ++shapeIndex) {
        j3dSys.offFlag(0x40000000);
        auto* shape = data->getShapeNodePointer(shapeIndex);
        auto* material = shape->getMaterial();
        require(material != nullptr, "shape has no material");
        for (materialIndex = 0; materialIndex < data->getMaterialNum(); ++materialIndex)
            if (data->getMaterialNodePointer(materialIndex) == material) break;
        require(materialIndex < data->getMaterialNum(), "material not in model table");
        if (replacementTable) {
            const auto index = replacementTable->getMaterialName()->getIndex(data->getMaterialName()->getName(materialIndex));
            if (index < 0) continue;
            materialIndex = index;
            material = replacementTable->getMaterialNodePointer(index);
        }
        if (!binary || replacementTable) {
            material->newSharedDisplayList(material->countDLSize());
            material->makeSharedDisplayList();
        }
        auto* list = material->getSharedDisplayListObj();
        require(list != nullptr, "missing material display list");
        reset();
        replay(defaults.data(), defaults.size(), false);
        replay(list->getDisplayList(0), list->getDisplayListSize(), false);
        for (bool postTransform : {false, true}) {
            if (postTransform) j3dSys.onFlag(0x40000000);
            else j3dSys.offFlag(0x40000000);
            material->calcCurrentMtx();
            {
                PetariNative::HostAllocationScope host;
                Buffer commands(4096);
                record_begin(commands.data(), commands.size());
                material->mCurrentMtx.load();
                auto length = record_end();
                replay(commands.data(), length, false);
            }
            shape->makeVcdVatCmd();
            replay(shape->getVcdVatCmd(), J3DShape::kVcdVatDLSize, false);
            for (unsigned group = 0; group < shape->getMtxGroupNum(); ++group) {
                auto* draw = shape->getShapeDraw(group);
                require(draw != nullptr, "missing shape draw group");
                replay(draw->getDisplayList(), draw->getDisplayListSize(), true);
            }
        }
        ++pairs;
    }
    j3dSys.offFlag(0x40000000);
    j3dSys.setTexture(nullptr);
    previous->becomeCurrentHeap();
    JKRHeap::destroy(heap);
    ++models;
}

int main(int argc, char** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: petari_pipeline_replay ARCHIVE.arc OUTPUT.db\n"); return 2; }
    try {
        auto archive = read_archive(argv[1]);
        require(sqlite3_open(argv[2], &database) == SQLITE_OK, "cannot open output");
        sql("CREATE TABLE IF NOT EXISTS aurora_schema(value INTEGER); DELETE FROM aurora_schema; INSERT INTO aurora_schema VALUES(1);"
            "CREATE TABLE IF NOT EXISTS pipeline_cache(type INTEGER,hash INTEGER,config_version INTEGER,config_size INTEGER,config BLOB,first_frame_used INTEGER,PRIMARY KEY(type,hash));"
            "CREATE TABLE IF NOT EXISTS replay_sources(resource TEXT,material INTEGER,shape INTEGER,hash INTEGER,PRIMARY KEY(resource,material,shape,hash)); BEGIN;");
        OSInit();
        JKRExpHeap::createRoot(1, false);
        // MarioActorDraw shares MarioAnime resources between both player models.
        // Match replacement materials by name; this is a conservative alternative
        // state, not a claim that each character uses every table at runtime.
        if (std::filesystem::path(argv[1]).filename() == "MarioAnime.arc") {
            for (const char* player : {"Mario.arc", "Luigi.arc"}) {
                auto base = [&] {
                    PetariNative::HostAllocationScope host;
                    return read_archive(std::filesystem::path(argv[1]).parent_path() / player);
                }();
                for (size_t t = 0; t < archive.entries().size(); ++t) {
                    if (!archive.entries()[t].name.ends_with(".bmt")) continue;
                    Buffer table;
                    { PetariNative::HostAllocationScope host; table = archive.resourceData(t); }
                    for (size_t m = 0; m < base.entries().size(); ++m) {
                        if (!base.entries()[m].name.ends_with(".bdl")) continue;
                        Buffer image;
                        {
                            PetariNative::HostAllocationScope host;
                            image = base.resourceData(m);
                            resource = std::string(player) + ":" + base.entries()[m].name + "+" + archive.entries()[t].name;
                        }
                        std::fprintf(stderr, "[replay shared material] %s\n", resource.c_str());
                        model(image, &table);
                    }
                }
            }
        }
        for (size_t index = 0; index < archive.entries().size(); ++index) {
            const auto& entry = archive.entries()[index];
            if (entry.isDirectory() || !(entry.name.ends_with(".bdl") || entry.name.ends_with(".bmd") || entry.name.ends_with(".brlyt"))) continue;
            Buffer image;
            {
                PetariNative::HostAllocationScope host;
                resource = entry.name;
                image = archive.resourceData(index);
            }
            std::fprintf(stderr, "[replay model] %s\n", resource.c_str());
            if (entry.name.ends_with(".brlyt")) replay_layout(std::move(image), archive);
            else {
                model(image);
                for (size_t tableIndex = 0; tableIndex < archive.entries().size(); ++tableIndex) {
                    const auto& tableEntry = archive.entries()[tableIndex];
                    if (!tableEntry.name.ends_with(".bmt")) continue;
                    Buffer table;
                    {
                        PetariNative::HostAllocationScope host;
                        table = archive.resourceData(tableIndex);
                        resource = entry.name + "+" + tableEntry.name;
                    }
                    model(image, &table);
                }
            }
        }
        sql("COMMIT;");
        sqlite3_close(database);
        std::printf("models=%zu pairs=%zu draws=%zu configs=%zu shader_variants=%zu\n", models,pairs,draws,config_count(),variants);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[replay failure] resource=%s error=%s\n", resource.c_str(), error.what());
        return 1;
    }
}
