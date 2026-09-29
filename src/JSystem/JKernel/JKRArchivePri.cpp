#include "JSystem/JKernel/JKRArchive.hpp"
#include "JSystem/JKernel/JKRHeap.hpp"
#include <cstring>
#include <ctype.h>

// MSL's tolower returns values outside 0-255 unchanged. Host tolower is undefined
// for negative plain-char values, so keep MSL's result without calling it there.
#ifdef PETARI_NATIVE
#define JKR_ARC_TOLOWER(c) ((c) < 0 ? (c) : tolower(static_cast< unsigned char >(c)))
#else
#define JKR_ARC_TOLOWER(c) tolower(c)
#endif

u32 JKRArchive::sCurrentDirID;

JKRArchive::JKRArchive() {
    mIsMounted = false;
    mMountDir = MOUNT_DIRECTION_1;
#ifdef PETARI_NATIVE
    mRawInfoBlock = nullptr;
#endif
}

JKRArchive::JKRArchive(s32 entryNum, EMountMode mountMode) {
#ifdef PETARI_NATIVE
    mRawInfoBlock = nullptr;
#endif
    mIsMounted = false;
    mMountMode = mountMode;
    _34 = 1;
    _58 = 1;
    mHeap = JKRHeap::findFromRoot(this);

    if (mHeap == nullptr) {
        mHeap = JKRHeap::sCurrentHeap;
    }

    mEntryNum = entryNum;

    if (sCurrentVolume == nullptr) {
        sCurrentDirID = 0;
        sCurrentVolume = this;
    }
}

JKRArchive::~JKRArchive() {
}

bool JKRArchive::isSameName(CArcName& rName, u32 nameOffset, u16 hash) const {
    if (rName.mHash != hash) {
        return false;
    }

    return strcmp(mStringTable + nameOffset, rName.mName) == 0;
}

JKRArchive::SDIDirEntry* JKRArchive::findResType(u32 a1) const {
    SDIDirEntry* current = mDirs;
    for (u32 i = 0; i < mInfoBlock->mNrDirs; i++) {
        if (current->mID == a1) {
            return current;
        }

        current++;
    }

    return nullptr;
}

JKRArchive::SDIDirEntry* JKRArchive::findDirectory(const char* name, u32 directoryId) const {
    if (name == NULL) {
        return mDirs + directoryId;
    }

    CArcName arcName(&name, '/');
    SDIDirEntry* dirEntry = mDirs + directoryId;
    SDIFileEntry* fileEntry = mFiles + dirEntry->mFirstFileIndex;

    for (int i = 0; i < dirEntry->mNrFiles; i++) {
        if (isSameName(arcName, fileEntry->mNameOffset, fileEntry->mHash)) {
            if ((fileEntry->mFlag) & 2) {
                return findDirectory(name, fileEntry->mDataOffset);
            }
            break;
        }
        fileEntry++;
    }

    return NULL;
}

JKRArchive::SDIFileEntry* JKRArchive::findTypeResource(u32 a1, const char* pName) const {
    if (a1 != 0) {
        CArcName name;
        name.store(pName);

        SDIDirEntry* dir = findResType(a1);

        if (dir != nullptr) {
            SDIFileEntry* current = &mFiles[dir->mFirstFileIndex];

            for (s32 i = 0; i < dir->mNrFiles; i++) {
                if (isSameName(name, current->mNameOffset, current->mHash)) {
                    return current;
                }

                current++;
            }
        }
    }

    return nullptr;
}

JKRArchive::SDIFileEntry* JKRArchive::findFsResource(const char* name, u32 directoryId) const {
    if (name) {
        CArcName arcName(&name, '/');
        SDIDirEntry* dirEntry = mDirs + directoryId;
        SDIFileEntry* fileEntry = mFiles + dirEntry->mFirstFileIndex;

        for (int i = 0; i < dirEntry->mNrFiles; i++) {
            if (isSameName(arcName, fileEntry->mNameOffset, fileEntry->mHash)) {
                if ((fileEntry->mFlag) & 2) {
                    return findFsResource(name, fileEntry->mDataOffset);
                }

                if (name == NULL) {
                    return fileEntry;
                }

                return NULL;
            }
            fileEntry++;
        }
    }

    return NULL;
}

JKRArchive::SDIFileEntry* JKRArchive::findIdxResource(u32 index) const {
    if (index < mInfoBlock->mNrFiles) {
        return &mFiles[index];
    }

    return nullptr;
}

JKRArchive::SDIFileEntry* JKRArchive::findNameResource(const char* pName) const {
    SDIFileEntry* current = mFiles;

    CArcName name;
    name.store(pName);
    for (s32 i = 0; i < mInfoBlock->mNrFiles; i++) {
        if (isSameName(name, current->mNameOffset, current->mHash)) {
            return current;
        }

        current++;
    }

    return nullptr;
}

