// Spike profiler (spike_profiler.hpp).

#include "spike_profiler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(__APPLE__) && defined(__aarch64__)
#include <cxxabi.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>
#include <pthread/qos.h>
#define PETARI_SPIKE_PROFILER 1
#endif

namespace PetariNative::App::SpikeProfiler {

#ifdef PETARI_SPIKE_PROFILER
namespace {

constexpr unsigned kMaxDepth = 48;
constexpr std::size_t kRingSamples = 8192;
constexpr std::size_t kMaxSpikes = 64;
constexpr std::size_t kMaxSpikeSamples = 4000;
const char* const kThreadNames[] = {"game (main)", "GX FIFO processor"};
constexpr unsigned kThreads = 2;

struct Sample {
    std::uint64_t ns = 0;
    std::uint8_t thread = 0;
    std::uint8_t depth = 0;
    std::uint64_t pcs[kMaxDepth] = {};
};

struct Target {
    thread_act_t port = MACH_PORT_NULL;
    std::uintptr_t stackLow = 0, stackHigh = 0;
};

struct Spike {
    std::uint64_t frame = 0;
    double intervalMs = 0.0;
    std::vector<Sample> samples;
};

struct State {
    std::string path;
    unsigned periodUs = 1000;
    Target targets[kThreads];
    std::mutex ringLock;  // ring and next
    std::vector<Sample> ring;
    std::size_t next = 0;
    std::uint64_t samples = 0;
    std::vector<Spike> spikes;  // main thread only
    std::uint64_t droppedSpikes = 0;
    std::atomic<bool> stop{false};
};
State* gState = nullptr;

std::uint64_t nowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

bool bindTarget(Target& target, pthread_t thread, thread_act_t port) {
    target.port = port;
    target.stackHigh = reinterpret_cast<std::uintptr_t>(pthread_get_stackaddr_np(thread));
    target.stackLow = target.stackHigh - pthread_get_stacksize_np(thread);
    return true;
}

// Finds Aurora's FIFO processor thread by name.
bool findProcessor(Target& target) {
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
        return false;
    }
    bool found = false;
    for (mach_msg_type_number_t i = 0; i < count; i++) {
        pthread_t thread = pthread_from_mach_thread_np(threads[i]);
        char name[64] = {};
        if (!found && thread != nullptr && pthread_getname_np(thread, name, sizeof(name)) == 0 &&
            std::strstr(name, "FIFO processor") != nullptr) {
            found = bindTarget(target, thread, threads[i]);
            continue;  // keep this port reference
        }
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), count * sizeof(thread_act_t));
    return found;
}

// Suspends the thread, reads pc/lr/fp and walks the frame-pointer chain.
// Nothing here allocates or locks while the target is suspended.
bool capture(const Target& target, Sample& sample) {
    if (thread_suspend(target.port) != KERN_SUCCESS) {
        return false;
    }
    arm_thread_state64_t state;
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    const bool ok = thread_get_state(target.port, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state),
                                     &count) == KERN_SUCCESS;
    unsigned depth = 0;
    if (ok) {
        constexpr std::uint64_t kAddressMask = 0x0000ffffffffffffull;
        sample.pcs[depth++] = arm_thread_state64_get_pc(state) & kAddressMask;
        std::uintptr_t fp = arm_thread_state64_get_fp(state);
        const std::uint64_t lr = arm_thread_state64_get_lr(state) & kAddressMask;
        if (lr != 0 && lr != sample.pcs[0]) {
            sample.pcs[depth++] = lr;  // the leaf may not have pushed a frame yet
        }
        while (depth < kMaxDepth && fp >= target.stackLow && fp + 16 <= target.stackHigh && (fp & 7) == 0) {
            const auto* frame = reinterpret_cast<const std::uint64_t*>(fp);
            const std::uint64_t ret = frame[1] & kAddressMask;
            const std::uintptr_t prev = static_cast<std::uintptr_t>(frame[0]);
            if (ret == 0) {
                break;
            }
            sample.pcs[depth++] = ret;
            if (prev <= fp) {
                break;
            }
            fp = prev;
        }
    }
    thread_resume(target.port);
    sample.depth = static_cast<std::uint8_t>(depth);
    return ok && depth > 0;
}

void samplerMain() {
    pthread_setname_np("Petari spike profiler");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    State& s = *gState;
    auto nextProcessorLookup = nowNs();
    auto next = std::chrono::steady_clock::now();
    Sample sample;
    while (!s.stop.load(std::memory_order_relaxed)) {
        next += std::chrono::microseconds(s.periodUs);
        std::this_thread::sleep_until(next);
        if (s.targets[1].port == MACH_PORT_NULL && nowNs() >= nextProcessorLookup) {
            findProcessor(s.targets[1]);
            nextProcessorLookup = nowNs() + 1000000000ull;
        }
        for (unsigned t = 0; t < kThreads; t++) {
            if (s.targets[t].port == MACH_PORT_NULL) {
                continue;
            }
            sample.ns = nowNs();
            sample.thread = static_cast<std::uint8_t>(t);
            if (!capture(s.targets[t], sample)) {
                continue;
            }
            std::lock_guard<std::mutex> lock(s.ringLock);
            s.ring[s.next] = sample;
            s.next = (s.next + 1) % s.ring.size();
            ++s.samples;
        }
    }
}

