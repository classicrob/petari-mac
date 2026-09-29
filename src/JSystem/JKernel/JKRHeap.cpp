#include "JSystem/JKernel/JKRHeap.hpp"
#include "JSystem/JUtility/JUTException.hpp"
#include <revolution/os/OSBootInfo.h>

JKRHeap* JKRHeap::sSystemHeap;
JKRHeap* JKRHeap::sCurrentHeap;
JKRHeap* JKRHeap::sRootHeap;
JKRErrorHandler JKRHeap::mErrorHandler;

static bool byte_806B70B8;

void* JKRHeap::mCodeStart;
void* JKRHeap::mCodeEnd;
void* JKRHeap::mUserRamStart;
void* JKRHeap::mUserRamEnd;
u32 JKRHeap::mMemorySize;

static bool byte_806B26D8 = true;
uintptr_t ARALT_AramStartAdr = 0x90000000;

JKRHeap::JKRHeap(void* data, u32 size, JKRHeap* parent, bool error) : JKRDisposer(), mChildTree(this), mDisposerList() {
    OSInitMutex(&mMutex);
    mSize = size;
    mStart = (u8*)data;
    mEnd = (u8*)data + size;

    if (parent == nullptr) {
        JKRHeap::sSystemHeap = this;
        JKRHeap::sCurrentHeap = this;
    } else {
        parent->mChildTree.appendChild(&mChildTree);

        if (JKRHeap::sSystemHeap == JKRHeap::sRootHeap) {
            JKRHeap::sSystemHeap = this;
        }

        if (JKRHeap::sCurrentHeap == JKRHeap::sRootHeap) {
            JKRHeap::sCurrentHeap = this;
        }
    }

    mErrorFlag = error;

    if (mErrorFlag == true && mErrorHandler == nullptr) {
        mErrorHandler = JKRDefaultMemoryErrorRoutine;
    }

    _3C = byte_806B26D8;
    _3D = byte_806B70B8;
    _69 = false;
}

JKRHeap::~JKRHeap() {
    mChildTree.getParent()->removeChild(&mChildTree);
    JSUTree< JKRHeap >* nextRootHeap = sRootHeap->mChildTree.getFirstChild();

    if (sCurrentHeap == this)
        sCurrentHeap = !nextRootHeap ? sRootHeap : nextRootHeap->getObject();

    if (sSystemHeap == this)
        sSystemHeap = !nextRootHeap ? sRootHeap : nextRootHeap->getObject();
}

bool JKRHeap::initArena(char** memory, u32* size, int maxHeaps) {
    void *ramStart, *ramEnd, *arenaStart;

    void* arenaLo = OSGetArenaLo();
    void* arenaHi = OSGetArenaHi();

    OSReport("original arenaLo = %p arenaHi = %p\n", arenaLo, arenaHi);

    if (arenaLo == arenaHi) {
        return false;
    }

    arenaStart = OSInitAlloc(arenaLo, arenaHi, maxHeaps);
    OSBootInfo* code = (OSBootInfo*)OSPhysicalToCached(0);
    ramStart = (void*)(((uintptr_t)arenaStart + 31) & ~(uintptr_t)0x1F);
    ramEnd = (void*)((uintptr_t)arenaHi & ~(uintptr_t)0x1F);

    JKRHeap::mCodeStart = code;
    JKRHeap::mCodeEnd = ramStart;
    JKRHeap::mUserRamStart = ramStart;
    JKRHeap::mUserRamEnd = ramEnd;
    JKRHeap::mMemorySize = code->memorySize;

    OSSetArenaLo(ramEnd);
    OSSetArenaHi(ramEnd);

    *memory = (char*)ramStart;
    *size = (uintptr_t)ramEnd - (uintptr_t)ramStart;
    return true;
}

JKRHeap* JKRHeap::becomeSystemHeap() {
    JKRHeap* sys = sSystemHeap;
    sSystemHeap = this;
    return sys;
}

JKRHeap* JKRHeap::becomeCurrentHeap() {
    JKRHeap* cur = sCurrentHeap;
    sCurrentHeap = this;
    return cur;
}

void JKRHeap::destroy(JKRHeap* pHeap) {
    pHeap->do_destroy();
}

void* JKRHeap::alloc(u32 size, int align, JKRHeap* pHeap) {
    if (pHeap != nullptr) {
        return pHeap->alloc(size, align);
    }

    if (JKRHeap::sCurrentHeap != nullptr) {
        return JKRHeap::sCurrentHeap->alloc(size, align);
    }

    return nullptr;
}

void* JKRHeap::alloc(u32 size, int align) {
#ifdef PETARI_NATIVE
    // Wii callers commonly request 4-byte alignment. Host objects contain 8-byte
    // pointers and mutexes, so keep the sign that selects head or tail allocation.
    if (align >= 0 && align < 8) {
        align = 8;
    } else if (align < 0 && align > -8) {
        align = -8;
    }
#endif
    return do_alloc(size, align);
}

