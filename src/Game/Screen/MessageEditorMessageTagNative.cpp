// Native (PETARI_NATIVE) implementation of MessageEditorMessageTag. The Wii definitions are
// in MessageTagSkipTagProcessor.cpp. Kept separate so message text handling can be linked
// and tested without the nw4r text writer.
#ifdef PETARI_NATIVE
#include "Game/Screen/MessageTagSkipTagProcessor.hpp"
#include <revolution/os.h>
#include <petari/host_allocation.hpp>
#include <unordered_map>
#include <vector>

MessageEditorMessageTag::MessageEditorMessageTag(const wchar_t* pMessage) : mMessage(pMessage) {
}

namespace {
    // Byte at a serialized tag offset, read from host-order code units.
    u8 getTagByte(const wchar_t* pTag, int offset) {
        u16 unit = static_cast< u16 >(pTag[offset / 2]);
        return (offset & 1) == 0 ? static_cast< u8 >(unit >> 8) : static_cast< u8 >(unit);
    }

    // Replacement strings referenced by string-argument tags. Tag slots are 32 bits, so
    // they hold 1-based handles into this table. Callers pass long-lived buffers, so the
    // registry deduplicates pointers and outlives individual game scene heaps.
    struct ArgStrings {
        std::vector<const wchar_t*> strings;
        std::unordered_map<const wchar_t*, u32> handles;
    };

    ArgStrings& argStrings() {
        PetariNative::HostAllocationScope hostAllocations;
        static auto* registry = new ArgStrings;
        return *registry;
    }

    u32 getArgStringHandle(const wchar_t* pString) {
        if (pString == nullptr) {
            return 0;
        }

        PetariNative::HostAllocationScope hostAllocations;
        auto& registry = argStrings();
        const auto found = registry.handles.find(pString);
        if (found != registry.handles.end()) return found->second;
        if (registry.strings.size() >= UINT32_MAX) {
            OSPanic(__FILE__, __LINE__, "Message argument string table is full");
        }
        const auto handle = static_cast<u32>(registry.strings.size() + 1);
        registry.strings.push_back(pString);
        registry.handles.emplace(pString, handle);
        return handle;
    }
}  // namespace

u32 MessageEditorMessageTag::getTagLength() const {
    return getTagByte(mMessage, 0) - 2;
}

u32 MessageEditorMessageTag::getSkipLength() const {
    return getTagLength() / 2;
}

s32 MessageEditorMessageTag::getParamLength() const {
    return getTagByte(mMessage, 0) - 6;
}

u8 MessageEditorMessageTag::getParam8(int index) const {
    return getTagByte(mMessage, 4 + index);
}

u16 MessageEditorMessageTag::getParam16(int index) const {
    return static_cast< u16 >(mMessage[2 + index]);
}

u32 MessageEditorMessageTag::getParam32(int index) const {
    return static_cast< u32 >(static_cast< u16 >(mMessage[2 + index * 2])) << 16 | static_cast< u16 >(mMessage[3 + index * 2]);
}

wchar_t* MessageEditorMessageTag::getParamPtr(int index) const {
    // UTF-16 parameters start on a code unit; odd byte offsets cannot be addressed natively.
    if ((index & 1) != 0) {
        OSPanic(__FILE__, __LINE__, "Tag parameter at odd byte offset %d", 4 + index);
    }

    return const_cast< wchar_t* >(mMessage + 2 + index / 2);
}

void MessageEditorMessageTag::setParam32(int index, u32 value) const {
    wchar_t* pParam = const_cast< wchar_t* >(mMessage + 2 + index * 2);
    pParam[0] = static_cast< wchar_t >(value >> 16);
    pParam[1] = static_cast< wchar_t >(value & 0xFFFF);
}

const wchar_t* MessageEditorMessageTag::getArgString() const {
    u32 handle = getParam32(0);
    const auto& strings = argStrings().strings;
    return handle == 0 || handle > strings.size() ? nullptr : strings[handle - 1];
}

void MessageEditorMessageTag::setArgString(const wchar_t* pString) const {
    setParam32(0, getArgStringHandle(pString));
}
#endif
