#ifndef LIST_H
#define LIST_H

// Native C++ may include this inside another header's extern "C" block, so use
// the C header there.
#if defined(PETARI_NATIVE) || !defined(__cplusplus)
#include <stddef.h>
#else
#include <cstddef>
#endif
#include "revolution/types.h"

#ifdef __cplusplus
extern "C" {
#endif


typedef struct {
    void* prev;
    void* next;
} MEMLink;

typedef struct {
    void* head;
    void* tail;
    u16 num;
    u16 offs;
} MEMList;

void MEMInitList(MEMList*, u16);
void MEMAppendListObject(MEMList*, void*);
void MEMRemoveListObject(MEMList*, void*);
void* MEMGetNextListObject(MEMList*, void*);

#define MEM_INIT_LIST(list, structName, linkName) MEMInitList(list, offsetof(structName, linkName))

#ifdef __cplusplus
}
#endif

#endif  // LIST_H
