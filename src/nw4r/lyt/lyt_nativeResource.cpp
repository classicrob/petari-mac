// Native byte-order conversion for BRLYT and BRLAN resources. See
// nw4r::lyt::detail::NativeNormalizeResource in common.h.
#ifdef PETARI_NATIVE
#include "nw4r/lyt/common.h"
#include "nw4r/lyt/resources.h"
#include <petari/endian.hpp>
#include <cstring>

namespace nw4r {
    namespace lyt {
        namespace detail {
            namespace {
                // Walks a big-endian file twice: first to check every offset and size,
                // then to store each multi-byte field in host order. Each field is
                // visited exactly once per pass, so conversion never reads converted data.
                class Converter {
                public:
                    Converter(u8* file, u32 size, bool convert) : mFile(file), mSize(size), mConvert(convert), mValid(true) {
                    }

                    bool IsValid() const {
                        return mValid;
                    }

                    bool Fail() {
                        mValid = false;
                        return false;
                    }

                    // True if [offset, offset + length) lies inside the file.
                    bool Check(u32 offset, u32 length) {
                        if (offset > mSize || length > mSize - offset) {
                            mValid = false;
                        }
                        return mValid;
                    }

                    u16 U16(u32 offset) {
                        if (!Check(offset, 2)) {
                            return 0;
                        }
                        u16 value = PetariNative::readU16BE(mFile + offset);
                        if (mConvert) {
                            memcpy(mFile + offset, &value, sizeof(value));
                        }
                        return value;
                    }

                    u32 U32(u32 offset) {
                        if (!Check(offset, 4)) {
                            return 0;
                        }
                        u32 value = PetariNative::readU32BE(mFile + offset);
                        if (mConvert) {
                            memcpy(mFile + offset, &value, sizeof(value));
                        }
                        return value;
                    }

                    void U16Array(u32 offset, u32 count) {
                        if (Check(offset, count * 2)) {
                            for (u32 i = 0; i < count; i++) {
                                U16(offset + i * 2);
                            }
                        }
                    }

                    void U32Array(u32 offset, u32 count) {
                        if (count <= mSize / 4 && Check(offset, count * 4)) {
                            for (u32 i = 0; i < count; i++) {
                                U32(offset + i * 4);
                            }
                        }
                    }

                    // Reads a field that is not converted (bytes are endian-neutral).
                    u8 Byte(u32 offset) {
                        return Check(offset, 1) ? mFile[offset] : 0;
                    }

                    u32 Kind(u32 offset) {
                        return Check(offset, 4) ? PetariNative::readU32BE(mFile + offset) : 0;
                    }

                    bool CString(u32 offset) {
                        return Check(offset, 1) && memchr(mFile + offset, 0, mSize - offset) != nullptr ? true : Fail();
                    }

                private:
                    u8* mFile;
                    u32 mSize;
                    bool mConvert;
                    bool mValid;
                };

                const u32 kFileHeaderSize = sizeof(res::BinaryFileHeader);
                const u32 kBlockHeaderSize = sizeof(res::DataBlockHeader);
                const u32 kPaneSize = sizeof(res::Pane);
                const u32 kTexCoordSize = 4 * 2 * sizeof(f32);
                const u32 kTexSRTSize = 5 * sizeof(f32);

                void ConvertPane(Converter& c, u32 block) {
                    // translate(3), rotate(3), scale(2), size(2) follow flag/basePosition/alpha,
                    // the 16-byte name and 8 bytes of user data.
                    c.U32Array(block + 36, 10);
                }

                void ConvertMaterial(Converter& c, u32 material) {
                    c.U16Array(material + 20, 12);  // tevCols: 3 GXColorS10
                    u32 bits = c.U32(material + 60);
                    u32 offset = material + sizeof(res::Material);

                    u32 texMapNum = GetBits(bits, 0, 4);
                    for (u32 i = 0; i < texMapNum; i++) {
                        c.U16(offset);
                        offset += sizeof(res::TexMap);
                    }

                    u32 texSRTNum = GetBits(bits, 4, 4);
                    c.U32Array(offset, texSRTNum * 5);
                    offset += texSRTNum * kTexSRTSize;
                    offset += GetBits(bits, 8, 4) * sizeof(TexCoordGen);
                    offset += GetBits(bits, 25, 1) * sizeof(ChanCtrl);
                    offset += GetBits(bits, 27, 1) * sizeof(ut::Color);
                    offset += TestBit(bits, 12) ? GX_MAX_TEVSWAP * sizeof(TevSwapMode) : 0;

                    u32 indSRTNum = GetBits(bits, 13, 2);
                    c.U32Array(offset, indSRTNum * 5);
                    offset += indSRTNum * kTexSRTSize;
                    offset += GetBits(bits, 15, 3) * sizeof(IndirectStage);
                    offset += GetBits(bits, 18, 5) * sizeof(TevStage);
                    offset += TestBit(bits, 23) ? sizeof(AlphaCompare) : 0;
                    offset += TestBit(bits, 24) ? sizeof(BlendMode) : 0;
                    c.Check(material, offset - material);
                }

