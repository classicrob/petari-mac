#include "Game/System/MessageHolder.hpp"
#include "Game/NPC/TalkMessageInfo.hpp"
#include "Game/NPC/TalkNodeCtrl.hpp"
#include "Game/System/GameSystemObjHolder.hpp"
#include "Game/Util/FileUtil.hpp"
#include "Game/Util/SystemUtil.hpp"
#include <JSystem/JKernel/JKRArchive.hpp>
#ifdef PETARI_NATIVE
#include <petari/endian.hpp>
#endif

#define SYSTEMMESSAGE_ARC "/Memory/SystemMessage.arc"
#define MESSAGE_ARC "/MessageData/Message.arc"

namespace {
#ifdef PETARI_NATIVE
    u8* getBlock(u32 magic, u8* pData) {
        u32 numBlocks = PetariNative::readU32BE(pData + 0xc);
        pData += 0x20;

        for (u32 i = 0; i < numBlocks; i++) {
            if (PetariNative::readU32BE(pData) == magic) {
                return pData;
            }

            pData += PetariNative::readU32BE(pData + 4);
        }

        return nullptr;
    }

    // FLW1 node: u8 type, u8 group, u16 index, then either u32 argument (event nodes) or
    // u16 next index and u16 next group (message and branch nodes).
    void decodeTalkNode(TalkNode* pNode, const u8* pRaw) {
        pNode->mNodeType = pRaw[0];
        pNode->mGroupID = pRaw[1];
        pNode->mIndex = PetariNative::readU16BE(pRaw + 2);

        if (pNode->mNodeType == 3) {
            pNode->mUnknown = PetariNative::readU32BE(pRaw + 4);
        } else {
            pNode->mNextIdx = PetariNative::readU16BE(pRaw + 4);
            pNode->mNextGroup = PetariNative::readU16BE(pRaw + 6);
        }
    }
#else
    u8* getBlock(u32 magic, u8* pData) {
        u32 numBlocks = *reinterpret_cast< u32* >(pData + 0xc);
        pData += 0x20;

        for (u32 i = 0; i < numBlocks; i++) {
            u32 blockMagic = *reinterpret_cast< u32* >(pData);

            if (blockMagic == magic) {
                return pData;
            }

            u32 blockSize = *reinterpret_cast< u32* >(pData + 4);
            pData += blockSize;
        }

        return nullptr;
    }
#endif
};  // namespace

bool MessageData::getMessageDirect(TalkMessageInfo* pMessageInfo, const char* pMessage) const {
    s32 messageIndex = findMessageIndex(pMessage);

    if (messageIndex >= 0 && messageIndex < mInfoBlock->mItemCount) {
        getMessage(pMessageInfo, 0, messageIndex);

        return true;
    }

    return false;
}

bool MessageData::getMessage(TalkMessageInfo* pMessageInfo, u16, u16 infoToolIndex) const {
    u8* pInfoTool = getMessageInfoTool(infoToolIndex);
#ifdef PETARI_NATIVE
    // INF1 holds the byte offset of the text in DAT1; mText mirrors DAT1 in host order.
    pMessageInfo->_0 = reinterpret_cast< u8* >(mText) + PetariNative::readU32BE(pInfoTool);
    pMessageInfo->mCameraSetID = PetariNative::readU16BE(pInfoTool + 4);
#else
    pMessageInfo->_0 = reinterpret_cast< u8* >(mDataBlock + 1) + *reinterpret_cast< u32* >(pInfoTool);
    pMessageInfo->mCameraSetID = *reinterpret_cast< u16* >(pInfoTool + 4);
#endif
    pMessageInfo->_6 = *(pInfoTool + 6);
    pMessageInfo->mCameraType = *(pInfoTool + 7);
    pMessageInfo->mTalkType = *(pInfoTool + 8);
    pMessageInfo->_A = *(pInfoTool + 0xa);
    pMessageInfo->_B = *(pInfoTool + 0xb);
    pMessageInfo->mBalloonType = *(pInfoTool + 9);

    return true;
}

