#include <petari/host_allocation.hpp>

#include <cxxabi.h>
#include <dlfcn.h>
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace PetariNative {
namespace {
thread_local bool sGameAllocationThread;
thread_local unsigned int sHostAllocationDepth;
thread_local bool sCpuReleased;
thread_local bool sClassifying;
thread_local std::uintptr_t sStackLow, sStackHigh;

// ---- Allocation-site routing ----
// A game-heap candidate is an allocation on a game thread outside any
// HostAllocationScope while a JKR heap is current. Its call chain is hashed
// (frame pointers only, no locks, no allocation) and looked up in a fixed
// table of verdicts; a chain seen for the first time is symbolized once and
// classified by its first frame that is not the allocator, the C++ library or
// an anonymous-namespace helper. Renderer/host/platform code (Aurora, Dawn,
// SDL, fmt, PetariNative, any C-linkage function: petari_*/aurora_* ABI,
// native SDK replacements) is routed to the host allocator, exactly as if a
// HostAllocationScope were open, and counted per site (logged once): host
// bookkeeping in the game's current heap crashed, deadlocked or exhausted it
// three times (petari_gx_pipeline_stage_wait, os_cache logStore,
// petari_gx_revalidate_texobj). Game code keeps the JKR heap. Allocations
// while the thread has released the CPU (host work) are always host.

constexpr int kFrames = 16;
constexpr int kKeyFrames = 8;                // chain identity
constexpr std::size_t kTableSize = 1 << 16;  // power of two
constexpr int kProbes = 8;
constexpr int kReportedSites = 24;

enum Verdict : std::uint8_t { kUnknown = 0, kGame = 1, kHost = 2 };

std::atomic<std::uint64_t> gKeys[kTableSize];
std::atomic<std::uint8_t> gVerdicts[kTableSize];
std::atomic<std::uint64_t> gSiteIds[kTableSize];  // start of the deciding function (reports group by it)
std::atomic<std::uint32_t> gCounts[kTableSize];   // routed (or CPU-released) allocations per chain
std::atomic<std::uint64_t> gRouted{0}, gReleasedCount{0}, gEvicted{0};
std::atomic<bool> gSiteCheck{[] {
    const char* value = std::getenv("PETARI_ALLOC_SITE_CHECK");
    return value == nullptr || value[0] != '0';
}()};

struct Stats {
    std::mutex lock;
    std::uint64_t classified = 0;
    struct Site {
        std::string description;
        std::uint64_t key;
        std::uint64_t count;
        bool released;
    } sites[kReportedSites];
    int siteCount = 0;
};
Stats& stats() {
    static Stats* instance = [] {
        HostAllocationScope scope;
        auto* created = new Stats;
        // A thread in noteSite holds the lock while printing; a child forked
        // meanwhile would never get it back.
        pthread_atfork([] { stats().lock.lock(); }, [] { stats().lock.unlock(); }, [] { stats().lock.unlock(); });
        return created;
    }();
    return *instance;
}

void stackBounds() {
    if (sStackHigh == 0) {
        sStackHigh = reinterpret_cast<std::uintptr_t>(pthread_get_stackaddr_np(pthread_self()));
        sStackLow = sStackHigh - pthread_get_stacksize_np(pthread_self());
    }
}

// Return addresses of the calling chain, starting with this function's caller.
__attribute__((noinline)) int walk(std::uintptr_t (&frames)[kFrames]) {
    stackBounds();
    auto fp = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
    int n = 0;
    while (n < kFrames && fp >= sStackLow && fp + 16 <= sStackHigh && (fp & 7) == 0) {
        const auto* frame = reinterpret_cast<const std::uintptr_t*>(fp);
        frames[n++] = frame[1] & 0x0000FFFFFFFFFFFFull;  // strip pointer authentication
        if (frame[0] <= fp) {
            break;
        }
        fp = frame[0];
    }
    return n;
}

std::uint64_t hashFrames(const std::uintptr_t* frames, int n) {
    std::uint64_t h = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < n; ++i) {
        h ^= frames[i] + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    }
    return h == 0 ? 1 : h;
}

// Demangled name reduced to its qualified function name: no return type,
// parameters or template arguments; "(anonymous namespace)" becomes "{anon}".
// `cLinkage`: the symbol is not C++-mangled (an extern "C" or C function).
std::string functionName(std::uintptr_t address, std::uint64_t* start = nullptr, bool* cLinkage = nullptr) {
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(address - 1), &info) == 0 || info.dli_sname == nullptr) {
        return {};
    }
    if (start != nullptr) {
        *start = reinterpret_cast<std::uint64_t>(info.dli_saddr);
    }
    if (cLinkage != nullptr) {
        *cLinkage = std::strncmp(info.dli_sname, "_Z", 2) != 0;
    }
    char* demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, nullptr);
    std::string name = demangled != nullptr ? demangled : info.dli_sname;
    std::free(demangled);
    for (std::size_t at; (at = name.find("(anonymous namespace)")) != std::string::npos;) {
        name.replace(at, 21, "{anon}");
    }
    for (std::size_t at; (at = name.find("operator ")) != std::string::npos;) {
        name[at + 8] = '_';  // "operator new" stays one token
    }
    std::string reduced;
    int depth = 0;
    for (char c : name) {
        if (c == '(' && depth == 0) {
            break;
        }
        if (c == '<') {
            ++depth;
        } else if (c == '>') {
            --depth;
        } else if (depth == 0) {
            reduced += c;
        }
    }
    const std::size_t space = reduced.rfind(' ');
    return space == std::string::npos ? reduced : reduced.substr(space + 1);
}

