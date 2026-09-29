// BMG message tests for the native build.
//
// Links: src/Game/System/MessageHolder.cpp, src/Game/Screen/MessageEditorMessageTagNative.cpp,
// src/Game/NPC/TalkMessageInfo.cpp, src/Game/Util/JMapInfo.cpp, src/Game/Util/StringUtil.cpp,
// src/Game/Screen/ReplaceTagProcessor.cpp, src/JSystem/JGadget/hashcode.cpp, petari_resources,
// petari_platform_os (OSPanic).
// MessageHolder.cpp also holds the archive-mounting entry points, which need the game
// system; this file defines those as aborting stubs because the tests never reach them.
//
// Usage: petari_message_tests                 synthetic big-endian BMG + MessageId.tbl
//        petari_message_tests --assets FILES  also decode Message.arc for each language on disc
#include "archive.hpp"
#include "Game/NPC/TalkMessageInfo.hpp"
#include "Game/NPC/TalkNodeCtrl.hpp"
#include "Game/Screen/MessageTagSkipTagProcessor.hpp"
#include "Game/Screen/ReplaceTagProcessor.hpp"
#include "Game/System/MessageHolder.hpp"
#include "Game/Util/JMapInfo.hpp"
#include "Game/Util/StringUtil.hpp"
#include <JSystem/JGadget/hashcode.hpp>
#include <petari/endian.hpp>
#include <strings.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

class GameSystemObjHolder;
class JKRArchive;
class JKRHeap;
class JKRMemArchive;

namespace MR {
    [[noreturn]] static void unreachable(const char* pName) {
        std::fprintf(stderr, "message_tests: unexpected call to %s\n", pName);
        std::abort();
    }

    GameSystemObjHolder* getGameSystemObjHolder() {
        unreachable("MR::getGameSystemObjHolder");
    }

    void getMountedArchiveAndHeap(const char*, JKRArchive**, JKRHeap**) {
        unreachable("MR::getMountedArchiveAndHeap");
    }

    JKRMemArchive* mountArchive(const char*, JKRHeap*) {
        unreachable("MR::mountArchive");
    }

    // StringUtil / ReplaceTagProcessor dependencies (player, race and message lookups are not
    // reached by these tests; the picture tag test runs as Mario).
    bool isPlayerLuigi() {
        return false;
    }

    u32 getRaceBestTime(int) {
        unreachable("MR::getRaceBestTime");
    }

    u32 getRaceCurrentTime() {
        unreachable("MR::getRaceCurrentTime");
    }

    const char16_t* getGameMessageDirect(const char*) {
        unreachable("MR::getGameMessageDirect");
    }

    void copyMemory(void* pDst, const void* pSrc, u32 size) {
        std::memcpy(pDst, pSrc, size);
    }
}  // namespace MR

using Buffer = std::vector< std::uint8_t >;

static int sFailures = 0;
static void check(bool condition, const char* text) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", text);
        ++sFailures;
    }
}

static void put32(Buffer& data, std::size_t offset, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        data[offset + i] = static_cast< std::uint8_t >(value >> (24 - i * 8));
    }
}

static void put16(Buffer& data, std::size_t offset, std::uint16_t value) {
    data[offset] = static_cast< std::uint8_t >(value >> 8);
    data[offset + 1] = static_cast< std::uint8_t >(value);
}

static void append16(Buffer& data, std::uint16_t value) {
    data.push_back(static_cast< std::uint8_t >(value >> 8));
    data.push_back(static_cast< std::uint8_t >(value));
}

static void appendText(Buffer& data, const char16_t* pText) {
    for (; *pText != 0; pText++) {
        append16(data, *pText);
    }
}

// Appends a serialized tag: 0x1A, u8 size, u8 group, u16 tag, parameter bytes.
static void appendTag(Buffer& data, std::uint8_t group, std::uint16_t tag, const Buffer& params) {
    append16(data, 0x1A);
    data.push_back(static_cast< std::uint8_t >(6 + params.size()));
    data.push_back(group);
    append16(data, tag);
    data.insert(data.end(), params.begin(), params.end());
}