void JKRHeap::free(void* pData, JKRHeap* pHeap) {
    if (!pHeap) {
        pHeap = findFromRoot(pData);

        if (!pHeap) {
            return;
        }
    }

    pHeap->do_free(pData);
}

void JKRHeap::free(void* pData) {
    do_free(pData);
}

void JKRHeap::callAllDisposer() {
    while (mDisposerList.mHead != nullptr) {
        reinterpret_cast< JKRDisposer* >(mDisposerList.mHead->mData)->~JKRDisposer();
    }
}

void JKRHeap::freeAll() {
    do_freeAll();
}

void JKRHeap::freeTail() {
    do_freeTail();
}

s32 JKRHeap::resize(void* pData, u32 size) {
    return do_resize(pData, size);
}

s32 JKRHeap::getFreeSize() {
    return do_getFreeSize();
}

void* JKRHeap::getMaxFreeBlock() {
    return do_getMaxFreeBlock();
}

s32 JKRHeap::getTotalFreeSize() {
    return do_getTotalFreeSize();
}

JKRHeap* JKRHeap::findFromRoot(void* pData) {
    JKRHeap* root = sRootHeap;

    if (root == nullptr) {
        return nullptr;
    }

    if ((void*)root->mStart <= pData && pData < (void*)root->mEnd) {
        return root->find(pData);
    }

    return root->findAllHeap(pData);
}

JKRHeap* JKRHeap::find(void* pData) const {
    if (mStart <= pData && pData < mEnd) {
        const JSUTree< JKRHeap >& tree = mChildTree;

        if (tree.getNumChildren() != 0) {
            for (JSUTreeIterator< JKRHeap > iterator(mChildTree.getFirstChild()); iterator != mChildTree.getEndChild(); ++iterator) {
                JKRHeap* result = iterator->find(pData);

                if (result) {
                    return result;
                }
            }
        }

        return const_cast< JKRHeap* >(this);
    }

    return nullptr;
}

JKRHeap* JKRHeap::findAllHeap(void* ptr) const {
    if (mChildTree.getNumChildren() != 0) {
        for (JSUTreeIterator< JKRHeap > iterator(mChildTree.getFirstChild()); iterator != mChildTree.getEndChild(); ++iterator) {
            JKRHeap* heap = iterator->findAllHeap(ptr);

            if (heap != nullptr) {
                return heap;
            }
        }
    }

    if (mStart <= ptr && ptr < mEnd) {
        return const_cast< JKRHeap* >(this);
    }

    return nullptr;
}

void JKRHeap::dispose_subroutine(uintptr_t start, uintptr_t end) {
    JSUListIterator< JKRDisposer > it(mDisposerList.getFirst());
    JSUListIterator< JKRDisposer > last_it;

    JSULink< JKRDisposer >* link;
    while ((link = it.mLink) != nullptr) {
        JKRDisposer* disp = link->getObject();

        if (reinterpret_cast< void* >(start) <= disp && disp < reinterpret_cast< void* >(end)) {
            disp->~JKRDisposer();

            if (last_it == nullptr) {
                it = mDisposerList.getFirst();
            } else {
                it = last_it;
                it++;
            }
        } else {
            last_it = it;
            it++;
        }
    }
}

bool JKRHeap::dispose(void* ptr, u32 size) {
    uintptr_t begin = (uintptr_t)ptr;
    uintptr_t end = (uintptr_t)ptr + size;
    dispose_subroutine(begin, end);
    return false;
}

void JKRHeap::dispose(void* begin, void* end) {
    dispose_subroutine((uintptr_t)begin, (uintptr_t)end);
}

void JKRHeap::dispose() {
    const JSUList< JKRDisposer >& list = mDisposerList;
    JSUListIterator< JKRDisposer > iterator;

    while (list.getFirst() != list.getEnd()) {
        iterator = list.getFirst();
        iterator->~JKRDisposer();
    }
}

void JKRHeap::copyMemory(void* pDst, void* pSrc, u32 size) {
    u32 count = (size + 3) / 4;
    u32* dst_32 = (u32*)pDst;
    u32* src_32 = (u32*)pSrc;

    while (count > 0) {
        *dst_32 = *src_32;
        dst_32++;
        src_32++;
        count--;
    }
}

void JKRDefaultMemoryErrorRoutine(void* pHeap, u32 size, int alignment) {
    JUTException::panic(__FILE__, 0x355, "abort\n");
}

JKRErrorHandler JKRHeap::setErrorHandler(JKRErrorHandler errorHandler) {
    JKRErrorHandler prev = JKRHeap::mErrorHandler;

    if (!errorHandler) {
        errorHandler = JKRDefaultMemoryErrorRoutine;
    }

    JKRHeap::mErrorHandler = errorHandler;
    return prev;
}