                void ConvertLayoutBlock(Converter& c, u32 block, u32 kind, u32 size) {
                    switch (kind) {
                    case res::DATABLOCKKIND_LAYOUT:
                        c.U32Array(block + 12, 2);
                        break;

                    case res::DATABLOCKKIND_TEXTURELIST:
                    case res::DATABLOCKKIND_FONTLIST: {
                        u16 num = c.U16(block + 8);
                        u32 entries = block + 12;
                        for (u32 i = 0; i < num && c.IsValid(); i++) {
                            u32 nameOffset = c.U32(entries + i * 8);
                            c.CString(entries + nameOffset);
                        }
                        break;
                    }

                    case res::DATABLOCKKIND_MATERIALLIST: {
                        u16 num = c.U16(block + 8);
                        for (u32 i = 0; i < num && c.IsValid(); i++) {
                            u32 materialOffset = c.U32(block + 12 + i * 4);
                            if (!c.Check(block + materialOffset, sizeof(res::Material))) {
                                break;
                            }
                            ConvertMaterial(c, block + materialOffset);
                        }
                        break;
                    }

                    case res::DATABLOCKKIND_PANE:
                    case res::DATABLOCKKIND_BOUNDING:
                        ConvertPane(c, block);
                        break;

                    case res::DATABLOCKKIND_PICTURE: {
                        ConvertPane(c, block);
                        c.U32Array(block + kPaneSize, 4);  // vtxCols
                        c.U16(block + kPaneSize + 16);     // materialIdx
                        u8 texCoordNum = c.Byte(block + kPaneSize + 18);
                        c.U32Array(block + sizeof(res::Picture), texCoordNum * 8);
                        break;
                    }

                    case res::DATABLOCKKIND_TEXTBOX: {
                        ConvertPane(c, block);
                        u32 base = block + kPaneSize;
                        c.U16(base + 0);  // textBufBytes
                        u16 textStrBytes = c.U16(base + 2);
                        c.U16(base + 4);  // materialIdx
                        c.U16(base + 6);  // fontIdx
                        u32 textStrOffset = c.U32(base + 12);
                        c.U32Array(base + 16, 2);  // textCols
                        c.U32Array(base + 24, 4);  // fontSize, charSpace, lineSpace
                        if (textStrBytes != 0) {
                            c.U16Array(block + textStrOffset, textStrBytes / 2);
                        }
                        break;
                    }

                    case res::DATABLOCKKIND_WINDOW: {
                        ConvertPane(c, block);
                        u32 base = block + kPaneSize;
                        c.U32Array(base, 4);  // inflation
                        u8 frameNum = c.Byte(base + 16);
                        u32 contentOffset = c.U32(base + 20);
                        u32 frameTableOffset = c.U32(base + 24);

                        u32 content = block + contentOffset;
                        c.U32Array(content, 4);  // vtxCols
                        c.U16(content + 16);     // materialIdx
                        u8 texCoordNum = c.Byte(content + 18);
                        c.U32Array(content + sizeof(res::WindowContent), texCoordNum * 8);

                        for (u32 i = 0; i < frameNum && c.IsValid(); i++) {
                            u32 frameOffset = c.U32(block + frameTableOffset + i * 4);
                            c.U16(block + frameOffset);  // WindowFrame::materialIdx
                        }
                        break;
                    }

                    case res::DATABLOCKKIND_GROUP: {
                        u16 paneNum = c.U16(block + 24);
                        c.Check(block + sizeof(res::Group), paneNum * 16u);
                        break;
                    }

                    default:
                        // Begin/end markers have no payload. Other blocks (usd1) are not read
                        // by this lyt version and keep their serialized bytes.
                        break;
                    }

                    c.Check(block, size);
                }

