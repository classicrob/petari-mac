#include "revolution.h"
#include <ctype.h>
#include <locale.h>
#ifdef PETARI_NATIVE
#include <petari/endian.hpp>
#include <cstdio>
#endif

/* this is here because it won't be inlined otherwise */
#ifndef PETARI_NATIVE
inline int tolower(int c) {
    return ((c < 0) || (c >= 0x100)) ? c : (int) (_current_locale.ctype_cmpt_ptr->lower_map_ptr[c]);
}
#endif

typedef struct FSTEntry FSTEntry;

struct FSTEntry {
    unsigned int isDirAndStringOff;
    unsigned int parentOrPosition;
    unsigned int nextEntryOrLength;
};

#ifdef PETARI_NATIVE
#define entryIsDir(fstStart, i) (PetariNative::readU32BE(&(fstStart)[i].isDirAndStringOff) >> 24)
#define stringOff(fstStart, i) (PetariNative::readU32BE(&(fstStart)[i].isDirAndStringOff) & 0x00FFFFFF)
#define parentDir(fstStart, i) PetariNative::readU32BE(&(fstStart)[i].parentOrPosition)
#define nextDir(fstStart, i) PetariNative::readU32BE(&(fstStart)[i].nextEntryOrLength)
#define filePosition(fstStart, i) parentDir(fstStart, i)
#define fileLength(fstStart, i) nextDir(fstStart, i)
#else
#define entryIsDir(fstStart, i)     \
    ( ( ( fstStart[i].isDirAndStringOff & 0xFF000000 ) == 0 )? FALSE : TRUE )
#define stringOff(fstStart, i)      \
        ( fstStart[i].isDirAndStringOff & 0x00FFFFFF )
#define parentDir(fstStart, i)       \
        ( fstStart[i].parentOrPosition )
#define nextDir(fstStart, i)        \
        ( fstStart[i].nextEntryOrLength )
#define filePosition(fstStart, i)       \
        ( fstStart[i].parentOrPosition )
#define fileLength(fstStart, i)         \
        ( fstStart[i].nextEntryOrLength )
#endif

BOOL ARCInitHandle(void* arcStart, ARCHandle* handle) {
#ifdef PETARI_NATIVE
    if (!arcStart || !handle) return FALSE;
    const auto* header = static_cast<const u8*>(arcStart);
    if (PetariNative::readU32BE(header) != 0x55AA382D) return FALSE;
    const u32 fstStart = PetariNative::readU32BE(header + 4);
    const u32 fstSize = PetariNative::readU32BE(header + 8);
    const u32 fileStart = PetariNative::readU32BE(header + 12);
    if (fstStart < 32 || fstSize < 12 || fileStart < fstStart || fstSize > fileStart - fstStart) return FALSE;
    auto* entries = reinterpret_cast<FSTEntry*>(static_cast<u8*>(arcStart) + fstStart);
    const u32 count = nextDir(entries, 0);
    if (!entryIsDir(entries, 0) || count == 0 || count > fstSize / 12) return FALSE;
    handle->archiveStartAddr = arcStart;
    handle->FSTStart = entries;
    handle->fileStart = static_cast<u8*>(arcStart) + fileStart;
    handle->entryNum = count;
    handle->FSTStringStart = reinterpret_cast<char*>(entries + count);
    handle->FSTLength = fstSize;
    handle->currDir = 0;
    return TRUE;
#else
    FSTEntry* FSTEntries;
    ARCHeader* arcHeader = (ARCHeader*)arcStart;

    if (arcHeader->magic != 0x55AA382D) {
        OSPanic(__FILE__, 0x4A, "ARCInitHandle: bad archive format");
    }

    handle->archiveStartAddr = arcStart;
    handle->FSTStart = FSTEntries = (void*)((u32)arcStart + arcHeader->fstStart);
    handle->fileStart = (void*)((u32)arcStart + arcHeader->fileStart);
    handle->entryNum = nextDir(FSTEntries, 0);
    handle->FSTStringStart = (char*)&(FSTEntries[handle->entryNum]);
    handle->FSTLength = (u32)arcHeader->fstSize;
    handle->currDir = 0;
    return TRUE;
#endif
}