#ifdef PETARI_NATIVE
// Host runtime libraries share the replaceable global allocation functions,
// so memory is returned to the allocator that owns it. On registered game
// threads, allocations follow the current JKR heap as on Wii. Host threads,
// HostAllocationScope regions, and allocations made before a current heap
// exists use the host allocator (see petari/host_allocation.hpp).
#include <cstdlib>

static void JKRNativeAllocFailed() {
#if defined(__cpp_exceptions)
    throw std::bad_alloc();
#else
    std::abort();
#endif
}

static void* JKRNativeAlloc(std::size_t size, std::size_t align) {
    if (size > 0xFFFFFFFF || align > 0x7FFFFFFF) {
        JKRNativeAllocFailed();
    }

    void* ptr;
    if (JKRHeap::sCurrentHeap != nullptr && !PetariNative::isHostAllocationActive()) {
        ptr = JKRHeap::alloc(size, align, nullptr);
    } else if (align <= __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
        ptr = std::malloc(size == 0 ? 1 : size);
    } else {
        ptr = std::aligned_alloc(align, size == 0 ? align : (size + align - 1) & ~(align - 1));
    }

    if (ptr == nullptr) {
        JKRNativeAllocFailed();
    }

    return ptr;
}

static void JKRNativeFree(void* pData) {
    if (pData == nullptr) {
        return;
    }

    // JKR heaps live in the MEM1/MEM2 game arenas. Host memory is freed without
    // walking the heap tree, which game threads may be modifying.
    if (JKRHeap::sRootHeap == nullptr || (!OSIsMEM1Region(pData) && !OSIsMEM2Region(pData))) {
        std::free(pData);
        return;
    }

    JKRHeap* heap = JKRHeap::findFromRoot(pData);
    if (heap != nullptr) {
        heap->free(pData);
    } else {
        std::free(pData);
    }
}

void* operator new(std::size_t size) {
    return JKRNativeAlloc(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
}

void* operator new(std::size_t size, std::align_val_t align) {
    return JKRNativeAlloc(size, static_cast< std::size_t >(align));
}

void* operator new(std::size_t size, int align) {
    return JKRHeap::alloc(size, align, nullptr);
}

void* operator new(std::size_t size, JKRHeap* pHeap, int align) {
    return JKRHeap::alloc(size, align, pHeap);
}

void* operator new[](std::size_t size) {
    return JKRNativeAlloc(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
}

void* operator new[](std::size_t size, std::align_val_t align) {
    return JKRNativeAlloc(size, static_cast< std::size_t >(align));
}

void* operator new[](std::size_t size, int align) {
    return JKRHeap::alloc(size, align, nullptr);
}

void* operator new[](std::size_t size, JKRHeap* pHeap, int align) {
    return JKRHeap::alloc(size, align, pHeap);
}

void operator delete(void* pData) noexcept {
    JKRNativeFree(pData);
}

void operator delete(void* pData, std::align_val_t) noexcept {
    JKRNativeFree(pData);
}

void operator delete[](void* pData) noexcept {
    JKRNativeFree(pData);
}

void operator delete[](void* pData, std::align_val_t) noexcept {
    JKRNativeFree(pData);
}
#else
void* operator new(u32 size) {
    return JKRHeap::alloc(size, 4, nullptr);
}

void* operator new(u32 size, int align) {
    return JKRHeap::alloc(size, align, nullptr);
}

void* operator new(u32 size, JKRHeap* pHeap, int align) {
    return JKRHeap::alloc(size, align, pHeap);
}

void* operator new[](u32 size) {
    return JKRHeap::alloc(size, 4, nullptr);
}

void* operator new[](u32 size, int align) {
    return JKRHeap::alloc(size, align, nullptr);
}

void* operator new[](u32 size, JKRHeap* pHeap, int align) {
    return JKRHeap::alloc(size, align, pHeap);
}

void operator delete(void* pData) {
    JKRHeap::free(pData, nullptr);
}

void operator delete[](void* pData) {
    JKRHeap::free(pData, nullptr);
}
#endif

void JKRHeap::state_register(TState*, u32) const {
    return;
}

bool JKRHeap::state_compare(const TState& lhs, const TState& rhs) const {
    return lhs.mCheckCode == rhs.mCheckCode;
}

void JKRHeap::state_dump(const TState&) const {
    return;
}

void JKRHeap::setAltAramStartAdr(uintptr_t addr) {
    ARALT_AramStartAdr = addr;
}

uintptr_t JKRHeap::getAltAramStartAdr() {
    return ARALT_AramStartAdr;
}

s32 JKRHeap::do_changeGroupID(u8) {
    return 0;
}

u8 JKRHeap::do_getCurrentGroupId() {
    return 0;
}

bool JKRHeap::dump_sort() {
    return true;
}
