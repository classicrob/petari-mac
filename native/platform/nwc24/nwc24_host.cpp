// WiiConnect24 on the host: unavailable. Replaces src/RVL_SDK/nwc24/*.c.
//
// WiiConnect24 (Wii Message Board mail, sent by the game when players share
// pictures or letters) needs Nintendo's discontinued service and the console's
// NAND mail box; natively it behaves like a console with WiiConnect24 turned
// off in System Settings: NWC24OpenLib fails with NWC24_ERR_DISABLED, which
// the game handles (NWC24Messenger shows its WiiConnect24 notice for
// foreground sends and drops background sends). Nothing reports success.
//
// Only the functions the game calls are provided.

#include <revolution/nwc24.h>
#include <revolution/os.h>

namespace {

bool gOpenAttempted;

// Every message operation requires an open library, which never happens here.
NWC24Err notOpen() {
    return NWC24_ERR_NOT_READY;
}

}  // namespace

extern "C" {

NWC24Err NWC24OpenLib(void* work) {
    if (work == nullptr) {
        return NWC24_ERR_INVALID_VALUE;
    }
    if (!gOpenAttempted) {
        gOpenAttempted = true;
        OSReport("NWC24: WiiConnect24 is not available on this host (disabled)\n");
    }
    return NWC24_ERR_DISABLED;
}

NWC24Err NWC24CloseLib(void) {
    return notOpen();
}

// Error code of the last WiiConnect24 daemon failure, shown to the user by
// the game. The daemon never ran, so there is none.
s32 NWC24GetErrorCode(void) {
    return 0;
}

NWC24Err NWC24InitMsgObj(NWC24MsgObj*, NWC24MsgType) { return notOpen(); }
NWC24Err NWC24SetMsgToId(NWC24MsgObj*, NWC24UserId) { return notOpen(); }
NWC24Err NWC24SetMsgText(NWC24MsgObj*, const char*, u32, NWC24Charset, NWC24Encoding) { return notOpen(); }
NWC24Err NWC24SetMsgAttached(NWC24MsgObj*, const char*, u32, NWC24MIMEType) { return notOpen(); }
NWC24Err NWC24SetMsgTag(NWC24MsgObj*, u16) { return notOpen(); }
NWC24Err NWC24SetMsgLedPattern(NWC24MsgObj*, u16) { return notOpen(); }
NWC24Err NWC24SetMsgMBDelay(NWC24MsgObj*, u8) { return notOpen(); }
NWC24Err NWC24SetMsgAltName(NWC24MsgObj*, const u16*, u32) { return notOpen(); }
NWC24Err NWC24SetMsgMBNoReply(NWC24MsgObj*, BOOL) { return notOpen(); }
NWC24Err NWC24CommitMsg(NWC24MsgObj*) { return notOpen(); }
NWC24Err NWC24GetMyUserId(NWC24UserId*) { return notOpen(); }

NWC24Err NWC24GetMsgSize(const NWC24MsgObj*, u32* size) {
    if (size) {
        *size = 0;
    }
    return notOpen();
}

}  // extern "C"
