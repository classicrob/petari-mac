#ifndef GXGEOMETRY_H
#define GXGEOMETRY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "revolution/types.h"
#include "revolution/gx/GXEnum.h"
#include "revolution/gx/GXStruct.h"

void GXSetVtxDesc(GXAttr, GXAttrType);
void GXClearVtxDesc(void);

void GXSetVtxAttrFmt(GXVtxFmt, GXAttr, GXCompCnt, GXCompType, u8);
void GXSetVtxAttrFmtv(GXVtxFmt, const GXVtxAttrFmtList *);
#ifdef PETARI_NATIVE
#include <stdbool.h>
void GXSetArray(GXAttr, const void *, u32, u8, bool);
#define GXSETARRAY(attr, data, size, stride, littleEndian) GXSetArray((attr), (data), (size), (stride), (littleEndian))
#else
void GXSetArray(GXAttr, const void *, u8);
#define GXSETARRAY(attr, data, size, stride, littleEndian) GXSetArray((attr), (data), (stride))
#endif
void GXBegin(GXPrimitive, GXVtxFmt, u16);

void GXSetTexCoordGen2(GXTexCoordID, GXTexGenType, GXTexGenSrc, u32, GXBool, u32);

void GXSetNumTexGens(u8);
void GXInvalidateVtxCache(void);

void GXSetLineWidth(u8, GXTexOffset);
void GXSetPointSize(u8, GXTexOffset);
void GXEnableTexOffsets(GXTexCoordID, GXBool, GXBool);

#ifdef PETARI_NATIVE
void GXEnd(void);
#else
static inline void GXEnd(void) {

}
#endif

void __GXSetSUTexRegs(void);
void __GXSetVCD(void);
void __GXSetVAT(void);
void __GXCalculateVLim(void);
void __GXSetGenMode(void);
void __GXUpdateBPMask(void);
void __GXSetProjection(void);
void __GXSetViewport(void);

#ifdef __cplusplus
}
#endif

#endif // GXGEOMETRY_H