JKRArchive::SDIFileEntry* JKRArchive::findPtrResource(const void* pResource) const {
    SDIFileEntry* current = mFiles;
    for (s32 i = 0; i < mInfoBlock->mNrFiles; i++) {
        if (current->mFileData == pResource) {
            return current;
        }

        current++;
    }

    return nullptr;
}

JKRArchive::SDIFileEntry* JKRArchive::findIdResource(u16 fileID) const {
    if (fileID != 0xFFFF) {
        SDIFileEntry* current = mFiles;
        SDIFileEntry* indexed = &mFiles[fileID];

        if (indexed->mFileID == fileID && (indexed->mFlag & FILE_FLAG_FILE) != 0) {
            return indexed;
        }

        for (s32 i = 0; i < mInfoBlock->mNrFiles; i++) {
            if (current->mFileID == fileID && (current->mFlag & FILE_FLAG_FILE) != 0) {
                return current;
            }

            current++;
        }
    }

    return nullptr;
}

void JKRArchive::CArcName::store(const char* name) {
    mHash = 0;
    s32 length = 0;
    while (*name) {
        s32 ch = JKR_ARC_TOLOWER(*name);
        mHash = ch + mHash * 3;
        if (length < ARRAY_SIZE(mName)) {
            mName[length++] = ch;
        }
        name++;
    }

    mLength = static_cast< u16 >(length);
    mName[length] = 0;
}

const char* JKRArchive::CArcName::store(const char* name, char endChar) {
    mHash = 0;
    s32 length = 0;
    while (*name && *name != endChar) {
        s32 lch = JKR_ARC_TOLOWER(static_cast< int >(*name));
        mHash = lch + mHash * 3;
        if (length < ARRAY_SIZE(mName)) {
            mName[length++] = lch;
        }
        name++;
    }

    mLength = static_cast< u16 >(length);
    mName[length] = 0;

    if (*name == 0) {
        return NULL;
    }
    return name + 1;
}

void JKRArchive::setExpandSize(SDIFileEntry* pFile, u32 size) {
    u32 fileIndex = static_cast< u32 >(pFile - mFiles);
    if (mExpandSizes == nullptr || fileIndex >= mInfoBlock->mNrFiles) {
        return;
    }

    mExpandSizes[fileIndex] = size;
}

u32 JKRArchive::getExpandSize(SDIFileEntry* pFile) const {
    u32 fileIndex = static_cast< u32 >(pFile - mFiles);
    if (mExpandSizes == nullptr || fileIndex >= mInfoBlock->mNrFiles) {
        return 0;
    }

    return mExpandSizes[fileIndex];
}

#ifdef PETARI_NATIVE
// The Wii link placed this definition in src/Game/NPC/KinopioAstro.cpp. Native
// library targets link without game code, so it is defined here instead.
u32 JKRArchive::getExpandedResSize(const void* pResource) const {
    return getResSize(pResource);
}

#include <petari/endian.hpp>

namespace {
    const u32 kRarcHeaderSize = 0x20;
    const u32 kRarcInfoBlockSize = 0x20;
    const u32 kRarcDirEntrySize = 0x10;
    const u32 kRarcFileEntrySize = 0x14;

    bool rangeInside(u32 size, u32 offset, u32 length) {
        return offset <= size && length <= size - offset;
    }
}  // namespace

// Decodes a serialized RARC header. pDst may alias pSrc.
bool JKRArchive::readNativeHeader(RarcHeader* pDst, const void* pSrc) {
    const u8* src = static_cast< const u8* >(pSrc);
    RarcHeader header;
    header.mMagic = PetariNative::readU32BE(src + 0x00);
    header.mFileSize = PetariNative::readU32BE(src + 0x04);
    header.mHeaderSize = PetariNative::readU32BE(src + 0x08);
    header.mFileDataOffset = PetariNative::readU32BE(src + 0x0C);
    header.mTotalDataSize = PetariNative::readU32BE(src + 0x10);
    header.mMRamDataSize = PetariNative::readU32BE(src + 0x14);
    header.mARamDataSize = PetariNative::readU32BE(src + 0x18);
    header._1C = PetariNative::readU32BE(src + 0x1C);
    *pDst = header;
    return header.mMagic == RARC_MAGIC && header.mHeaderSize == kRarcHeaderSize && header.mFileDataOffset >= kRarcInfoBlockSize;
}

