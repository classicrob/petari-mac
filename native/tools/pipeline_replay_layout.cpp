#include "pipeline_replay.hpp"
#include "archive.hpp"
#include <petari/host_allocation.hpp>
#include <nw4r/lyt/common.h>
#include <nw4r/lyt/layout.h>
#include <nw4r/lyt/material.h>
#include <nw4r/lyt/resourceAccessor.h>
#include <nw4r/lyt/resources.h>
#include <revolution/mem.h>
#include <JSystem/J3DGraphBase/J3DSys.hpp>
#include <map>
#include <set>
#include <vector>
#include <cstring>
#include <strings.h>
#include <stdexcept>

namespace {
using Buffer = std::vector<uint8_t>;
using namespace nw4r::lyt;
class Accessor : public ResourceAccessor {
    const PetariNative::Resource::Archive& archive;
    std::map<std::string, Buffer> resources;
public:
    explicit Accessor(const PetariNative::Resource::Archive& value) : archive(value) {}
    void* GetResource(ResType, const char* name, u32* size) override {
        auto found = resources.find(name);
        if (found == resources.end()) {
            for (size_t i = 0; i < archive.entries().size(); ++i) {
                if (strcasecmp(archive.entries()[i].name.c_str(), name) == 0 && !archive.entries()[i].isDirectory()) {
                    found = resources.emplace(name, archive.resourceData(i)).first;
                    break;
                }
            }
        }
        if (found == resources.end()) {
            std::fprintf(stderr, "[replay missing texture] %s\n", name);
            throw std::runtime_error("layout texture not present in archive");
        }
        if (size) *size = found->second.size();
        return found->second.data();
    }
};
}

void replay_layout(std::vector<uint8_t> image, const PetariNative::Resource::Archive& archive) {
    PetariNative::HostAllocationScope host;
    using namespace nw4r::lyt;
    require(detail::NativeNormalizeResource(image.data(), res::FILESIGNATURE_RLYT), "invalid BRLYT resource");
    Buffer allocatorMemory(8 * 1024 * 1024);
    MEMAllocator allocator;
    const auto heap = MEMCreateExpHeapEx(allocatorMemory.data(), allocatorMemory.size(), 0);
    MEMInitAllocatorForExpHeap(&allocator, heap, 8);
    auto* previousAllocator = Layout::mspAllocator;
    Layout::mspAllocator = &allocator;
    Accessor accessor(archive);
    ResBlockSet blocks{};
    blocks.pResAccessor = &accessor;
    auto* header = reinterpret_cast<const res::BinaryFileHeader*>(image.data());
    size_t offset = header->headerSize;
    std::map<unsigned, std::set<unsigned>> texcoordCounts;
    for (unsigned i = 0; i < header->dataBlocks; ++i) {
        auto* block = reinterpret_cast<const res::DataBlockHeader*>(image.data() + offset);
        const auto kind = detail::GetSignatureInt(block->kind);
        if (kind == 'mat1') blocks.pMaterialList = reinterpret_cast<const res::MaterialList*>(block);
        if (kind == 'txl1') blocks.pTextureList = reinterpret_cast<const res::TextureList*>(block);
        if (kind == 'pic1') {
            auto* pane = reinterpret_cast<const res::Picture*>(block);
            texcoordCounts[pane->materialIdx].insert(pane->texCoordNum);
        }
        if (kind == 'wnd1') {
            auto* pane = reinterpret_cast<const res::Window*>(block);
            auto* content = detail::ConvertOffsToPtr<res::WindowContent>(pane, pane->contentOffset);
            texcoordCounts[content->materialIdx].insert(content->texCoordNum);
            auto* offsets = detail::ConvertOffsToPtr<u32>(pane, pane->frameOffsetTableOffset);
            for (unsigned frame = 0; frame < pane->frameNum; ++frame) {
                auto* record = detail::ConvertOffsToPtr<res::WindowFrame>(pane, offsets[frame]);
                texcoordCounts[record->materialIdx].insert(1);
            }
        }
        offset += block->size;
    }
    if (blocks.pMaterialList) {
        auto* offsets = detail::ConvertOffsToPtr<u32>(blocks.pMaterialList, sizeof(res::MaterialList));
        for (materialIndex = 0; materialIndex < blocks.pMaterialList->materialNum; ++materialIndex) {
            auto* materialResource = detail::ConvertOffsToPtr<res::Material>(blocks.pMaterialList, offsets[materialIndex]);
            Material material(materialResource, blocks);
            auto counts = texcoordCounts[materialIndex];
            if (counts.empty()) counts.insert(materialResource->resNum.GetTexCoordGenNum());
            for (auto count : counts) {
                require(count <= 8, "invalid layout texcoord count");
                shapeIndex = count;
                for (bool modulate : {false, true}) {
                    for (u8 alpha : {u8(255), u8(128)}) {
                        reset();
                        Buffer commands(65536);
                        record_begin(commands.data(), commands.size());
                        j3dSys.reinitGX();
                        GXSetZMode(GX_FALSE, GX_NEVER, GX_FALSE);
                        GXSetCullMode(GX_CULL_NONE);
                        GXSetColorUpdate(GX_TRUE);
                        GXSetAlphaUpdate(GX_TRUE);
                        GXSetFog(GX_FOG_NONE, 0, 1, 0, 1, GXColor{0, 0, 0, 0});
                        bool color = material.SetupGX(modulate, alpha);
                        detail::SetVertexFormat(color, count);
                        const auto bytes = record_end();
                        require(bytes < commands.size(), "layout display list overflow");
                        replay(commands.data(), bytes, false);
                        emit_quad();
                    }
                }
                ++pairs;
            }
        }
    }
    Layout::mspAllocator = previousAllocator;
    MEMDestroyExpHeap(heap);
    ++models;
}