// These SDK entry points are retained in the retail binary for the product.sel export symbol table.
#pragma push
#pragma force_active on
BOOL ARCOpen(ARCHandle* handle, const char* fileName, ARCFileInfo* af) {
    s32 entry;
    char currentDir[128];
    FSTEntry* FSTEntries = (FSTEntry*)handle->FSTStart;
    entry = ARCConvertPathToEntrynum(handle, fileName);

    if (0 > entry) {
        ARCGetCurrentDir(handle, currentDir, 128);
#ifdef PETARI_NATIVE
        std::fprintf(stderr, "Warning: ARCOpen(): file '%s' was not found under %s in the archive.\n", fileName, currentDir);
#else
        OSReport("Warning: ARCOpen(): file '%s' was not found under %s in the archive.\n", fileName, currentDir);
#endif
        return FALSE;
    }

    if ((entry < 0) || entryIsDir(FSTEntries, entry)) {
        return FALSE;
    } 

    af->handle = handle;
    af->startOffset = filePosition(FSTEntries, entry);
    af->length = fileLength(FSTEntries, entry);

    return TRUE;
}
#pragma pop

BOOL ARCFastOpen(ARCHandle* handle, s32 entrynum, ARCFileInfo* af) {
    FSTEntry* FSTEntries = (FSTEntry*)handle->FSTStart;

    if ((entrynum < 0) || (entrynum >= handle->entryNum) || entryIsDir(FSTEntries, entrynum)) {
        return FALSE;
    }

    af->handle = handle;
    af->startOffset = filePosition(FSTEntries, entrynum);
    af->length = fileLength(FSTEntries, entrynum);
    return TRUE;
}