static void padTo(Buffer& data, std::size_t alignment) {
    while (data.size() % alignment != 0) {
        data.push_back(0);
    }
}

namespace {
    struct Synthetic {
        Buffer mBmg;
        Buffer mIdTable;
        std::uint32_t mTextOffsets[3];
    };
}  // namespace

// MessageId.tbl: BCSV with "MessageId" (STRING_PTR) and "Index" (LONG), sorted by id.
static Buffer buildIdTable() {
    const char* cIds[] = {"Alpha", "Bravo", "Charlie"};
    const std::int32_t cIndices[] = {2, 0, 1};
    Buffer data(0x10 + 2 * 0xC + 3 * 8);
    put32(data, 0x0, 3);
    put32(data, 0x4, 2);
    put32(data, 0x8, 0x10 + 2 * 0xC);
    put32(data, 0xC, 8);
    put32(data, 0x10, JGadget::getHashCode("MessageId"));
    put32(data, 0x14, 0xFFFFFFFF);
    put16(data, 0x18, 0);
    data[0x1B] = JMAP_VALUE_TYPE_STRING_PTR;
    put32(data, 0x1C, JGadget::getHashCode("Index"));
    put32(data, 0x20, 0xFFFFFFFF);
    put16(data, 0x24, 4);
    data[0x27] = JMAP_VALUE_TYPE_LONG;

    std::string strings;
    for (int i = 0; i < 3; i++) {
        put32(data, 0x28 + i * 8, static_cast< std::uint32_t >(strings.size()));
        put32(data, 0x2C + i * 8, static_cast< std::uint32_t >(cIndices[i]));
        strings += cIds[i];
        strings += '\0';
    }
    data.insert(data.end(), strings.begin(), strings.end());
    return data;
}

static Synthetic buildSynthetic() {
    Synthetic out;

    // DAT1 payload: a leading NUL (offset 0 is the empty message), then three messages.
    Buffer text;
    append16(text, 0);

    out.mTextOffsets[0] = static_cast< std::uint32_t >(text.size());
    appendText(text, u"Hi é");
    appendTag(text, 255, 0, {0x00, 0x02});                              // color 2
    appendTag(text, 6, 1, {0, 0, 0, 0, 0, 0, 0, 1});                    // number arg #1
    appendTag(text, 7, 0, {0, 0, 0, 0, 0, 0, 0, 3});                    // string arg #3
    appendTag(text, 2, 0, {0x00, 'S', 0x00, 'E', 0x00, '_', 0x00, 'A'});  // sound "SE_A"
    appendTag(text, 1, 0, {0x12, 0x34});                                // wait 0x1234
    appendText(text, u"!");
    append16(text, 0);

    out.mTextOffsets[1] = static_cast< std::uint32_t >(text.size());
    appendText(text, u"Second");
    append16(text, 0);

    out.mTextOffsets[2] = static_cast< std::uint32_t >(text.size());
    appendText(text, u"Third あ");
    append16(text, 0);

    Buffer& bmg = out.mBmg;
    bmg.assign(0x20, 0);
    std::memcpy(bmg.data(), "MESGbmg1", 8);
    put32(bmg, 0xC, 4);
    bmg[0x10] = 2;

    // INF1: 3 items of 12 bytes: u32 text offset, u16 camera set, u8 _6, camera type,
    // talk type, balloon type, _A, _B.
    const std::size_t inf = bmg.size();
    bmg.resize(inf + 0x10 + 3 * 12);
    std::memcpy(&bmg[inf], "INF1", 4);
    put16(bmg, inf + 8, 3);
    put16(bmg, inf + 10, 12);
    for (int i = 0; i < 3; i++) {
        const std::size_t item = inf + 0x10 + i * 12;
        put32(bmg, item, out.mTextOffsets[i]);
        put16(bmg, item + 4, static_cast< std::uint16_t >(0x0100 + i));
        bmg[item + 6] = static_cast< std::uint8_t >(0xF0 + i);
        bmg[item + 7] = 1;
        bmg[item + 8] = static_cast< std::uint8_t >(i);
        bmg[item + 9] = 3;
        bmg[item + 10] = 0x7F;
        bmg[item + 11] = 0x80;
    }
    padTo(bmg, 0x20);
    put32(bmg, inf + 4, static_cast< std::uint32_t >(bmg.size() - inf));

    const std::size_t dat = bmg.size();
    bmg.resize(dat + 8);
    std::memcpy(&bmg[dat], "DAT1", 4);
    bmg.insert(bmg.end(), text.begin(), text.end());
    padTo(bmg, 0x20);
    put32(bmg, dat + 4, static_cast< std::uint32_t >(bmg.size() - dat));

    // FLW1: message node -> branch node -> event node, plus a two-entry branch table.
    const std::size_t flw = bmg.size();
    bmg.resize(flw + 0x10 + 3 * 8 + 2 * 2 + 2);
    std::memcpy(&bmg[flw], "FLW1", 4);
    put16(bmg, flw + 8, 3);
    put16(bmg, flw + 10, 2);
    const std::size_t nodes = flw + 0x10;
    bmg[nodes + 0] = 1;
    bmg[nodes + 1] = 0;
    put16(bmg, nodes + 2, 2);  // message index 2
    put16(bmg, nodes + 4, 1);  // next node
    put16(bmg, nodes + 6, 0x0203);
    bmg[nodes + 8] = 2;
    bmg[nodes + 9] = 1;
    put16(bmg, nodes + 10, 3);  // branch condition
    put16(bmg, nodes + 12, 0x0405);
    put16(bmg, nodes + 14, 0x0607);
    bmg[nodes + 16] = 3;
    bmg[nodes + 17] = 5;
    put16(bmg, nodes + 18, 0);
    put32(bmg, nodes + 20, 0x12345678);
    put16(bmg, nodes + 24, 2);
    put16(bmg, nodes + 26, 0xFFFF);
    bmg[nodes + 28] = 0xAB;
    padTo(bmg, 0x20);
    put32(bmg, flw + 4, static_cast< std::uint32_t >(bmg.size() - flw));

    const std::size_t fli = bmg.size();
    bmg.resize(fli + 0x10);
    std::memcpy(&bmg[fli], "FLI1", 4);
    put32(bmg, fli + 4, 0x10);

    put32(bmg, 0x8, static_cast< std::uint32_t >(bmg.size()));
    out.mIdTable = buildIdTable();
    return out;
}

