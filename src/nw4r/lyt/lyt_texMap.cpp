#include "nw4r/lyt/common.h"
#include "nw4r/lyt/texMap.h"
#include <revolution/gx/GXEnum.h>
#include <revolution/gx/GXGet.h>
#include <revolution/gx/GXStruct.h>
#include <revolution/tpl.h>
#include <stdint.h>

#ifdef PETARI_NATIVE
#include <petari/endian.hpp>
#endif

namespace nw4r {
    namespace lyt {
        void TexMap::Get(_GXTexObj* pTexObj) const {
            if (detail::IsCITexelFormat(GetTexelFormat())) {
                u32 tlutName = GXGetTexObjTlut(pTexObj);
                GXInitTexObjCI(pTexObj, mImage, mWidth, mHeight, GXCITexFmt(GetTexelFormat()), GetWrapModeS(), GetWrapModeT(), IsMipMap(), tlutName);
            } else {
                GXInitTexObj(pTexObj, mImage, mWidth, mHeight, GetTexelFormat(), GetWrapModeS(), GetWrapModeT(), IsMipMap());
            }

            GXInitTexObjLOD(pTexObj, GetMinFilter(), GetMagFilter(), GetMinLOD(), GetMaxLOD(), GetLODBias(), IsBiasClampEnable(), IsEdgeLODEnable(),
                            GetAnisotropy());
        }

        void TexMap::Get(_GXTlutObj* pTlutObj) const {
            GXInitTlutObj(pTlutObj, GetPalette(), GetPaletteFormat(), GetPaletteEntryNum());
        }

        void TexMap::Set(const GXTexObj& texObj) {
            void* image;
            u16 width, height;
            GXTexFmt format;
            GXTexWrapMode wrapS, wrapT;
            GXBool mipmap;

            GXGetTexObjAll(&texObj, &image, &width, &height, &format, &wrapS, &wrapT, &mipmap);

            mImage = image;
            SetSize(width, height);
            mBits.textureFormat = format;
            SetWrapMode(wrapS, wrapT);
            SetMipMap(mipmap);

            GXTexFilter minFilter, magFilter;
            f32 minLOD, maxLOD, lodBias;
            GXBool biasCLampEnable, edgeLODEnable;
            GXAnisotropy aniso;
            GXGetTexObjLODAll(&texObj, &minFilter, &magFilter, &minLOD, &maxLOD, &lodBias, &biasCLampEnable, &edgeLODEnable, &aniso);

            SetFilter(minFilter, magFilter);
            SetLOD(minLOD, maxLOD);
            SetLODBias(lodBias);
            SetBiasClampEnable(biasCLampEnable);
            SetEdgeLODEnable(edgeLODEnable);
            mBits.anisotropy = aniso;
        }

        void TexMap::ReplaceImage(const TPLDescriptor* pTPLDesc) {
            const TPLHeader& header = *pTPLDesc->textureHeader;
            mImage = header.data;
            SetSize(header.width, header.height);
            SetTexelFormat(GXTexFmt(header.format));

            if (const TPLClutHeader* const pClut = pTPLDesc->CLUTHeader) {
                SetPalette(pClut->data);
                SetPaletteFormat(pClut->format);
                SetPaletteEntryNum(pClut->numEntries);
            } else {
                SetPalette(nullptr);
                SetPaletteFormat(GXTlutFmt(0));
                SetPaletteEntryNum(0);
            }
        }

        void TexMap::ReplaceImage(TPLPalette* p, u32 id) {
#ifdef PETARI_NATIVE
            // TPLBind relocates file offsets into the 32-bit pointer fields of the
            // TPL structures, which cannot hold host pointers. Read the big-endian
            // file directly instead; it is not modified. Image and palette data
            // stay big-endian, as GX texture formats are byte-defined.
            const u8* file = reinterpret_cast< const u8* >(p);
            const u32 imageNum = PetariNative::readU32BE(file + 4);
            if (id >= imageNum) {
                OSPanic(__FILE__, __LINE__, "TexMap::ReplaceImage: TPL image %u of %u", id, imageNum);
            }

            const u8* entry = file + PetariNative::readU32BE(file + 8) + id * 8;
            const u8* image = file + PetariNative::readU32BE(entry);
            const u32 paletteOffset = PetariNative::readU32BE(entry + 4);

            mImage = const_cast< u8* >(file) + PetariNative::readU32BE(image + 8);
            SetSize(PetariNative::readU16BE(image + 2), PetariNative::readU16BE(image));
            SetTexelFormat(GXTexFmt(PetariNative::readU32BE(image + 4)));

            if (paletteOffset != 0) {
                const u8* palette = file + paletteOffset;
                SetPalette(const_cast< u8* >(file) + PetariNative::readU32BE(palette + 8));
                SetPaletteFormat(GXTlutFmt(PetariNative::readU32BE(palette + 4)));
                SetPaletteEntryNum(PetariNative::readU16BE(palette));
            } else {
                SetPalette(nullptr);
                SetPaletteFormat(GXTlutFmt(0));
                SetPaletteEntryNum(0);
            }
#else
            if (reinterpret_cast< uintptr_t >(p->descriptorArray) < 0x80000000) {
                TPLBind(p);
            }

            ReplaceImage(TPLGet(p, id));
#endif
        }
    };  // namespace lyt
};  // namespace nw4r
