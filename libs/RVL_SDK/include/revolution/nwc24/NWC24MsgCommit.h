#ifndef RVL_SDK_NWC24_MSG_COMMIT_H
#define RVL_SDK_NWC24_MSG_COMMIT_H
#include "revolution/types.h"
#if defined(PETARI_NATIVE) && !defined(__cplusplus)
#include <stdlib.h>
#else
#include <cstdlib>
#endif

#include "revolution/nwc24/NWC24Types.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct NWC24MsgObj NWC24MsgObj;

NWC24Err NWC24CommitMsg(NWC24MsgObj* pMsg);

#ifdef __cplusplus
}
#endif
#endif
