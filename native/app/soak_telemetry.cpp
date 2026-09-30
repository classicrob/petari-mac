// Soak telemetry (soak_telemetry.hpp). SDK side: walks the JKR heap tree.

#include "soak_telemetry.hpp"

#include <JSystem/JKernel/JKRHeap.hpp>
#include <revolution/os.h>
#include <revolution/vi.h>

#include <dlfcn.h>
#include <pthread.h>
#include <libproc.h>
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "petari/audio_sdl.hpp"
#include "petari/host_allocation.hpp"
#include "petari/platform/audio.hpp"

// The Objective-C runtime, declared here: its headers' BOOL clashes with the SDK's.
extern "C" void* sel_registerName(const char* name);
extern "C" void objc_msgSend(void);

namespace PetariNative::App::Soak {
namespace {

using Clock = std::chrono::steady_clock;

struct HeapSummary {
    int count = 0;
    double totalFreeMb = 0, rootFreeMb = 0, rootMaxFreeMb = 0;
};

struct State {
    std::mutex lock;  // everything below
    std::FILE* csv = nullptr;
    std::FILE* heaps = nullptr;
    unsigned intervalSeconds = 10;
    Clock::time_point start;
    std::vector<float> frameMs;  // this window
    Clock::time_point lastFrame;
    bool haveLastFrame = false;
    unsigned long frame = 0;
    int cycle = 0;
    std::string phase;
    std::uint32_t retraces = 0;
    OSTime osTime = 0;
    HeapSummary heapSummary;
    Clock::time_point nextHeapSample;
};

std::atomic<bool> gActive{false};
State* gState = nullptr;

double mb(double bytes) {
    return bytes / (1024.0 * 1024.0);
}

double metalAllocatedMb() {
    using CreateDevice = void* (*)();
    static void* device = [] {
        auto create = reinterpret_cast<CreateDevice>(dlsym(RTLD_DEFAULT, "MTLCreateSystemDefaultDevice"));
        return create != nullptr ? create() : nullptr;  // retained for the process
    }();
    if (device == nullptr) {
        return -1;
    }
    static void* selector = sel_registerName("currentAllocatedSize");
    using Getter = unsigned long (*)(void*, void*);
    return mb(static_cast<double>(reinterpret_cast<Getter>(objc_msgSend)(device, selector)));
}

int threadCount() {
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
        return -1;
    }
    for (mach_msg_type_number_t i = 0; i < count; ++i) {
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), count * sizeof(thread_act_t));
    return static_cast<int>(count);
}

int fdCount() {
    const int bytes = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, nullptr, 0);
    if (bytes <= 0) {
        return -1;
    }
    std::vector<proc_fdinfo> fds(static_cast<std::size_t>(bytes) / sizeof(proc_fdinfo) + 16);
    const int used = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, fds.data(), static_cast<int>(fds.size() * sizeof(proc_fdinfo)));
    return used <= 0 ? -1 : used / static_cast<int>(sizeof(proc_fdinfo));
}

double percentile(std::vector<float>& sorted, double p) {
    if (sorted.empty()) {
        return 0;
    }
    const std::size_t index = std::min(sorted.size() - 1, static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1) + 0.5));
    return sorted[index];
}

void writeSample(State& s) {
    std::vector<float> frames;
    unsigned long frame;
    int cycle;
    std::string phase;
    std::uint32_t retraces;
    OSTime osTime;
    HeapSummary heaps;
    {
        std::lock_guard<std::mutex> guard(s.lock);
        frames.swap(s.frameMs);
        frame = s.frame;
        cycle = s.cycle;
        phase = s.phase;
        retraces = s.retraces;
        osTime = s.osTime;
        heaps = s.heapSummary;
    }
    std::sort(frames.begin(), frames.end());
    const double elapsed = std::chrono::duration<double>(Clock::now() - s.start).count();
    task_vm_info_data_t vm{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm), &count);
    malloc_statistics_t mallocStats{};
    malloc_zone_statistics(nullptr, &mallocStats);
    int over33 = 0;
    for (float ms : frames) {
        over33 += ms > 1000.0f / 30.0f;
    }
    std::fprintf(s.csv,
                 "%.1f,%lu,%d,%s,%zu,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%d,%d,%llu,%llu,%u,%.3f,%d,%.2f,%.2f,%.2f\n",
                 elapsed, frame, cycle, phase.c_str(), frames.size(), frames.size() / static_cast<double>(s.intervalSeconds),
                 percentile(frames, 0.50), percentile(frames, 0.95), percentile(frames, 0.99), frames.empty() ? 0.0 : frames.back(),
                 over33, mb(static_cast<double>(vm.phys_footprint)), mb(static_cast<double>(vm.resident_size)),
                 mb(static_cast<double>(vm.compressed)), mb(static_cast<double>(mallocStats.size_in_use)),
                 mb(static_cast<double>(mallocStats.size_allocated)), metalAllocatedMb(), threadCount(), fdCount(),
                 static_cast<unsigned long long>(Platform::Audio::replayedBlocks()),
                 static_cast<unsigned long long>(AudioSDL::submittedFrames()), retraces,
                 static_cast<double>(osTime) / static_cast<double>(OS_TIMER_CLOCK), heaps.count, heaps.totalFreeMb,
                 heaps.rootFreeMb, heaps.rootMaxFreeMb);
    std::fflush(s.csv);
}

void* samplerMain(void*) {
    HostAllocationScope hostAllocations;
    pthread_setname_np("Petari soak telemetry");
    State& s = *gState;
    auto next = Clock::now();
    while (true) {
        next += std::chrono::seconds(s.intervalSeconds);
        std::this_thread::sleep_until(next);
        writeSample(s);
    }
}