bool startsWith(const std::string& text, const char* prefix) {
    return text.compare(0, std::strlen(prefix), prefix) == 0;
}

bool allocatorOrLibrary(const std::string& name) {
    return name.empty() || startsWith(name, "std::") || startsWith(name, "{anon}") || startsWith(name, "operator_new") ||
           startsWith(name, "operator_delete") || name == "JKRNativeAlloc" || startsWith(name, "PetariNative::isHostAllocationActive") ||
           startsWith(name, "PetariNative::{anon}") || startsWith(name, "__");
}

// Where a thread's own code starts: an allocation reached only through
// anonymous-namespace frames from here is the thread's (game) code.
bool threadStart(const std::string& name) {
    return name == "PetariNative::Platform::OS::{anon}::hostEntry" || name == "main" || name == "start" ||
           name == "_pthread_start" || name == "thread_start" || name == "petari_game_main";
}

bool hostCode(const std::string& name) {
    static const char* const kQualified[] = {"aurora::", "PetariNative::", "dawn::", "dawn_native::", "wgpu::", "webgpu::",
                                             "tint::", "fmt::", "absl::", "tracy::", "ImGui", "Rml::", "nod::"};
    for (const char* prefix : kQualified) {
        if (startsWith(name, prefix)) {
            return true;
        }
    }
    if (name.find("::") != std::string::npos) {
        return false;  // a game class or namespace
    }
    if (name == "petari_game_main") {
        return false;  // the game's entry point (src/Game/System/GameSystem.cpp)
    }
    // C entry points: Petari/Aurora ABI, and SDK functions natively implemented in C++.
    static const char* const kC[] = {"petari_", "aurora_", "wgpu", "SDL_", "GX", "VI", "OS", "__OS", "DVD", "NAND", "AI",
                                     "AX", "DSP", "SC", "WPAD", "KPAD", "NWC24", "VF", "SI", "CARD"};
    for (const char* prefix : kC) {
        if (startsWith(name, prefix)) {
            return true;
        }
    }
    return false;
}

