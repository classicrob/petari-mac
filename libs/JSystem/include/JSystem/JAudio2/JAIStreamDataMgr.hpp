#pragma once

#include "JSystem/JAudio2/JAISound.hpp"
#include <stdint.h>

struct JAIStreamDataMgr {
    virtual s32 getStreamFileEntry(JAISoundID) = 0;
    virtual ~JAIStreamDataMgr();
};

struct JAIStreamAramMgr {
    virtual void* newStreamAram(u32*) = 0;
    virtual bool deleteStreamAram(uintptr_t) = 0;
    virtual ~JAIStreamAramMgr();
};
