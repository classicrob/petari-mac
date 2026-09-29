#include <petari/asset_diagnostics.hpp>
#include <petari/host_allocation.hpp>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <tuple>

namespace {
struct Ledger {
    std::atomic<std::uint64_t> occurrences{0};
    std::atomic<std::uint64_t> unique{0};
    std::mutex mutex;
    std::set<std::pair<std::string, std::string>> seen;
};

Ledger& ledger(bool sound) {
    // Game shutdown can still perform lookups after other static destructors.
    static Ledger* layout = new Ledger;
    static Ledger* audio = new Ledger;
    return sound ? *audio : *layout;
}

void report(bool sound, const char* layout, const char* name, const char* kind) {
    PetariNative::HostAllocationScope host;
    Ledger& entries = ledger(sound);
    const auto count = entries.occurrences.fetch_add(1, std::memory_order_relaxed) + 1;
    std::lock_guard<std::mutex> guard(entries.mutex);
    const char* safeLayout = layout ? layout : "<uninitialized>";
    const char* safeName = name ? name : "<root/null>";
    if (entries.seen.emplace(safeLayout, safeName).second) {
        entries.unique.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr, "[%s] missing %s layout=\"%s\" name=\"%s\" count=%llu; operation skipped\n",
                     sound ? "sound" : "layout", kind, safeLayout, safeName,
                     static_cast<unsigned long long>(count));
    }
}

std::uint64_t count(bool sound, bool unique) {
    PetariNative::HostAllocationScope host;
    Ledger& entries = ledger(sound);
    return (unique ? entries.unique : entries.occurrences).load(std::memory_order_relaxed);
}
}

namespace PetariNative {
void reportMissingLayoutReference(const char* layout, const char* name, const char* kind) {
    report(false, layout, name, kind);
}
void reportMissingSoundReference(const char* name) {
    report(true, "BSTN", name, "sound");
}
}

extern "C" std::uint64_t petari_layout_missing_reference_count() { return count(false, false); }
extern "C" std::uint64_t petari_sound_missing_reference_count() { return count(true, false); }
extern "C" std::uint64_t petari_layout_missing_reference_unique_count() { return count(false, true); }
extern "C" std::uint64_t petari_sound_missing_reference_unique_count() { return count(true, true); }