void noteSite(std::uint64_t key, const std::uintptr_t* frames, int n, bool released) {
    HostAllocationScope scope;
    sClassifying = true;  // no checks for this function's own allocations
    struct Done {
        ~Done() { sClassifying = false; }
    } done;
    Stats& st = stats();
    std::lock_guard<std::mutex> guard(st.lock);
    for (int i = 0; i < st.siteCount; ++i) {
        if (st.sites[i].key == key) {
            return;  // described and logged at its first chain
        }
    }
    if (st.siteCount == kReportedSites) {
        return;
    }
    std::string chain;
    int shown = 0;
    for (int i = 0; i < n && shown < 5; ++i) {
        std::string name = functionName(frames[i]);
        if (name.empty() || startsWith(name, "PetariNative::isHostAllocationActive") || startsWith(name, "operator_new") ||
            name == "JKRNativeAlloc" || startsWith(name, "PetariNative::{anon}")) {
            continue;
        }
        chain += shown++ == 0 ? name : " <- " + name;
    }
    st.sites[st.siteCount++] = {chain, key, 1, released};
    std::fprintf(stderr, "[alloc] %s: %s\n",
                 released ? "allocation with the CPU released, moved to the host allocator"
                          : "host code allocating on a game thread without a HostAllocationScope, routed to the host allocator",
                 chain.c_str());
}

// Returns true when the chain is host code. Classifies it once.
bool classify(bool released) {
    std::uintptr_t frames[kFrames];
    const int n = walk(frames);
    const std::uint64_t key = hashFrames(frames, n < kKeyFrames ? n : kKeyFrames);
    const std::size_t start = key & (kTableSize - 1);
    for (int p = 0; p < kProbes; ++p) {
        const std::size_t slot = (start + p) & (kTableSize - 1);
        const std::uint64_t stored = gKeys[slot].load(std::memory_order_acquire);
        if (stored == key) {
            const std::uint8_t verdict = gVerdicts[slot].load(std::memory_order_acquire);
            if (verdict == kGame && !released) {
                return false;
            }
            if (verdict != kUnknown) {
                gCounts[slot].fetch_add(1, std::memory_order_relaxed);
                (released ? gReleasedCount : gRouted).fetch_add(1, std::memory_order_relaxed);
                return verdict == kHost;
            }
            break;
        }
        if (stored == 0) {
            break;
        }
    }
    // First sight: symbolize (host allocator: the scope below routes it there).
    HostAllocationScope scope;
    sClassifying = true;
    bool host = false;
    std::uint64_t site = key;
    for (int i = 0; i < n; ++i) {
        std::uint64_t function = 0;
        bool cLinkage = false;
        const std::string name = functionName(frames[i], &function, &cLinkage);
        if (threadStart(name)) {
            break;
        }
        if (allocatorOrLibrary(name)) {
            continue;
        }
        // Game C++ is mangled, and C cannot call operator new: an unmangled
        // function here is native code behind a C entry point (an SDK
        // replacement such as DCStoreRange, a petari_* ABI), whatever its name.
        host = hostCode(name) || cLinkage;
        site = function;
        break;
    }
    std::size_t slot = start;
    bool stored = false;
    for (int p = 0; p < kProbes && !stored; ++p) {
        slot = (start + p) & (kTableSize - 1);
        std::uint64_t expected = 0;
        stored = gKeys[slot].compare_exchange_strong(expected, key, std::memory_order_acq_rel) || expected == key;
    }
    if (!stored) {
        // A full cluster evicts its first entry (re-classified if seen again);
        // never stop checking, which would let host code into the game heap.
        slot = start;
        gVerdicts[slot].store(kUnknown, std::memory_order_release);
        gKeys[slot].store(key, std::memory_order_release);
        gEvicted.fetch_add(1, std::memory_order_relaxed);
    }
    gSiteIds[slot].store(site, std::memory_order_relaxed);
    gCounts[slot].store(host || released ? 1 : 0, std::memory_order_relaxed);
    gVerdicts[slot].store(host ? kHost : kGame, std::memory_order_release);
    if (host || released) {
        (released ? gReleasedCount : gRouted).fetch_add(1, std::memory_order_relaxed);
    }
    {
        std::lock_guard<std::mutex> guard(stats().lock);
        ++stats().classified;
    }
    sClassifying = false;
    if (host || released) {
        noteSite(site, frames, n, released);
    }
    return host;
}
}  // namespace

