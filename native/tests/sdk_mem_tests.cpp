// Wii SDK MEM expanded heap and allocator on native pointers.
#include <revolution/mem/allocator.h>
#include <revolution/mem/expHeap.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

extern "C" void OSInit();

static int sFailures;
static int sChecks;

#define CHECK(expr)                                                                                                                                  \
    do {                                                                                                                                             \
        sChecks++;                                                                                                                                   \
        if (!(expr)) {                                                                                                                               \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr);                                                          \
            sFailures++;                                                                                                                             \
        }                                                                                                                                            \
    } while (0)

namespace {
struct Block {
    u8* data;
    u32 size;
    u8 fill;
};

bool aligned(const void* pointer, u32 alignment) {
    return reinterpret_cast< uintptr_t >(pointer) % alignment == 0;
}

void testCreate(std::vector< u8 >& buffer) {
    // An unaligned start is rounded up; too small a region is rejected.
    MEMHeapHandle tiny = MEMCreateExpHeapEx(buffer.data() + 3, 32, 0);
    CHECK(tiny == nullptr);

    MEMHeapHandle heap = MEMCreateExpHeapEx(buffer.data() + 3, 1 << 20, 4);
    CHECK(heap != nullptr && aligned(heap, 8));
    u32 total = MEMGetAllocatableSizeForExpHeapEx(heap, 8);
    CHECK(total > (1 << 20) - 256 && total < (1 << 20));
    MEMDestroyExpHeap(heap);
}

void testRandomAllocations(std::vector< u8 >& buffer) {
    MEMHeapHandle heap = MEMCreateExpHeapEx(buffer.data(), static_cast< u32 >(buffer.size()), 4);
    CHECK(heap != nullptr);
    const u32 initial = MEMGetAllocatableSizeForExpHeapEx(heap, 8);

    std::mt19937 random(1234);
    std::vector< Block > blocks;
    const int alignments[] = {4, 8, 16, 32, -4, -8, -32};
    bool ok = true;
    for (int step = 0; step < 4000; step++) {
        if (blocks.empty() || random() % 3 != 0) {
            u32 size = 1 + random() % 3000;
            int alignment = alignments[random() % 7];
            u8* data = static_cast< u8* >(MEMAllocFromExpHeapEx(heap, size, alignment));
            if (data == nullptr) {
                continue;
            }
            u32 magnitude = alignment < 0 ? -alignment : alignment;
            ok &= aligned(data, magnitude < 8 ? 8 : magnitude);
            ok &= data >= buffer.data() && data + size <= buffer.data() + buffer.size();
            u8 fill = static_cast< u8 >(random());
            std::memset(data, fill, size);
            blocks.push_back({data, size, fill});
        } else {
            size_t index = random() % blocks.size();
            Block block = blocks[index];
            for (u32 i = 0; i < block.size; i++) {
                ok &= block.data[i] == block.fill;
            }
            MEMFreeToExpHeap(heap, block.data);
            blocks.erase(blocks.begin() + index);
        }
    }
    CHECK(ok);

    // Every surviving block kept its contents, and freeing all of them
    // coalesces the heap back to one free region.
    for (const Block& block : blocks) {
        for (u32 i = 0; i < block.size; i++) {
            ok &= block.data[i] == block.fill;
        }
        MEMFreeToExpHeap(heap, block.data);
    }
    CHECK(ok);
    CHECK(MEMGetAllocatableSizeForExpHeapEx(heap, 8) == initial);
    MEMDestroyExpHeap(heap);
}

void testAllocator(std::vector< u8 >& buffer) {
    MEMHeapHandle heap = MEMCreateExpHeapEx(buffer.data(), 1 << 16, 4);
    MEMAllocator allocator;
    MEMInitAllocatorForExpHeap(&allocator, heap, 32);
    void* a = MEMAllocFromAllocator(&allocator, 100);
    void* b = MEMAllocFromAllocator(&allocator, 200);
    CHECK(a != nullptr && b != nullptr && a != b && aligned(a, 32) && aligned(b, 32));
    MEMFreeToAllocator(&allocator, a);
    MEMFreeToAllocator(&allocator, b);
    CHECK(MEMGetAllocatableSizeForExpHeapEx(heap, 8) > (1 << 16) - 256);
    MEMDestroyExpHeap(heap);
}

// Heaps created inside another heap's region are found through the child list.
void testNestedHeaps(std::vector< u8 >& buffer) {
    MEMHeapHandle parent = MEMCreateExpHeapEx(buffer.data(), 1 << 20, 4);
    void* region = MEMAllocFromExpHeapEx(parent, 1 << 16, 8);
    MEMHeapHandle child = MEMCreateExpHeapEx(region, 1 << 16, 4);
    CHECK(child != nullptr && parent->childList.num == 1);
    void* inner = MEMAllocFromExpHeapEx(child, 64, 8);
    CHECK(inner != nullptr);
    MEMFreeToExpHeap(child, inner);
    MEMDestroyExpHeap(child);
    CHECK(parent->childList.num == 0);
    MEMFreeToExpHeap(parent, region);
    MEMDestroyExpHeap(parent);
}
}  // namespace

int main() {
    OSInit();
    std::vector< u8 > buffer(8 << 20);
    testCreate(buffer);
    testRandomAllocations(buffer);
    testAllocator(buffer);
    testNestedHeaps(buffer);

    if (sFailures != 0) {
        std::fprintf(stderr, "%d of %d sdk mem check(s) failed\n", sFailures, sChecks);
        return 1;
    }
    std::printf("sdk mem tests passed (%d checks)\n", sChecks);
    return 0;
}