static BOOL isSame(const char* path, const char* string) {
    while(*string != '\0') {
#ifdef PETARI_NATIVE
        if (tolower((unsigned char)*path++) != tolower((unsigned char)*string++)) {
#else
        if (tolower(*path++) != tolower(*string++)) {
#endif
            return FALSE;
        }
    }

    if ((*path == '/') || (*path == '\0')) {
        return TRUE;
    }

    return FALSE;
}

s32 ARCConvertPathToEntrynum(ARCHandle* handle, const char* pathPtr)
{
    const char*  ptr;
    char* stringPtr;
    BOOL isDir;
    s32 length;
    u32 dirLookAt;
    u32 i;
    const char* origPathPtr = pathPtr;
    FSTEntry* FSTEntries;

    dirLookAt = handle->currDir;
    FSTEntries = (FSTEntry*)handle->FSTStart;

    while (1) {
        if (*pathPtr == '\0') {
            return (s32)dirLookAt;
        } 
        else if (*pathPtr == '/') {
            dirLookAt = 0;
            pathPtr++;
            continue;
        }
        else if (*pathPtr == '.') {
            if (*(pathPtr + 1) == '.') {
                if (*(pathPtr + 2) == '/') {
                    dirLookAt = parentDir(FSTEntries, dirLookAt);
                    pathPtr += 3;
                    continue;
                }
                else if (*(pathPtr + 2) == '\0') {
                    return (s32)parentDir(FSTEntries, dirLookAt);
                }
            }
            else if (*(pathPtr + 1) == '/') {
                pathPtr += 2;
                continue;
            }
            else if (*(pathPtr + 1) == '\0') {
                return (s32)dirLookAt;
            }
        }

        for(ptr = pathPtr; (*ptr != '\0') && (*ptr != '/'); ptr++);
        isDir = (*ptr == '\0')? FALSE : TRUE;
        length = (s32)(ptr - pathPtr);
        ptr = pathPtr;

        for(i = dirLookAt + 1; i < nextDir(FSTEntries, dirLookAt);
            i = entryIsDir(FSTEntries, i)? nextDir(FSTEntries, i): (i+1) )
        {
dot:
            if ((entryIsDir(FSTEntries, i) == FALSE) && (isDir == TRUE)) {
                continue;
            }

            stringPtr = handle->FSTStringStart + stringOff(FSTEntries, i);

            if (*stringPtr == '.' && *(stringPtr + 1) == '\0') {
                i++;
                goto dot;
            }

            if (isSame(ptr, stringPtr) == TRUE) {
                goto next_hier;
            }
        }

        return -1;

      next_hier:
        if (!isDir) {
            return (s32)i;
        }

        dirLookAt = i;
        pathPtr += length + 1;
    }

    // the world ends if this is reached
}

static u32 myStrncpy(char* dest, char* src, u32 maxlen) {
    u32 i = maxlen;

    while ((i > 0) && (*src != 0)) {
        *dest++ = *src++;
        i--;
    }
    
    return (maxlen - i);
}

static u32 entryToPath(ARCHandle* handle, u32 entry, char* path, u32 maxlen) {
    char* name;
    u32 loc;
    FSTEntry* FSTEntries = (FSTEntry*)handle->FSTStart;

    if (entry == 0) {
        return 0;
    }

    name = handle->FSTStringStart + stringOff(FSTEntries, entry);
    loc = entryToPath(handle, parentDir(FSTEntries, entry), path, maxlen);
    
    if (loc == maxlen) {
        return loc;
    }

    *(path + loc++) = '/';
    loc += myStrncpy(path + loc, name, maxlen - loc);
    return loc;
}

static BOOL ARCConvertEntrynumToPath(ARCHandle* handle, s32 entrynum, char* path, u32 maxlen) {
    u32 loc;
    FSTEntry* FSTEntries = (FSTEntry*)handle->FSTStart;

    loc = entryToPath(handle, (u32)entrynum, path, maxlen);

    if (loc == maxlen) {
        path[maxlen - 1] = '\0';
        return FALSE;
    }

    if (entryIsDir(FSTEntries, entrynum)) {
        if (loc == maxlen - 1) {
            path[loc] = '\0';
            return FALSE;
        }

        path[loc++] = '/';
    }
    
    path[loc] = '\0';
    return TRUE;
}

#ifndef PETARI_NATIVE
static
#endif
BOOL ARCGetCurrentDir(ARCHandle* handle, char* path, u32 maxlen) {
#ifdef PETARI_NATIVE
    if (!path || maxlen == 0) return FALSE;
#endif
    return ARCConvertEntrynumToPath(handle, (s32)handle->currDir, path, maxlen);
}

void* ARCGetStartAddrInMem(ARCFileInfo* af) {
    ARCHandle* handle = af->handle;
#ifdef PETARI_NATIVE
    return static_cast<u8*>(handle->archiveStartAddr) + af->startOffset;
#else
    return (void*)((u32)handle->archiveStartAddr + af->startOffset);
#endif
}

u32 ARCGetLength(ARCFileInfo* af) {
    return af->length;
}

BOOL ARCClose(ARCFileInfo* af) {
    return TRUE;
}

BOOL ARCChangeDir(ARCHandle* handle, const char* dirName) {
    s32 entry;   
    FSTEntry* FSTEntries;

    entry = ARCConvertPathToEntrynum(handle, dirName);
    FSTEntries = (FSTEntry*)handle->FSTStart;

    if ((entry < 0) || (entryIsDir(FSTEntries, entry) == FALSE)) {
        return FALSE;
    }

    handle->currDir = (u32)entry;
    return TRUE;
}

BOOL ARCOpenDir(ARCHandle* handle, const char* dirName, ARCDir* dir) {
    s32         entry;
    FSTEntry*   FSTEntries;

    entry = ARCConvertPathToEntrynum(handle, dirName);
    FSTEntries = (FSTEntry*)handle->FSTStart;

    if ((entry < 0) || (entryIsDir(FSTEntries, entry) == FALSE)) {
        return FALSE;
    }

    dir->handle = handle;
    dir->entryNum = (u32)entry;
    dir->location = (u32)entry + 1;
    dir->next = nextDir(FSTEntries, entry);
    return TRUE;
}

BOOL ARCReadDir(ARCDir* dir, ARCDirEntry* dirent) {
    u32 loc;
    FSTEntry* FSTEntries;
    ARCHandle* handle;

    handle = dir->handle;
    FSTEntries = (FSTEntry*)handle->FSTStart;
    loc = dir->location;
retry:
    if ((loc <= dir->entryNum) || (dir->next <= loc)) {
        return FALSE;
    }

    dirent->handle = handle;
    dirent->entryNum = loc;
    dirent->isDir = entryIsDir(FSTEntries, loc);
    dirent->name = handle->FSTStringStart + stringOff(FSTEntries, loc);

    if (dirent->name[0] == '.' && dirent->name[1] == '\0') {
        loc++;
        goto retry;
    }

    dir->location = entryIsDir(FSTEntries, loc)? nextDir(FSTEntries, loc) : (loc+1);
    return TRUE;
}

BOOL ARCCloseDir(ARCDir* dir) {
    return TRUE;
}
