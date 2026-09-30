#pragma once

// Native-only generator for an "everything unlocked" saved file
// (--make-unlocked-save, native/include/petari/unlocked_save.hpp). It changes
// progression only through GameDataHolder/ConfigDataHolder, stores it with
// SaveDataHandler, and checks it after the game's own reload. The Wii build
// does not compile it in.
#ifdef PETARI_NATIVE
class SaveDataHandler;
class SaveDataHandleSequence;
class UserFile;

namespace NativeUnlockedSave {
    bool isRequested();

    // At PreLoadDone after the saved file passed SaveDataHandler's checks. The
    // first time it unlocks and stores the file and returns true: the caller
    // starts the game's save. The second time (after the reload) it checks the
    // file and exits the process.
    bool onLoaded(SaveDataHandleSequence* pSequence, UserFile* pWorkFile, SaveDataHandler* pHandler);

    // True once the save has been written and a reload should start.
    bool isSaveWritten(bool isSequenceIdle);

    // Reports the reason and exits the process with status 1.
    void fail(const char* pReason);
}  // namespace NativeUnlockedSave
#endif