// Builds the host-layout tables from a serialized info block of infoSize bytes.
// dataSize bounds the file payload offsets. On success mInfoBlock, mDirs, mFiles
// and mStringTable point into a new block allocated from mHeap; on failure they
// are unchanged and nothing is allocated.
bool JKRArchive::setupNativeTables(const void* pInfoBlock, u32 infoSize, u32 dataSize, int alignment) {
    const u8* info = static_cast< const u8* >(pInfoBlock);
    if (info == nullptr || infoSize < kRarcInfoBlockSize) {
        return false;
    }

    u32 nrDirs = PetariNative::readU32BE(info + 0x00);
    u32 dirOffset = PetariNative::readU32BE(info + 0x04);
    u32 nrFiles = PetariNative::readU32BE(info + 0x08);
    u32 fileOffset = PetariNative::readU32BE(info + 0x0C);
    u32 stringSize = PetariNative::readU32BE(info + 0x10);
    u32 stringOffset = PetariNative::readU32BE(info + 0x14);

    if (nrDirs == 0 || nrDirs > infoSize / kRarcDirEntrySize || !rangeInside(infoSize, dirOffset, nrDirs * kRarcDirEntrySize) ||
        nrFiles > infoSize / kRarcFileEntrySize || !rangeInside(infoSize, fileOffset, nrFiles * kRarcFileEntrySize) ||
        !rangeInside(infoSize, stringOffset, stringSize) || stringSize == 0) {
        return false;
    }

    const char* strings = reinterpret_cast< const char* >(info + stringOffset);
    if (strings[stringSize - 1] != 0) {
        return false;
    }

    u32 nativeDirOffset = sizeof(RarcInfoBlock);
    u32 nativeFileOffset = ALIGN_NEXT(nativeDirOffset + nrDirs * sizeof(SDIDirEntry), sizeof(void*));
    u32 nativeStringOffset = nativeFileOffset + nrFiles * sizeof(SDIFileEntry);
    u32 blockSize = nativeStringOffset + stringSize;

    u8* block = static_cast< u8* >(JKRHeap::alloc(blockSize, alignment, mHeap));
    if (block == nullptr) {
        return false;
    }

    RarcInfoBlock* nativeInfo = reinterpret_cast< RarcInfoBlock* >(block);
    SDIDirEntry* dirs = reinterpret_cast< SDIDirEntry* >(block + nativeDirOffset);
    SDIFileEntry* files = reinterpret_cast< SDIFileEntry* >(block + nativeFileOffset);
    char* nativeStrings = reinterpret_cast< char* >(block + nativeStringOffset);

    bool valid = true;
    for (u32 i = 0; i < nrDirs && valid; i++) {
        const u8* src = info + dirOffset + i * kRarcDirEntrySize;
        SDIDirEntry& dir = dirs[i];
        dir.mID = PetariNative::readU32BE(src + 0x0);
        dir.mNameOffset = PetariNative::readU32BE(src + 0x4);
        dir.mHash = PetariNative::readU16BE(src + 0x8);
        dir.mNrFiles = PetariNative::readU16BE(src + 0xA);
        dir.mFirstFileIndex = PetariNative::readU32BE(src + 0xC);
        valid = dir.mNameOffset < stringSize && rangeInside(nrFiles, dir.mFirstFileIndex, dir.mNrFiles);
    }

    for (u32 i = 0; i < nrFiles && valid; i++) {
        const u8* src = info + fileOffset + i * kRarcFileEntrySize;
        SDIFileEntry& file = files[i];
        file.mFileID = PetariNative::readU16BE(src + 0x0);
        file.mHash = PetariNative::readU16BE(src + 0x2);
        file.mFlag = src[0x4];
        file.mNameOffset = PetariNative::readU24BE(src + 0x5);
        file.mDataOffset = PetariNative::readU32BE(src + 0x8);
        file.mDataSize = PetariNative::readU32BE(src + 0xC);
        file.mFileData = nullptr;

        if (file.mNameOffset >= stringSize) {
            valid = false;
        } else if (file.mFlag & FILE_FLAG_FOLDER) {
            // ".." in the root directory has no parent.
            valid = file.mDirIndex < nrDirs || file.mDirIndex == 0xFFFFFFFF;
        } else {
            valid = (file.mFlag & FILE_FLAG_FILE) != 0 && rangeInside(dataSize, file.mDataOffset, file.mDataSize);
        }
    }

    if (!valid) {
        JKRHeap::free(block, nullptr);
        return false;
    }

    nativeInfo->mNrDirs = nrDirs;
    nativeInfo->mDirOffset = nativeDirOffset;
    nativeInfo->mNrFiles = nrFiles;
    nativeInfo->mFileOffset = nativeFileOffset;
    nativeInfo->mStringTableSize = stringSize;
    nativeInfo->mStringTableOffset = nativeStringOffset;
    nativeInfo->mNextAvailableFileID = PetariNative::readU16BE(info + 0x18);
    nativeInfo->mFileIDIsIndex = PetariNative::readU16BE(info + 0x1A);
    nativeInfo->_1C = PetariNative::readU32BE(info + 0x1C);
    memcpy(nativeStrings, strings, stringSize);

    mInfoBlock = nativeInfo;
    mDirs = dirs;
    mFiles = files;
    mStringTable = nativeStrings;
    return true;
}
#endif
