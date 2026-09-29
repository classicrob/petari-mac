#pragma once

#include <nw4r/ut/TagProcessorBase.h>

class MessageTagSkipTagProcessor : public nw4r::ut::TagProcessorBase< wchar_t > {
public:
    MessageTagSkipTagProcessor();

    virtual ~MessageTagSkipTagProcessor() {
    }

    virtual nw4r::ut::TagProcessorBase< wchar_t >::Operation Process(u16, ContextType*);
    virtual nw4r::ut::TagProcessorBase< wchar_t >::Operation CalcRect(nw4r::ut::Rect*, u16, ContextType*);

    nw4r::ut::TagProcessorBase< wchar_t >::Operation skipTag(nw4r::ut::Rect*, ContextType*, bool);
};

// Tag layout after the 0x1A code unit, in serialized bytes: u8 size (including the 0x1A
// unit), u8 group, u16 tag, then parameters. Natively the text is held as host-order
// UTF-16 code units (see MessageData), so byte fields are decoded from code units.
class MessageEditorMessageTag {
public:
    MessageEditorMessageTag(const wchar_t*);
    MessageEditorMessageTag(const nw4r::ut::PrintContext< wchar_t >*);

#ifdef PETARI_NATIVE
    s32 getGroup() const {
        return static_cast< u16 >(mMessage[0]) & 0xFF;
    }
    int getTag() const {
        return static_cast< u16 >(mMessage[1]);
    }

    bool isGroupTagId(int group, int tag) const NO_INLINE {
        return getGroup() == group && getTag() == tag;
    }

    /// @brief Stores a 32-bit parameter in the tag (text buffers only, not resource text).
    void setParam32(int index, u32 value) const;

    /// @brief Replacement string of a string-argument tag (group 7). The Wii build stores the
    /// pointer in the 32-bit parameter slot; natively the slot holds a handle.
    const wchar_t* getArgString() const;
    void setArgString(const wchar_t* pString) const;
#else
    s32 getGroup() const {
        return reinterpret_cast< const u8* >(mMessage)[1];
    }
    int getTag() const {
        return mMessage[1];
    }

    bool isGroupTagId(int group, int tag) const NO_INLINE {
        return reinterpret_cast< const u8* >(mMessage)[1] == group && mMessage[1] == tag;
    }
#endif

    u32 getTagLength() const;
    u32 getSkipLength() const;
    s32 getParamLength() const;
    u8 getParam8(int) const;
    u16 getParam16(int) const;
    u32 getParam32(int) const;
    wchar_t* getParamPtr(int) const;

    /* 0x00 */ const wchar_t* mMessage;
};
