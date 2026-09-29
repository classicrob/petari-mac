#pragma once

#include "nw4r/ut/ResFontBase.h"

namespace nw4r {
    namespace ut {
        struct BinaryFileHeader;

        class ResFont : public detail::ResFontBase {
        public:
            ResFont();
            virtual ~ResFont();

            bool SetResource(void* pBuffer);
            void RemoveResource();

            static FontInformation* Rebuild(BinaryFileHeader* pHeader);

#ifdef PETARI_NATIVE
        private:
            // Host-layout copy of the font tables; the caller's buffer is not modified.
            void* mNativeFontData;
#endif
        };
    };  // namespace ut
};  // namespace nw4r