std::string symbolize(std::uint64_t pc, std::unordered_map<std::uint64_t, std::string>& cache) {
    const auto it = cache.find(pc);
    if (it != cache.end()) {
        return it->second;
    }
    std::string name;
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(pc - 1), &info) != 0 && info.dli_sname != nullptr) {
        int status = 0;
        char* demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
        name = status == 0 && demangled != nullptr ? demangled : info.dli_sname;
        std::free(demangled);
        if (name.size() > 160) {
            name = name.substr(0, 157) + "...";
        }
    } else {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(pc));
        name = buffer;
    }
    cache.emplace(pc, name);
    return name;
}

}  // namespace

bool startFromEnvironment() {
    const char* path = std::getenv("PETARI_SPIKE_PROFILE");
    if (path == nullptr || path[0] == '\0' || gState != nullptr) {
        return gState != nullptr;
    }
    gState = new State;
    gState->path = path;
    if (const char* period = std::getenv("PETARI_SPIKE_PROFILE_US")) {
        const long value = std::strtol(period, nullptr, 10);
        if (value >= 200 && value <= 100000) {
            gState->periodUs = static_cast<unsigned>(value);
        }
    }
    gState->ring.resize(kRingSamples);
    bindTarget(gState->targets[0], pthread_self(), mach_thread_self());
    std::thread(samplerMain).detach();
    std::fprintf(stderr, "Petari spike profiler: sampling the game thread and GX processor every %u us into %s\n",
                 gState->periodUs, path);
    return true;
}

bool enabled() {
    return gState != nullptr;
}

void keepSpike(std::uint64_t frameIndex, double intervalMs, std::uint64_t startNs, std::uint64_t endNs) {
    if (gState == nullptr) {
        return;
    }
    State& s = *gState;
    if (s.spikes.size() >= kMaxSpikes) {
        ++s.droppedSpikes;
        return;
    }
    Spike spike{frameIndex, intervalMs, {}};
    {
        std::lock_guard<std::mutex> lock(s.ringLock);
        for (const Sample& sample : s.ring) {
            if (sample.depth != 0 && sample.ns >= startNs && sample.ns <= endNs &&
                spike.samples.size() < kMaxSpikeSamples) {
                spike.samples.push_back(sample);
            }
        }
    }
    std::sort(spike.samples.begin(), spike.samples.end(),
              [](const Sample& a, const Sample& b) { return a.ns < b.ns; });
    s.spikes.push_back(std::move(spike));
}

void writeReport() {
    if (gState == nullptr || gState->stop.exchange(true)) {
        return;
    }
    State& s = *gState;
    std::FILE* out = std::fopen(s.path.c_str(), "w");
    if (out == nullptr) {
        std::fprintf(stderr, "Petari spike profiler: cannot write %s\n", s.path.c_str());
        return;
    }
    std::unordered_map<std::uint64_t, std::string> symbols;
    std::fprintf(out, "Petari spike profile: %zu spikes kept (%llu dropped), sample period %u us, %llu samples taken\n",
                 s.spikes.size(), static_cast<unsigned long long>(s.droppedSpikes), s.periodUs,
                 static_cast<unsigned long long>(s.samples));
    for (const Spike& spike : s.spikes) {
        std::fprintf(out, "\n=== frame %llu: %.2f ms ===\n", static_cast<unsigned long long>(spike.frame), spike.intervalMs);
        for (unsigned t = 0; t < kThreads; t++) {
            // Inclusive time per function (samples whose stack contains it), and
            // the most common stacks, leaf first.
            std::map<std::string, unsigned> inclusive;
            std::map<std::string, unsigned> stacks;
            unsigned total = 0;
            for (const Sample& sample : spike.samples) {
                if (sample.thread != t) {
                    continue;
                }
                ++total;
                std::vector<std::string> seen;
                std::string stack;
                for (unsigned d = 0; d < sample.depth; d++) {
                    const std::string name = symbolize(sample.pcs[d], symbols);
                    if (std::find(seen.begin(), seen.end(), name) == seen.end()) {
                        seen.push_back(name);
                        ++inclusive[name];
                    }
                    if (d < 12) {
                        stack += (d ? " <- " : "") + name;
                    }
                }
                ++stacks[stack];
            }
            std::fprintf(out, "-- %s: %u samples\n", kThreadNames[t], total);
            if (total == 0) {
                continue;
            }
            std::vector<std::pair<unsigned, std::string>> byCount;
            for (const auto& [name, count] : inclusive) {
                byCount.emplace_back(count, name);
            }
            std::sort(byCount.rbegin(), byCount.rend());
            std::fprintf(out, "   inclusive (samples, %%):\n");
            for (std::size_t i = 0; i < byCount.size() && i < 30; i++) {
                std::fprintf(out, "   %5u %5.1f%%  %s\n", byCount[i].first, 100.0 * byCount[i].first / total,
                             byCount[i].second.c_str());
            }
            std::vector<std::pair<unsigned, std::string>> topStacks;
            for (const auto& [stack, count] : stacks) {
                topStacks.emplace_back(count, stack);
            }
            std::sort(topStacks.rbegin(), topStacks.rend());
            std::fprintf(out, "   top stacks (leaf first):\n");
            for (std::size_t i = 0; i < topStacks.size() && i < 8; i++) {
                std::fprintf(out, "   %5u  %s\n", topStacks[i].first, topStacks[i].second.c_str());
            }
        }
    }
    std::fclose(out);
    std::fprintf(stderr, "Petari spike profiler: %zu spikes written to %s\n", s.spikes.size(), s.path.c_str());
}

#else

bool startFromEnvironment() {
    return false;
}
bool enabled() {
    return false;
}
void keepSpike(std::uint64_t, double, std::uint64_t, std::uint64_t) {}
void writeReport() {}

#endif

}  // namespace PetariNative::App::SpikeProfiler