TalkNode* MessageData::findNode(const char* pMessage) const {
    s32 messageIndex = findMessageIndex(pMessage);

    for (int i = 0; i < mFlowBlock->mNodeCount; i++) {
#ifdef PETARI_NATIVE
        TalkNode* pNode = mNodes + i;
#else
        TalkNode* pNode = reinterpret_cast< TalkNode* >(mFlowBlock + 1) + i;
#endif

        if (pNode->mNodeType == 1 && pNode->mIndex == messageIndex) {
            return pNode;
        }
    }

    return nullptr;
}

#ifdef PETARI_NATIVE
TalkNode* MessageData::getNode(u32 index) const {
    return mNodes + index;
}

TalkNode* MessageData::getBranchNode(u32 index) const {
    return mNodes + mBranchNodeIndices[index];
}

bool MessageData::isValidBranchNode(u32 index) const {
    return mBranchNodeIndices[index] != 0xffff;
}
#else
TalkNode* MessageData::getNode(u32 index) const {
    return reinterpret_cast< TalkNode* >(mFlowBlock + 1) + index;
}

TalkNode* MessageData::getBranchNode(u32 index) const {
    return reinterpret_cast< TalkNode* >(mFlowBlock + 1) + _14[index];
}

bool MessageData::isValidBranchNode(u32 index) const {
    return _14[index] != 0xffff;
}
#endif

u8* MessageData::getMessageInfoTool(int index) const {
    return reinterpret_cast< u8* >(mInfoBlock + 1) + mInfoBlock->mItemSize * index;
}

MessageHolder::MessageHolder() : mSystemMessageData(nullptr), mGameMessageData(nullptr), mSceneMessageData(nullptr) {
}

void MessageHolder::initSceneData() {
    mSceneMessageData = mGameMessageData;
}

void MessageHolder::destroySceneData() {
    mSceneMessageData = nullptr;
}

bool MessageSystem::getSystemMessageDirect(TalkMessageInfo* pMessageInfo, const char* pMessageId) {
    return MR::getGameSystemObjHolder()->mMessageHolder->mSystemMessageData->getMessageDirect(pMessageInfo, pMessageId);
}

bool MessageSystem::getGameMessageDirect(TalkMessageInfo* pMessageInfo, const char* pMessageId) {
    return MR::getGameSystemObjHolder()->mMessageHolder->mGameMessageData->getMessageDirect(pMessageInfo, pMessageId);
}

bool MessageSystem::getLayoutMessageDirect(TalkMessageInfo* pMessageInfo, const char* pMessageId) {
    return MR::getGameSystemObjHolder()->mMessageHolder->mGameMessageData->getMessageDirect(pMessageInfo, pMessageId);
}

MessageData* MessageSystem::getSceneMessageData() {
    return MR::getGameSystemObjHolder()->mMessageHolder->mSceneMessageData;
}

#ifdef PETARI_NATIVE
MessageData::MessageData(const char* pArchiveName)
    : mIDTable(nullptr), mInfoBlock(nullptr), mDataBlock(nullptr), _C(0), mFlowBlock(nullptr), _14(nullptr), _18(nullptr), mFLI1Block(nullptr),
      mText(nullptr), mTextUnitCount(0), mNodes(nullptr), mBranchNodeIndices(nullptr) {
    JKRArchive* pArchive = nullptr;
    JKRHeap* pHeap = nullptr;
    MR::getMountedArchiveAndHeap(pArchiveName, &pArchive, &pHeap);

    initFromResource(pArchive->getResource("Message.bmg"), pArchive->getResource(QUESTIONMARK_MAGIC, "MessageId.tbl"));
}

MessageData::MessageData(const void* pMessageData, const void* pIdTable)
    : mIDTable(nullptr), mInfoBlock(nullptr), mDataBlock(nullptr), _C(0), mFlowBlock(nullptr), _14(nullptr), _18(nullptr), mFLI1Block(nullptr),
      mText(nullptr), mTextUnitCount(0), mNodes(nullptr), mBranchNodeIndices(nullptr) {
    initFromResource(pMessageData, pIdTable);
}

