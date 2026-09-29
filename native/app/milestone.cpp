// petari/milestone.hpp (a lock-free history of the last kHistory names) and
// petari/ui_observe.hpp (targets and prompts published since the last take).

#include "petari/milestone.hpp"
#include "petari/ui_observe.hpp"
#include "ui_observe_store.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <algorithm>
#include <mutex>

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

// --- petari/ui_observe.hpp ---
// Fixed arrays of plain values: the hooks run on game threads, where operator
// new would take game heap memory. IDs are static strings per the contract.

namespace {

struct RawTarget {
    const char* id;
    int index;
    float u, v;
    unsigned flags;
};
struct RawPrompt {
    const char* messageId;
    int type;
};

constexpr int kMaxTargets = 128;
constexpr int kMaxPrompts = 32;

std::mutex gUiMutex;
RawTarget gUiTargets[kMaxTargets];
int gUiTargetCount = 0;
RawPrompt gUiPrompts[kMaxPrompts];
int gUiPromptCount = 0;

bool isObserving() {
    static const bool observing = [] {
        const char* value = std::getenv("PETARI_SMOKE");
        return value != nullptr && value[0] != '\0';
    }();
    return observing;
}

}  // namespace

extern "C" int petari_ui_observing(void) {
    return isObserving() ? 1 : 0;
}

extern "C" void petari_ui_target(const char* id, int index, float u, float v, unsigned flags) {
    if (!isObserving() || id == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(gUiMutex);
    if (gUiTargetCount < kMaxTargets) {
        gUiTargets[gUiTargetCount++] = {id, index, u, v, flags};
    }
}

extern "C" void petari_ui_prompt(const char* messageId, int type) {
    if (!isObserving() || messageId == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(gUiMutex);
    if (gUiPromptCount < kMaxPrompts) {
        gUiPrompts[gUiPromptCount++] = {messageId, type};
    }
}

namespace PetariNative::App::UiObserve {

// App side (under the seam's host allocation scope): copies into strings.
void take(std::vector<Target>* targets, std::vector<Prompt>* prompts) {
    RawTarget rawTargets[kMaxTargets];
    RawPrompt rawPrompts[kMaxPrompts];
    int targetCount;
    int promptCount;
    {
        std::lock_guard<std::mutex> lock(gUiMutex);
        targetCount = gUiTargetCount;
        promptCount = gUiPromptCount;
        std::copy(gUiTargets, gUiTargets + targetCount, rawTargets);
        std::copy(gUiPrompts, gUiPrompts + promptCount, rawPrompts);
        gUiTargetCount = 0;
        gUiPromptCount = 0;
    }
    targets->clear();
    prompts->clear();
    for (int i = 0; i < targetCount; i++) {
        targets->push_back({rawTargets[i].id, rawTargets[i].index, rawTargets[i].u, rawTargets[i].v, rawTargets[i].flags});
    }
    for (int i = 0; i < promptCount; i++) {
        prompts->push_back({rawPrompts[i].messageId, rawPrompts[i].type});
    }
}

}  // namespace PetariNative::App::UiObserve