static bool equalsText(const u8* pText, const char16_t* pExpected, std::size_t count) {
    const char16_t* pUnits = reinterpret_cast< const char16_t* >(pText);
    return std::memcmp(pUnits, pExpected, count * sizeof(char16_t)) == 0;
}

static void testSynthetic() {
    const Synthetic synthetic = buildSynthetic();
    const Buffer bmgBefore = synthetic.mBmg;
    MessageData data(synthetic.mBmg.data(), synthetic.mIdTable.data());

    check(data.mInfoBlock != nullptr && data.mInfoBlock->mItemCount == 3 && data.mInfoBlock->mItemSize == 12, "INF1 header");
    check(data.mDataBlock != nullptr && data.mFlowBlock != nullptr && data.mFLI1Block != nullptr, "block lookup");
    check(data.findMessageIndex("Alpha") == 2 && data.findMessageIndex("Charlie") == 1, "MessageId.tbl lookup");
    check(data.findMessageIndex("Delta") == -1, "missing message id");

    TalkMessageInfo info;
    check(data.getMessageDirect(&info, "Bravo"), "getMessageDirect");
    const char16_t cFirst[] = u"Hi é";
    check(equalsText(info._0, cFirst, 4), "DAT1 text decoded to host-order UTF-16");
    check(info.mCameraSetID == 0x0100 && info._6 == static_cast< s8 >(0xF0) && info.mCameraType == 1 && info.mTalkType == 0 &&
              info.mBalloonType == 3 && info._A == 0x7F && info._B == static_cast< s8 >(0x80),
          "INF1 record fields");
    check(!data.getMessageDirect(&info, "Delta"), "getMessageDirect miss");

    check(data.getMessageDirect(&info, "Alpha"), "getMessageDirect third");
    const char16_t cThird[] = u"Third あ";
    check(equalsText(info._0, cThird, 8) && info.mTalkType == 2, "third message");

    // Walk the tags of the first message through the game accessor.
    data.getMessageDirect(&info, "Bravo");
    const char16_t* pText = reinterpret_cast< const char16_t* >(info._0) + 4;
    check(*pText == 0x1A, "color tag start");
    MessageEditorMessageTag color(pText + 1);
    check(color.getGroup() == 255 && color.getTag() == 0 && color.getTagLength() == 6 && color.getSkipLength() == 3 &&
              color.getParamLength() == 2 && color.getParam8(0) == 0 && color.getParam8(1) == 2 && color.getParam16(0) == 2,
          "color tag fields");

    pText += 1 + color.getSkipLength();
    MessageEditorMessageTag number(pText + 1);
    check(*pText == 0x1A && number.getGroup() == 6 && number.getTag() == 1 && number.getTagLength() == 12 &&
              number.getParam32(0) == 0 && number.getParam32(1) == 1 && number.isGroupTagId(6, 1) && !number.isGroupTagId(7, 1),
          "number argument tag fields");

    pText += 1 + number.getSkipLength();
    MessageEditorMessageTag string(pText + 1);
    check(string.getGroup() == 7 && string.getParam32(1) == 3 && string.getArgString() == nullptr, "string argument tag fields");

    pText += 1 + string.getSkipLength();
    MessageEditorMessageTag sound(pText + 1);
    const char16_t cSound[] = u"SE_A";
    check(sound.getGroup() == 2 && sound.getParamLength() == 8 &&
              std::memcmp(sound.getParamPtr(0), cSound, 4 * sizeof(char16_t)) == 0,
          "UTF-16 string parameter");

    pText += 1 + sound.getSkipLength();
    MessageEditorMessageTag wait(pText + 1);
    check(wait.getGroup() == 1 && wait.getParam16(0) == 0x1234, "wait tag parameter");
    pText += 1 + wait.getSkipLength();
    check(pText[0] == u'!' && pText[1] == 0, "text after tags");

    // Replacement arguments are written into a text buffer, never into the resource.
    const char16_t* pMessage = reinterpret_cast< const char16_t* >(info._0);
    // Tag parameters may contain zero code units, so skip tags when measuring the message.
    std::size_t messageUnits = 0;
    while (pMessage[messageUnits] != 0) {
        messageUnits += pMessage[messageUnits] == 0x1A ? 1 + MessageEditorMessageTag(pMessage + messageUnits + 1).getSkipLength() : 1;
    }
    std::vector< char16_t > buffer(pMessage, pMessage + messageUnits + 1);
    MessageEditorMessageTag bufferNumber(&buffer[4 + 1 + 3 + 1]);
    bufferNumber.setParam32(0, static_cast< u32 >(-1234));
    check(static_cast< s32 >(bufferNumber.getParam32(0)) == -1234 && bufferNumber.getParam32(1) == 1, "setParam32 keeps the index");

    MessageEditorMessageTag bufferString(&buffer[4 + 1 + 3 + 1 + 6 + 1]);
    const char16_t cNameA[] = u"Mario";
    const char16_t cNameB[] = u"Luigi";
    bufferString.setArgString(reinterpret_cast< const char16_t* >(cNameA));
    check(bufferString.getArgString() == reinterpret_cast< const char16_t* >(cNameA) && bufferString.getParam32(1) == 3,
          "string argument handle");
    const u32 handleA = bufferString.getParam32(0);
    bufferString.setArgString(reinterpret_cast< const char16_t* >(cNameB));
    bufferString.setArgString(reinterpret_cast< const char16_t* >(cNameA));
    check(bufferString.getParam32(0) == handleA, "string argument handles are reused");
    bufferString.setArgString(nullptr);
    check(bufferString.getArgString() == nullptr, "null string argument");
    static char16_t replacements[1024][2]{};
    u32 handles[1024];
    for (unsigned i = 0; i < 1024; ++i) {
        replacements[i][0] = static_cast<char16_t>(u'A' + i % 26);
        bufferString.setArgString(replacements[i]);
        handles[i] = bufferString.getParam32(0);
    }
    for (unsigned i = 0; i < 1024; ++i) {
        bufferString.setParam32(0, handles[i]);
        check(bufferString.getArgString() == replacements[i], "string handles survive registry growth");
        bufferString.setArgString(replacements[i]);
        check(bufferString.getParam32(0) == handles[i], "grown registry deduplicates pointers");
    }

    // Flow nodes, decoded per node type.
    TalkNode* pMessageNode = data.findNode("Alpha");
    check(pMessageNode == data.getNode(0) && pMessageNode->mNodeType == 1 && pMessageNode->mIndex == 2 && pMessageNode->mNextIdx == 1 &&
              pMessageNode->mNextGroup == 0x0203,
          "message node");
    TalkNode* pBranch = data.getNode(1);
    check(pBranch->mNodeType == 2 && pBranch->mGroupID == 1 && pBranch->mIndex == 3 && pBranch->mNextIdx == 0x0405 &&
              pBranch->mNextGroup == 0x0607,
          "branch node");
    TalkNode* pEvent = data.getNode(2);
    check(pEvent->mNodeType == 3 && pEvent->mGroupID == 5 && pEvent->mUnknown == 0x12345678, "event node u32 argument");
    check(data.findNode("Bravo") == nullptr, "findNode miss");
    check(data.isValidBranchNode(0) && data.getBranchNode(0) == pEvent && !data.isValidBranchNode(1), "branch table");
    check(data._18 != nullptr && *data._18 == 0xAB, "byte table after branch table");

    check(synthetic.mBmg == bmgBefore, "resource bytes left unmodified");
}