void MessageData::initFromResource(const void* pMessageData, const void* pIdTable) {
    u8* msgData = static_cast< u8* >(const_cast< void* >(pMessageData));

    mIDTable = new JMapInfo();
    mIDTable->attach(pIdTable);

    mInfoBlock = (MessageInfoBlock*)::getBlock('INF1', msgData);
    mDataBlock = (MessageDataBlock*)::getBlock('DAT1', msgData);
    mFlowBlock = (MessageFlowBlock*)::getBlock('FLW1', msgData);
    mFLI1Block = (MessageFLI1Block*)::getBlock('FLI1', msgData);

    if (mDataBlock != nullptr) {
        // DAT1 text is UTF-16BE with embedded 0x1A tags; tag payloads are converted by the
        // same 16-bit swap and decoded by MessageEditorMessageTag.
        const u8* pRaw = reinterpret_cast< const u8* >(mDataBlock + 1);
        mTextUnitCount = (mDataBlock->mBlockSize - sizeof(MessageDataBlock)) / 2;
        mText = new u16[mTextUnitCount + 1];

        for (u32 i = 0; i < mTextUnitCount; i++) {
            mText[i] = PetariNative::readU16BE(pRaw + i * 2);
        }

        mText[mTextUnitCount] = 0;
    }

    if (mFlowBlock != nullptr) {
        const u8* pRawNodes = reinterpret_cast< const u8* >(mFlowBlock + 1);
        const u32 nodeCount = mFlowBlock->mNodeCount;
        const u32 branchCount = mFlowBlock->_A;
        mNodes = new TalkNode[nodeCount];

        for (u32 i = 0; i < nodeCount; i++) {
            decodeTalkNode(&mNodes[i], pRawNodes + i * 8);
        }

        const u8* pRawBranches = pRawNodes + nodeCount * 8;
        mBranchNodeIndices = new u16[branchCount];

        for (u32 i = 0; i < branchCount; i++) {
            mBranchNodeIndices[i] = PetariNative::readU16BE(pRawBranches + i * 2);
        }

        // _14 is the host-order branch table; _18 is a byte table and stays in the resource.
        _14 = mBranchNodeIndices;
        _18 = const_cast< u8* >(pRawBranches + branchCount * 2);
    }
}
#else
MessageData::MessageData(const char* pArchiveName)
    : mIDTable(nullptr), mInfoBlock(nullptr), mDataBlock(nullptr), _C(0), mFlowBlock(nullptr), _14(nullptr), _18(nullptr), mFLI1Block(nullptr) {
    JKRArchive* pArchive = nullptr;
    JKRHeap* pHeap = nullptr;
    MR::getMountedArchiveAndHeap(pArchiveName, &pArchive, &pHeap);

    u8* msgData = (u8*)pArchive->getResource("Message.bmg");
    u8* mapData = (u8*)pArchive->getResource(QUESTIONMARK_MAGIC, "MessageId.tbl");

    mIDTable = new JMapInfo();
    mIDTable->attach(mapData);

    mInfoBlock = (MessageInfoBlock*)::getBlock('INF1', msgData);
    mDataBlock = (MessageDataBlock*)::getBlock('DAT1', msgData);
    mFlowBlock = (MessageFlowBlock*)::getBlock('FLW1', msgData);

    if (mFlowBlock != nullptr) {
        _14 = reinterpret_cast< u16* >(reinterpret_cast< TalkNode* >(mFlowBlock + 1) + mFlowBlock->mNodeCount);
        _18 = reinterpret_cast< u8* >(_14 + mFlowBlock->_A);
    }

    mFLI1Block = (MessageFLI1Block*)::getBlock('FLI1', msgData);
}
#endif

inline JMapInfoIter end(const JMapInfo* pInfo) {
    return JMapInfoIter(pInfo, pInfo->getNumEntries());
}

s32 MessageData::findMessageIndex(const char* pMessage) const {
    JMapInfoIter iter = mIDTable->findElementBinary("MessageId", pMessage);

    // This *should* be mIDTable->end(), but I have had trouble
    // getting the compiler to inline that (see comment in JMapInfo.hpp)
    if (iter == end(mIDTable)) {
        return -1;
    }

    s32 messageIndex = -1;
    iter.getValue("Index", &messageIndex);

    return messageIndex;
}

void MessageHolder::initSystemData() {
    mSystemMessageData = new MessageData(SYSTEMMESSAGE_ARC);
}

void MessageHolder::initGameData() {
    MR::mountArchive(MESSAGE_ARC, nullptr);

    mGameMessageData = new MessageData(MESSAGE_ARC);
}
