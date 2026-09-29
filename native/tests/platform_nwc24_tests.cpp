// WiiConnect24 is unavailable natively: the game's open path must take its
// non-retrying error branch, and no message operation may report success.

#include <revolution/nwc24.h>
#include <revolution/os.h>

#include <cstdio>
#include <cstdlib>

extern "C" void __OSThreadInit(void);
extern "C" void VFInitEx(void*, u32);
namespace PetariNative::Platform::NWC24 {
void* vfWorkArea(u32* size);
}

namespace {

int checks = 0;
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        std::exit(1);
    }
}

// NWC24MessengerSub::SendState::isRestorableError.
bool isRestorableError(NWC24Err err) {
    return err == NWC24_ERR_MUTEX || err == NWC24_ERR_BUSY || err == NWC24_ERR_INPROGRESS;
}

}  // namespace

int main() {
    __OSThreadInit();
    alignas(32) static u8 work[16384];

    // NWC24System's constructor: VF gets its work memory first.
    alignas(32) static u8 vfWork[16 * 1024];
    VFInitEx(vfWork, sizeof(vfWork));
    u32 vfSize = 0;
    check(PetariNative::Platform::NWC24::vfWorkArea(&vfSize) == vfWork && vfSize == sizeof(vfWork), "VFInitEx takes the work area");

    // NWC24System::open.
    const NWC24Err err = NWC24OpenLib(work);
    check(err == NWC24_ERR_DISABLED, "opening WiiConnect24 reports it disabled");
    check(!isRestorableError(err), "the game does not retry: it shows its WiiConnect24 notice");
    check(NWC24GetErrorCode() == 0, "no daemon error code has been recorded");
    check(NWC24OpenLib(work) == NWC24_ERR_DISABLED, "repeated opens fail the same way");
    check(NWC24OpenLib(nullptr) == NWC24_ERR_INVALID_VALUE, "a missing work buffer is invalid");

    // NWC24SendThread::sendMessage would never get here, but nothing succeeds.
    NWC24MsgObj msg;
    NWC24UserId id = 0;
    u32 size = 123;
    check(NWC24InitMsgObj(&msg, NWC24_MSGTYPE_RVL_MENU) == NWC24_ERR_NOT_READY, "message object needs an open library");
    check(NWC24GetMyUserId(&id) == NWC24_ERR_NOT_READY && NWC24SetMsgToId(&msg, id) == NWC24_ERR_NOT_READY, "no user id");
    check(NWC24SetMsgTag(&msg, 1) == NWC24_ERR_NOT_READY && NWC24SetMsgMBNoReply(&msg, TRUE) == NWC24_ERR_NOT_READY, "setters refuse");
    check(NWC24CommitMsg(&msg) == NWC24_ERR_NOT_READY, "commit refuses");
    check(NWC24GetMsgSize(&msg, &size) == NWC24_ERR_NOT_READY && size == 0, "size query reports nothing sent");
    check(NWC24CloseLib() == NWC24_ERR_NOT_READY, "closing a library that never opened");
    OSReport("platform NWC24 tests passed (%d checks)\n", checks);
    return 0;
}
