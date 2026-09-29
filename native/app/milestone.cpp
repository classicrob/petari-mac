// petari/milestone.hpp. A lock-free history of the last kHistory names.

#include "petari/milestone.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace {

constexpr unsigned long kHistory = 64;

std::atomic<const char*> gNames[kHistory];
std::atomic<unsigned long> gWritten{0};    // slots claimed
std::atomic<unsigned long> gPublished{0};  // slots readable, in order

bool isVerbose() {
    static const bool verbose = [] {
        for (const char* variable : {"PETARI_TRACE_BOOT", "PETARI_SMOKE"}) {
            const char* value = std::getenv(variable);
            if (value != nullptr && value[0] != '\0' && value[0] != '0') {
                return true;
            }
        }
        return false;
    }();
    return verbose;
}

}  // namespace

extern "C" void petari_milestone(const char* name) {
    const unsigned long index = gWritten.fetch_add(1);
    gNames[index % kHistory].store(name, std::memory_order_relaxed);
    // Publish in index order, so readers never see a slot before it is written.
    unsigned long expected = index;
    while (!gPublished.compare_exchange_weak(expected, index + 1, std::memory_order_release)) {
        expected = index;
    }
    if (isVerbose()) {
        std::fprintf(stderr, "[milestone] %s\n", name);
        std::fflush(stderr);
    }
}

extern "C" unsigned long petari_milestone_count(void) {
    return gPublished.load(std::memory_order_acquire);
}

extern "C" const char* petari_milestone_at(unsigned long index) {
    const unsigned long count = petari_milestone_count();
    if (index >= count || count - index > kHistory) {
        return nullptr;
    }
    return gNames[index % kHistory].load(std::memory_order_relaxed);
}
