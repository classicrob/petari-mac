#include "Game/Player/GhostPacket.hpp"
#include <JSystem/JGeometry/TVec.hpp>
#include <cstdio>

GhostPacket::GhostPacket(void* pData, u32 len) {
    mDataPtr = (u8*)pData;
    mCurOffs = 0;
    _C = len;
}

void GhostPacket::read(u8* pOut, u32 len) {
    for (int i = 0; i < len; i++) {
        *pOut = mDataPtr[mCurOffs];
        pOut++;
        mCurOffs++;
    }
}

#ifdef PETARI_NATIVE
// Ghost data (.gst) stores multi-byte fields big-endian; the Wii copies the bytes in order.
void GhostPacket::read(u32* pOut) {
    u8 bytes[4];
    read(bytes, 4);
    *pOut = static_cast< u32 >(bytes[0]) << 24 | static_cast< u32 >(bytes[1]) << 16 | static_cast< u32 >(bytes[2]) << 8 | bytes[3];
}

void GhostPacket::read(s16* pOut) {
    u8 bytes[2];
    read(bytes, 2);
    *pOut = static_cast< s16 >(bytes[0] << 8 | bytes[1]);
}
#else
void GhostPacket::read(u32* pOut) {
    read((u8*)pOut, 4);
}

void GhostPacket::read(s16* pOut) {
    read((u8*)pOut, 2);
}
#endif

void GhostPacket::read(char** pOut) {
    char* v3 = (char*)&mDataPtr[mCurOffs];
    *pOut = (char*)v3;
    s32 offs = strlen(v3) + 1;
    mCurOffs += offs;
}

void GhostPacket::read(s8* pOut) {
    read((u8*)pOut, 1);
}

void GhostPacket::read(TVec3Sc* pOut) {
    read((u8*)&pOut->x, 1);
    read((u8*)&pOut->y, 1);
    read((u8*)&pOut->z, 1);
}

void GhostPacket::read(TVec3s* pOut) {
#ifdef PETARI_NATIVE
    read(&pOut->x);
    read(&pOut->y);
    read(&pOut->z);
#else
    read((u8*)&pOut->x, 2);
    read((u8*)&pOut->y, 2);
    read((u8*)&pOut->z, 2);
#endif
}