// Message tag helpers on host-order text: the walker, the next-page test and the replace
// functions (picture tag rewrite, number and string arguments).
static void testTagFunctions() {
    const char16_t cNumberTag[] = {u'A', 0x1A, 14 << 8 | 6, 0, 0, 0, 0, 1, u'B', 0};
    const char16_t cStringTag[] = {u'<', 0x1A, 14 << 8 | 7, 0, 0, 0, 0, 0, u'>', 0};
    const char16_t cPageBreak[] = {u'x', 0x1A, 6 << 8 | 1, 1, u'y', 0};
    const char16_t cPictureTags[] = {0x1A, 6 << 8 | 3, 0x05, 0x1A, 6 << 8 | 3, 0x5B - 0x30, u'!', 0};

    check(MR::getStringLengthWithMessageTag(cNumberTag) == 9, "tag walker counts a tag as its size in units");
    check(MR::getStringLengthWithMessageTag(cPageBreak) == 1 && MR::isMessageEditorNextTag(&cPageBreak[1]) &&
              !MR::isMessageEditorNextTag(&cNumberTag[1]),
          "next-page tag (group 1, tag 1) ends the walk");

    char16_t buffer[64] = {};
    ReplaceTagFunction::ReplaceArgs(buffer, 64, cNumberTag, 11, 42);
    check(std::u16string(buffer) == u"A42B", "number argument tag takes the argument its parameter names (va_copy)");
    ReplaceTagFunction::ReplaceArgs(buffer, 64, cStringTag, u"xyz");
    check(std::u16string(buffer) == u"<xyz>", "string argument tag");

    // A picture tag is rewritten in place; 0x5B - 0x30 is the player icon, which becomes 0x12 for Mario.
    const u32 length = ReplaceTagProcessor::Replace(buffer, cPictureTags);
    const char16_t cExpected[] = {0x1A, 6 << 8 | 3, 0x05, 0x1A, 6 << 8 | 3, 0x12, u'!', 0};
    check(length == 7 && std::memcmp(buffer, cExpected, sizeof(cExpected)) == 0 && MR::getStringLengthWithMessageTag(buffer) == 7,
          "picture tags are rewritten as host-order code units");
}