                void ConvertAnimationBlock(Converter& c, u32 block, u32 kind) {
                    switch (kind) {
                    case res::DATABLOCKKIND_PANEANIMTAG: {
                        c.U16(block + 8);   // tagOrder
                        u16 groupNum = c.U16(block + 10);
                        u32 nameOffset = c.U32(block + 12);
                        u32 groupsOffset = c.U32(block + 16);
                        c.U16(block + 20);  // startFrame
                        c.U16(block + 22);  // endFrame
                        c.CString(block + nameOffset);
                        c.Check(block + groupsOffset, groupNum * sizeof(AnimationGroupRef));
                        break;
                    }

                    case res::DATABLOCKKIND_PANEANIMSHARE: {
                        u32 infoOffset = c.U32(block + 8);
                        u16 shareNum = c.U16(block + 12);
                        c.Check(block + infoOffset, shareNum * sizeof(AnimationShareInfo));
                        break;
                    }

                    case res::DATABLOCKKIND_PANEANIMINFO: {
                        c.U16(block + 8);  // frameSize
                        u16 fileNum = c.U16(block + 12);
                        u16 contentNum = c.U16(block + 14);
                        u32 contentTable = block + c.U32(block + 16);

                        u32 fileTable = block + sizeof(res::AnimationBlock);
                        for (u32 i = 0; i < fileNum && c.IsValid(); i++) {
                            c.CString(fileTable + c.U32(fileTable + i * 4));
                        }

                        for (u32 i = 0; i < contentNum && c.IsValid(); i++) {
                            u32 content = block + c.U32(contentTable + i * 4);
                            u8 infoNum = c.Byte(content + 20);
                            u32 infoTable = content + sizeof(res::AnimationContent);
                            for (u32 j = 0; j < infoNum && c.IsValid(); j++) {
                                u32 info = content + c.U32(infoTable + j * 4);
                                u32 animKind = c.U32(info);
                                u8 targetNum = c.Byte(info + 4);
                                u32 targetTable = info + sizeof(res::AnimationInfo);
                                // Visibility and texture pattern use step keys; the others
                                // use Hermite keys, matching AnimTransformBasic::Animate.
                                bool stepKeys = animKind == res::ANIMATIONTYPE_RLVI || animKind == res::ANIMATIONTYPE_RLTP;
                                for (u32 k = 0; k < targetNum && c.IsValid(); k++) {
                                    u32 target = info + c.U32(targetTable + k * 4);
                                    u16 keyNum = c.U16(target + 4);
                                    u32 keys = target + c.U32(target + 8);
                                    for (u32 key = 0; key < keyNum && c.IsValid(); key++) {
                                        if (stepKeys) {
                                            c.U32(keys + key * sizeof(res::StepKey));
                                            c.U16(keys + key * sizeof(res::StepKey) + 4);
                                        } else {
                                            c.U32Array(keys + key * sizeof(res::HermiteKey), 3);
                                        }
                                    }
                                }
                            }
                        }
                        break;
                    }

                    default:
                        break;
                    }
                }

                bool ConvertFile(Converter& c, u32 signature) {
                    c.U16(4);  // byteOrder
                    c.U16(6);  // version
                    c.U32(8);  // fileSize
                    u16 headerSize = c.U16(12);
                    u16 dataBlocks = c.U16(14);
                    if (!c.IsValid() || headerSize < kFileHeaderSize) {
                        return c.Fail();
                    }

                    u32 block = headerSize;
                    for (u32 i = 0; i < dataBlocks && c.IsValid(); i++) {
                        u32 kind = c.Kind(block);
                        u32 size = c.U32(block + 4);
                        if (size < kBlockHeaderSize || !c.Check(block, size)) {
                            return c.Fail();
                        }

                        if (signature == res::FILESIGNATURE_RLYT) {
                            ConvertLayoutBlock(c, block, kind, size);
                        } else {
                            ConvertAnimationBlock(c, block, kind);
                        }
                        block += size;
                    }

                    return c.IsValid();
                }
            }  // namespace

            bool NativeNormalizeResource(void* pFile, u32 signature) {
                u8* file = static_cast< u8* >(pFile);
                if (file == nullptr || PetariNative::readU32BE(file) != signature) {
                    return false;
                }

                u16 byteOrder;
                memcpy(&byteOrder, file + 4, sizeof(byteOrder));
                if (byteOrder == 0xFEFF) {
                    // Already host order.
                    return true;
                }
                if (PetariNative::readU16BE(file + 4) != 0xFEFF) {
                    return false;
                }

                u32 fileSize = PetariNative::readU32BE(file + 8);
                if (fileSize < kFileHeaderSize) {
                    return false;
                }

                Converter validator(file, fileSize, false);
                if (!ConvertFile(validator, signature)) {
                    return false;
                }

                Converter converter(file, fileSize, true);
                return ConvertFile(converter, signature);
            }
        }  // namespace detail
    }  // namespace lyt
}  // namespace nw4r
#endif
