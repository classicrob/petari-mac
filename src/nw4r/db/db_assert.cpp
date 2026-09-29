#include "nw4r/db/assert.h"
#include "nw4r/db/console.h"
#include "nw4r/db/directPrint.h"
#include "nw4r/db/mapFile.h"
#include <va_list.h>

#ifdef PETARI_NATIVE
#include <dlfcn.h>
#include <execinfo.h>
#define NW4R_DB_WEAK __attribute__((weak))
#else
#define NW4R_DB_WEAK __declspec(weak)
#endif

namespace nw4r {
    namespace db {
        static ConsoleHandle sAssertionConsole = NULL;

        static void Assertion_Printf_(const char* fmt, ...) {
            va_list vlist;
            va_start(vlist, fmt);
            if (sAssertionConsole) {
            } else {
                OSVReport(fmt, vlist);
            }
            va_end(vlist);
        }

#ifdef PETARI_NATIVE
        // Host equivalent of the PowerPC back-chain walk: print return addresses
        // and the nearest exported symbol from the dynamic loader.
        static void ShowStack_() {
            Assertion_Printf_("-------------------------------- TRACE\n");
            Assertion_Printf_("Address:            Symbol\n");

            void* frames[17];
            int count = backtrace(frames, 17);
            for (int i = 1; i < count; i++) {
                Dl_info info;
                if (dladdr(frames[i], &info) && info.dli_sname != NULL) {
                    Assertion_Printf_("%p  %s+0x%lx\n", frames[i], info.dli_sname,
                                      static_cast< unsigned long >(static_cast< char* >(frames[i]) - static_cast< char* >(info.dli_saddr)));
                } else {
                    Assertion_Printf_("%p\n", frames[i]);
                }
            }
        }
#else
        static bool ShowMapInfoSubroutine_(u32 address, bool preCRFlag) {
            if (address < 0x80000000 || 0x82ffffff < address) {
                return false;
            }

            {
                enum { STR_LEN = 260 };
                u8 strBuf[STR_LEN];
                bool result;

                result = MapFile_QuerySymbol(address, strBuf, STR_LEN);
                if (result) {
                    if (preCRFlag) {
                        Assertion_Printf_("\n");
                    }
                    Assertion_Printf_("%s\n", strBuf);
                    return true;
                } else {
                    return false;
                }
            }
        }

        static void ShowStack_(u32 sp) {
            Assertion_Printf_("-------------------------------- TRACE\n");
            u32 i;
            u32* p;

            Assertion_Printf_("Address:   BackChain   LR save\n");
            p = (u32*)sp;

            for (i = 0; i < 16; i++) {
                if (p == NULL || (u32)p == 0xffffffff || ((u32)p & 0x80000000) == 0) {
                    break;
                }
                Assertion_Printf_("%08X:  %08X    %08X ", p, p[0], p[1]);
                if (!ShowMapInfoSubroutine_(p[1], false)) {
                    Assertion_Printf_("\n");
                }

                p = (u32*)*p;
            }

            return;
        }
#endif

        NW4R_DB_WEAK void VPanic(const char* file, int line, const char* fmt, va_list vlist, bool halt) {
#ifndef PETARI_NATIVE
            register u32 stackPointer;
            asm {
        mr  stackPointer, r1
            }
            stackPointer = *((u32*)stackPointer);
#endif
            (void)OSDisableInterrupts();
            (void)OSDisableScheduler();

            (void)VISetPreRetraceCallback(NULL);
            (void)VISetPostRetraceCallback(NULL);

            if (sAssertionConsole) {
                (void)detail::DirectPrint_SetupFB(NULL);
            }

#ifdef PETARI_NATIVE
            ShowStack_();
#else
            ShowStack_(stackPointer);
#endif

            if (sAssertionConsole) {
                Console_Printf(sAssertionConsole, "%s:%d Panic:", file, line);
                Console_Printf(sAssertionConsole, "\n");

                (void)Console_ShowLatestLine(sAssertionConsole);
                (void)Console_SetVisible(sAssertionConsole, true);
                Console_DrawDirect(sAssertionConsole);
            } else {
                OSReport("%s:%d Panic:", file, line);
                OSVReport(fmt, vlist);
                OSReport("\n");
            }

            if (halt) {
                PPCHalt();
            }
        }

        NW4R_DB_WEAK void Panic(const char* file, int line, const char* fmt, ...) {
            va_list vlist;
            va_start(vlist, fmt);
            nw4r::db::VPanic(file, line, fmt, vlist, true);
            va_end(vlist);
            PPCHalt();
        }

    };  // namespace db
};  // namespace nw4r
