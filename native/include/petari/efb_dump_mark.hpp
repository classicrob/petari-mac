#pragma once
// Phase marks shared between the smoke script and the opt-in EFB PNG dump
// (native/gx/efb_snapshot.cpp, PETARI_EFB_DUMP). Header-only so the smoke
// library needs no renderer dependency; one binary shares the inline atomics.
#include <atomic>
#include <cstdint>

namespace PetariNative::EfbDump {
inline std::atomic<std::uint64_t> markCounter{0};
inline std::atomic<const char*> markLabel{"none"};
// `label` must have static storage duration (a string literal).
inline void mark(const char* label) {
    markLabel.store(label, std::memory_order_release);
    markCounter.fetch_add(1, std::memory_order_acq_rel);
}
}
