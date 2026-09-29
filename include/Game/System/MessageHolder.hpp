#pragma once

#include <revolution/types.h>
#ifdef PETARI_NATIVE
#include "Game/Util/BigEndian.hpp"
#endif

class JMapInfo;
class TalkMessageInfo;
class TalkNode;

#ifdef PETARI_NATIVE
// BMG block headers as stored in the resource (big-endian).
struct MessageInfoBlock {
    BigEndianValue< u32 > mMagic;
    BigEndianValue< u32 > mBlockSize;
    BigEndianValue< u16 > mItemCount;
    BigEndianValue< u16 > mItemSize;
    BigEndianValue< u32 > _C;
};

struct MessageDataBlock {
    BigEndianValue< u32 > mMagic;
    BigEndianValue< u32 > mBlockSize;
};

struct MessageFlowBlock {
    BigEndianValue< u32 > mMagic;
    BigEndianValue< u32 > mBlockSize;
    BigEndianValue< u16 > mNodeCount;
    BigEndianValue< u16 > _A;
    BigEndianValue< u32 > _C;
};

struct MessageFLI1Block {
    BigEndianValue< u32 > mMagic;
    BigEndianValue< u32 > mBlockSize;
};
#else
struct MessageInfoBlock {
    u32 mMagic;
    u32 mBlockSize;
    u16 mItemCount;
    u16 mItemSize;
    u32 _C;
};

struct MessageDataBlock {
    u32 mMagic;
    u32 mBlockSize;
};

struct MessageFlowBlock {
    u32 mMagic;
    u32 mBlockSize;
    u16 mNodeCount;
    u16 _A;
    u32 _C;
};

struct MessageFLI1Block {
    u32 mMagic;
    u32 mBlockSize;
};
#endif

class MessageData {
public:
    MessageData(const char*);
#ifdef PETARI_NATIVE
    /// @brief Builds the message tables from BMG and MessageId.tbl resources in memory.
    MessageData(const void* pMessageData, const void* pIdTable);
    void initFromResource(const void* pMessageData, const void* pIdTable);
#endif

    bool getMessageDirect(TalkMessageInfo*, const char*) const;
    bool getMessage(TalkMessageInfo*, u16, u16) const;
    TalkNode* findNode(const char*) const;
    TalkNode* getNode(u32) const;
    TalkNode* getBranchNode(u32) const;
    bool isValidBranchNode(u32) const;
    u8* getMessageInfoTool(int) const;
    s32 findMessageIndex(const char*) const;

    JMapInfo* mIDTable;            // 0x0
    MessageInfoBlock* mInfoBlock;  // 0x4
    MessageDataBlock* mDataBlock;  // 0x8
    u32 _C;
    MessageFlowBlock* mFlowBlock;  // 0x10
    u16* _14;
    u8* _18;
    MessageFLI1Block* mFLI1Block;  // 0x1C
#ifdef PETARI_NATIVE
    // Host-order copies built at load time; the resource stays untouched.
    // mText holds the DAT1 payload as UTF-16 code units, so INF1 byte offsets index it.
    u16* mText;
    u32 mTextUnitCount;
    TalkNode* mNodes;
    u16* mBranchNodeIndices;
#endif
};

class MessageHolder {
public:
    MessageHolder();

    void initSceneData();
    void destroySceneData();
    void initSystemData();
    void initGameData();

    /* 0x00 */ MessageData* mSystemMessageData;
    /* 0x04 */ MessageData* mGameMessageData;
    /* 0x08 */ MessageData* mSceneMessageData;
};

class MessageSystem {
public:
    class Node {};

    struct FlowNodeBranch {};

    struct FlowNodeEvent {
        /* 0x00 */ u8 mFlowType;
        /* 0x01 */ u8 mEventType;
        /* 0x02 */ u16 mBranchID;
        /* 0x04 */ u32 mArg;
    };

    static bool getSystemMessageDirect(TalkMessageInfo*, const char*);
    static bool getGameMessageDirect(TalkMessageInfo*, const char*);
    static bool getLayoutMessageDirect(TalkMessageInfo*, const char*);
    static MessageData* getSceneMessageData();
};