std::string fourcc(u32 type) {
    std::string out;
    for (int shift = 24; shift >= 0; shift -= 8) {
        const char c = static_cast<char>((type >> shift) & 0xFF);
        out += (c >= 32 && c < 127 && c != ',') ? c : '?';
    }
    return out;
}

// Game thread. One row per heap, depth-first from the root.
void sampleHeaps(State& s, double elapsed, int cycle, const std::string& phase) {
    JKRHeap* root = JKRHeap::getRootHeap();
    if (root == nullptr) {
        return;
    }
    HeapSummary summary;
    std::string rows;
    struct Item {
        JKRHeap* heap;
        JKRHeap* parent;
        int depth;
    };
    std::vector<Item> stack{{root, nullptr, 0}};
    while (!stack.empty() && summary.count < 512) {
        const Item item = stack.back();
        stack.pop_back();
        JKRHeap* heap = item.heap;
        const s32 totalFree = heap->getTotalFreeSize();
        const s32 maxFree = heap->getFreeSize();
        ++summary.count;
        summary.totalFreeMb += mb(totalFree);
        if (item.depth == 0) {
            summary.rootFreeMb = mb(totalFree);
            summary.rootMaxFreeMb = mb(maxFree);
        }
        char row[256];
        std::snprintf(row, sizeof(row), "%.1f,%d,%s,%d,%s,%p,%p,%u,%d,%d\n", elapsed, cycle, phase.c_str(), item.depth,
                      fourcc(heap->getHeapType()).c_str(), heap->getStartAddr(),
                      item.parent != nullptr ? item.parent->getStartAddr() : nullptr, heap->mSize, totalFree, maxFree);
        rows += row;
        for (JSUTree<JKRHeap>* child = heap->mChildTree.getFirstChild(); child != nullptr; child = child->getNextChild()) {
            stack.push_back({child->getObject(), heap, item.depth + 1});
        }
    }
    std::fputs(rows.c_str(), s.heaps);
    std::fflush(s.heaps);
    std::lock_guard<std::mutex> guard(s.lock);
    s.heapSummary = summary;
}

}  // namespace

bool startTelemetryFromEnvironment() {
    const char* path = std::getenv("PETARI_SOAK_CSV");
    if (path == nullptr || path[0] == '\0' || gState != nullptr) {
        return gState != nullptr;
    }
    HostAllocationScope hostAllocations;
    auto* s = new State;
    if (const char* interval = std::getenv("PETARI_SOAK_INTERVAL"); interval != nullptr && std::atoi(interval) > 0) {
        s->intervalSeconds = static_cast<unsigned>(std::atoi(interval));
    }
    s->csv = std::fopen(path, "w");
    s->heaps = std::fopen((std::string(path) + ".heaps.csv").c_str(), "w");
    if (s->csv == nullptr || s->heaps == nullptr) {
        std::fprintf(stderr, "PETARI SOAK: cannot write %s\n", path);
        return false;
    }
    std::fputs("t_s,frame,cycle,phase,frames,fps,ft_p50_ms,ft_p95_ms,ft_p99_ms,ft_max_ms,frames_over_33ms,footprint_mb,"
               "resident_mb,compressed_mb,malloc_in_use_mb,malloc_allocated_mb,metal_mb,threads,fds,audio_replayed,"
               "audio_submitted_frames,vi_retraces,os_time_s,heap_count,heap_total_free_mb,root_free_mb,root_max_free_mb\n",
               s->csv);
    std::fputs("t_s,cycle,phase,depth,type,start,parent_start,size,total_free,max_free\n", s->heaps);
    s->start = Clock::now();
    s->nextHeapSample = s->start;
    gState = s;
    gActive = true;
    pthread_t thread;
    if (pthread_create(&thread, nullptr, samplerMain, nullptr) == 0) {
        pthread_detach(thread);
    }
    std::fprintf(stderr, "PETARI SOAK: telemetry every %u s to %s (+ .heaps.csv)\n", s->intervalSeconds, path);
    return true;
}

bool telemetryActive() {
    return gActive.load(std::memory_order_relaxed);
}

void gameFrame(unsigned long frame, int cycle, const char* phase) {
    if (!gActive.load(std::memory_order_relaxed)) {
        return;
    }
    HostAllocationScope hostAllocations;
    State& s = *gState;
    const auto now = Clock::now();
    const std::uint32_t retraces = VIGetRetraceCount();
    const OSTime osTime = OSGetTime();
    bool heapsDue = false;
    std::string heapPhase;
    {
        std::lock_guard<std::mutex> guard(s.lock);
        if (s.haveLastFrame) {
            s.frameMs.push_back(std::chrono::duration<float, std::milli>(now - s.lastFrame).count());
        }
        s.lastFrame = now;
        s.haveLastFrame = true;
        s.frame = frame;
        s.cycle = cycle;
        s.phase = phase != nullptr ? phase : "";
        std::replace(s.phase.begin(), s.phase.end(), ',', ';');  // a CSV field
        s.retraces = retraces;
        s.osTime = osTime;
        if (now >= s.nextHeapSample) {
            s.nextHeapSample = now + std::chrono::seconds(s.intervalSeconds);
            heapsDue = true;
            heapPhase = s.phase;
        }
    }
    if (heapsDue) {
        sampleHeaps(s, std::chrono::duration<double>(now - s.start).count(), cycle, heapPhase);
    }
}

}  // namespace PetariNative::App::Soak
