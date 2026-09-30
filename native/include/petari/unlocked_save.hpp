#pragma once

#include <string>

namespace PetariNative::UnlockedSave {
// Set by the native CLI (--make-unlocked-save VARIANT) only after validating an
// explicitly marked isolated --user directory that already holds a saved file.
// At the boot-time save load the game's own GameDataHolder API unlocks that
// file, the game's save sequence writes it, and the game's loader re-reads and
// checks it; the process then exits (0 verified, 1 not). See native/SAVES.md.
//   all-missions    Mario: every star except the Grand Finale's (120), all domes
//                   and galaxies open, every mission selectable.
//   complete-luigi  all-missions plus the 120-star ending seen: Luigi playable.
//   grand-finale    120 stars for both Mario and Luigi: Grand Finale Galaxy open.
// Empty: not in use.
inline std::string variant;
inline int slot = 1;
}  // namespace PetariNative::UnlockedSave