void setGameAllocationThread(bool enabled) {
    sGameAllocationThread = enabled;
}

bool isGameAllocationThread() {
    return sGameAllocationThread;
}

HostAllocationScope::HostAllocationScope() {
    sHostAllocationDepth++;
}

HostAllocationScope::~HostAllocationScope() {
    sHostAllocationDepth--;
}

bool isHostAllocationActive() {
    if (!sGameAllocationThread || sHostAllocationDepth != 0 || sClassifying) {
        return true;
    }
    if (sCpuReleased) {
        classify(true);
        return true;
    }
    // Host code on a game thread: the host allocator, as if a scope were open.
    return gSiteCheck.load(std::memory_order_relaxed) && classify(false);
}

CodeKind classifyCode(std::uintptr_t address, std::string* name) {
    HostAllocationScope scope;
    bool cLinkage = false;
    std::string function = functionName(address, nullptr, &cLinkage);
    CodeKind kind;
    if (function.empty()) {
        kind = CodeKind::Unknown;
    } else if (threadStart(function)) {
        kind = CodeKind::ThreadStart;
    } else if (startsWith(function, "{anon}") || function.find("::{anon}") != std::string::npos) {
        kind = CodeKind::Anonymous;
    } else if (allocatorOrLibrary(function)) {
        kind = CodeKind::Library;
    } else {
        kind = hostCode(function) || cLinkage ? CodeKind::Host : CodeKind::Game;
    }
    if (name != nullptr) {
        *name = std::move(function);
    }
    return kind;
}

void setGameCpuReleased(bool released) {
    sCpuReleased = released;
}

void setAllocationSiteCheck(bool enabled) {
    gSiteCheck.store(enabled, std::memory_order_relaxed);
}

AllocationDiagnostics allocationDiagnostics() {
    Stats& st = stats();
    std::lock_guard<std::mutex> guard(st.lock);
    return {gRouted.load(), gReleasedCount.load(), st.classified, st.siteCount};
}

void reportAllocationDiagnostics(std::FILE* out) {
    HostAllocationScope scope;
    // Routed allocations per deciding function, from the chain table.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> perSite;
    for (std::size_t slot = 0; slot < kTableSize; ++slot) {
        const std::uint32_t count = gCounts[slot].load(std::memory_order_relaxed);
        if (count == 0) {
            continue;
        }
        const std::uint64_t site = gSiteIds[slot].load(std::memory_order_relaxed);
        auto it = std::find_if(perSite.begin(), perSite.end(), [&](const auto& e) { return e.first == site; });
        if (it == perSite.end()) {
            perSite.emplace_back(site, count);
        } else {
            it->second += count;
        }
    }
    std::sort(perSite.begin(), perSite.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    Stats& st = stats();
    std::lock_guard<std::mutex> guard(st.lock);
    std::fprintf(out,
                 "[alloc] summary: %llu allocations by host code on game threads routed to the host allocator, %llu with "
                 "the CPU released, %zu sites, %llu call chains classified (%llu evicted)%s\n",
                 static_cast<unsigned long long>(gRouted.load()), static_cast<unsigned long long>(gReleasedCount.load()),
                 perSite.size(), static_cast<unsigned long long>(st.classified),
                 static_cast<unsigned long long>(gEvicted.load()), gSiteCheck.load() ? "" : " (site routing off)");
    for (const auto& [site, count] : perSite) {
        std::string description;
        for (int i = 0; i < st.siteCount; ++i) {
            if (st.sites[i].key == site) {
                description = st.sites[i].description + (st.sites[i].released ? " (CPU released)" : "");
            }
        }
        if (description.empty()) {
            description = functionName(site + 1);
        }
        std::fprintf(out, "[alloc]   %llu x %s\n", static_cast<unsigned long long>(count), description.c_str());
    }
}
}  // namespace PetariNative