static bool readFile(const std::filesystem::path& path, Buffer* pOut) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    pOut->assign(std::istreambuf_iterator< char >(file), std::istreambuf_iterator< char >());
    return true;
}

// Decodes one Message.arc and checks every message, tag and flow node stays in bounds.
static void smokeMessageArchive(const std::filesystem::path& path) {
    Buffer bytes;
    if (!readFile(path, &bytes)) {
        std::fprintf(stderr, "FAIL: cannot read %s\n", path.c_str());
        ++sFailures;
        return;
    }

    const auto archive = PetariNative::Resource::Archive::parse({bytes.data(), bytes.size()});
    Buffer bmg;
    Buffer table;
    for (std::size_t i = 0; i < archive.entries().size(); i++) {
        const auto& entry = archive.entries()[i];
        if (strcasecmp(entry.name.c_str(), "Message.bmg") == 0) {
            bmg = archive.resourceData(i);
        } else if (strcasecmp(entry.name.c_str(), "MessageId.tbl") == 0) {
            table = archive.resourceData(i);
        }
    }
    if (bmg.size() < 0x20 || std::memcmp(bmg.data(), "MESGbmg1", 8) != 0 || table.empty()) {
        std::fprintf(stderr, "FAIL: %s has no Message.bmg/MessageId.tbl\n", path.c_str());
        ++sFailures;
        return;
    }

    MessageData data(bmg.data(), table.data());
    const JMapInfo* pIds = data.mIDTable;
    const u32 itemCount = data.mInfoBlock->mItemCount;
    std::size_t messages = 0, tags = 0, units = 0, asciiUnits = 0, argTags = 0, nextPageTags = 0;
    int failures = 0;

    for (int entry = 0; entry < pIds->getNumEntries() && failures < 5; entry++) {
        const char* pId = nullptr;
        pIds->getValue< const char* >(entry, "MessageId", &pId);
        TalkMessageInfo info;
        const s32 index = data.findMessageIndex(pId);
        if (index < 0 || static_cast< u32 >(index) >= itemCount || !data.getMessageDirect(&info, pId)) {
            std::fprintf(stderr, "FAIL: %s: message '%s' did not resolve\n", path.filename().c_str(), pId);
            ++failures;
            continue;
        }

        const char16_t* pBegin = reinterpret_cast< const char16_t* >(data.mText);
        const char16_t* pEnd = pBegin + data.mTextUnitCount;
        const char16_t* pText = reinterpret_cast< const char16_t* >(info._0);

        // The game's tag walker (MR::getStringLengthWithMessageTag, which the text box setup
        // uses) against a reference on the serialized big-endian bytes of the same message:
        // a tag is u16 0x1A, u8 size (whole tag), u8 group, u16 tag; group 1 tag 1 ends a page.
        const u8* pRaw = reinterpret_cast< const u8* >(data.mDataBlock + 1) + (pText - pBegin) * 2;
        const u8* pRawEnd = reinterpret_cast< const u8* >(data.mDataBlock + 1) + data.mTextUnitCount * 2;
        int expectedLength = 0;
        for (const u8* p = pRaw; p + 1 < pRawEnd && PetariNative::readU16BE(p) != 0;) {
            if (PetariNative::readU16BE(p) == 0x1A) {
                if (p[3] == 1 && PetariNative::readU16BE(p + 4) == 1) {
                    nextPageTags++;
                    break;
                }
                expectedLength += p[2] / 2;
                p += p[2];
            } else {
                expectedLength++;
                p += 2;
            }
        }
        if (MR::getStringLengthWithMessageTag(pText) != expectedLength) {
            std::fprintf(stderr, "FAIL: %s: '%s' tag walker length %d, expected %d\n", path.filename().c_str(), pId,
                         MR::getStringLengthWithMessageTag(pText), expectedLength);
            ++failures;
        }
        while (pText < pEnd && *pText != 0) {
            if (*pText == 0x1A) {
                MessageEditorMessageTag tag(pText + 1);
                const u32 size = tag.getTagLength() + 2;
                if (size < 6 || (size & 1) != 0 || pText + size / 2 > pEnd) {
                    std::fprintf(stderr, "FAIL: %s: bad tag in '%s'\n", path.filename().c_str(), pId);
                    ++failures;
                    break;
                }
                if (tag.getGroup() == 6 || tag.getGroup() == 7) {
                    argTags += tag.getParamLength() == 8 && tag.getParam32(1) < 16;
                }
                pText += 1 + tag.getSkipLength();
                tags++;
            } else {
                asciiUnits += *pText >= 0x20 && *pText < 0x7F;
                pText++;
                units++;
            }
        }
        if (pText >= pEnd) {
            std::fprintf(stderr, "FAIL: %s: '%s' is not terminated\n", path.filename().c_str(), pId);
            ++failures;
        }
        messages++;
    }

    for (u32 i = 0; data.mFlowBlock != nullptr && i < data.mFlowBlock->mNodeCount; i++) {
        const TalkNode* pNode = data.getNode(i);
        const bool validType = pNode->mNodeType >= 1 && pNode->mNodeType <= 4;
        const bool validMessage = pNode->mNodeType != 1 || pNode->mIndex < itemCount;
        if (!validType || !validMessage) {
            std::fprintf(stderr, "FAIL: %s: flow node %u type %u index %u\n", path.filename().c_str(), i, pNode->mNodeType, pNode->mIndex);
            ++failures;
            break;
        }
    }
    for (u32 i = 0; data.mFlowBlock != nullptr && i < data.mFlowBlock->_A; i++) {
        if (data.isValidBranchNode(i) && data.mBranchNodeIndices[i] >= data.mFlowBlock->mNodeCount) {
            std::fprintf(stderr, "FAIL: %s: branch %u points past the node table\n", path.filename().c_str(), i);
            ++failures;
            break;
        }
    }

    // Host-order decoding of Latin-script text yields mostly printable ASCII code units.
    if (units == 0 || asciiUnits * 2 < units) {
        std::fprintf(stderr, "FAIL: %s: only %zu of %zu code units are printable ASCII\n", path.filename().c_str(), asciiUnits, units);
        ++failures;
    }

    if (path.string().find("UsEnglish") != std::string::npos) {
        // Spot check of decoded content in the RMGE01 English data.
        TalkMessageInfo info;
        const char16_t cExpected[] = u"Mario!";
        if (!data.getMessageDirect(&info, "Layout_StoryDemoPeachTalk000") || !equalsText(info._0, cExpected, 7)) {
            std::fprintf(stderr, "FAIL: %s: Layout_StoryDemoPeachTalk000 is not \"Mario!\"\n", path.filename().c_str());
            ++failures;
        }
    }

    sFailures += failures;
    if (nextPageTags == 0) {
        std::fprintf(stderr, "FAIL: %s: no next-page tag found by the reference walker\n", path.filename().c_str());
        ++failures;
    }
    std::printf("%s: %zu messages, %zu tags (%zu argument tags, %zu page breaks), %zu text units, %u flow nodes\n",
                path.parent_path().parent_path().filename().c_str(), messages, tags, argTags, nextPageTags, units,
                data.mFlowBlock != nullptr ? static_cast< u32 >(data.mFlowBlock->mNodeCount) : 0);
}

static void testAssets(const std::filesystem::path& filesRoot) {
    namespace fs = std::filesystem;
    std::size_t archives = 0;
    for (const char* pLanguage : {"UsEnglish", "UsFrench", "UsSpanish", "JpJapanese", "EuEnglish", "KrKorean"}) {
        const fs::path path = filesRoot / pLanguage / "MessageData" / "Message.arc";
        if (fs::exists(path)) {
            try {
                smokeMessageArchive(path);
            } catch (const std::exception& error) {
                std::fprintf(stderr, "FAIL: %s: %s\n", path.c_str(), error.what());
                ++sFailures;
            }
            archives++;
        }
    }
    check(archives > 0, "no Message.arc found under the asset directory");
}

int main(int argc, char** argv) {
    testTagFunctions();
    testSynthetic();

    if (argc == 3 && std::strcmp(argv[1], "--assets") == 0) {
        testAssets(argv[2]);
    } else if (argc != 1) {
        std::fprintf(stderr, "Usage: %s [--assets GAME_FILES_DIR]\n", argv[0]);
        return 2;
    }

    if (sFailures != 0) {
        std::fprintf(stderr, "%d message check(s) failed\n", sFailures);
        return 1;
    }
    std::puts("Message tests passed");
    return 0;
}
