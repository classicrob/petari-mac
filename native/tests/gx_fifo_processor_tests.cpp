// The patched Aurora FIFO (patch_aurora_sync.py's fifo.cpp, compiled as
// generated) with a fake command processor (gx_fifo_processor_fake.cpp) whose
// draws block like first-use pipeline compiles.
//
// Observatory run 5 stopped every game thread (JAudio2's audio thread missed
// ~57 DMA blocks a second) while the processor compiled pipelines. The
// processor held sBufferMutex across each whole batch, and a game thread that
// grew the FIFO buffer waited for that lock in host code while holding the OS
// CPU, so no other game thread could run until the batch ended. Here a
// priority-2 "audio" thread is woken every 2 ms by a host "DMA interrupt"
// thread while the main game thread writes enough to grow the buffer during a
// one-second compile burst: the write must not wait for the batch, and the
// audio thread must keep running.

#include <revolution/os.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

extern "C" {
void __OSThreadInit(void);
void fake_fifo_init(void);
void fake_fifo_shutdown(void);
void fake_fifo_write(const void* data, uint32_t size);
void fake_fifo_publish_all(void);
void fake_fifo_drain(void);
uint64_t fake_fifo_processed(void);
uint64_t fake_fifo_written(void);
void fake_set_compile_ms(int ms);
int fake_compiles(void);
uint64_t fake_data_sum(void);
uint64_t fake_data_bytes(void);
}

namespace {

int checks = 0;
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        std::exit(1);
    }
}

using Clock = std::chrono::steady_clock;
std::int64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
}

// ---- A high-priority game thread with a deadline, fed by a host interrupt ----

OSMessageQueue gAudioQueue;
OSMessage gAudioSlots[64];
std::atomic<std::int64_t> gLastWakeUs{0};
std::atomic<std::int64_t> gMaxGapUs{0};
std::atomic<int> gWakes{0};
std::atomic<bool> gStopInterrupts{false};

void* audioThread(void*) {
    while (true) {
        OSMessage msg;
        OSReceiveMessage(&gAudioQueue, &msg, OS_MESSAGE_BLOCK);
        if (msg != nullptr) {
            return nullptr;
        }
        const std::int64_t now = nowUs();
        const std::int64_t gap = now - gLastWakeUs.exchange(now);
        if (gap > gMaxGapUs.load()) {
            gMaxGapUs = gap;
        }
        ++gWakes;
    }
}

void resetGaps() {
    gLastWakeUs = nowUs();
    gMaxGapUs = 0;
    gWakes = 0;
}

void sleepMs(int ms) {
    OSSleepTicks(OSMillisecondsToTicks(static_cast<OSTime>(ms)));  // yields the CPU
}

// Data bytes (never 0xC0 or 0x61), and their expected sum.
std::uint64_t gExpectedSum = 0;
std::uint64_t gExpectedBytes = 0;
void writeData(std::size_t count) {
    std::vector<std::uint8_t> chunk(4096);
    while (count > 0) {
        const std::size_t n = count < chunk.size() ? count : chunk.size();
        for (std::size_t i = 0; i < n; ++i) {
            chunk[i] = static_cast<std::uint8_t>((gExpectedBytes + i) % 15 + 1);
            gExpectedSum += chunk[i];
        }
        gExpectedBytes += n;
        fake_fifo_write(chunk.data(), static_cast<std::uint32_t>(n));
        count -= n;
    }
}

void testCompileBurstKeepsGameThreadsRunning() {
    constexpr int kCompiles = 4;
    constexpr int kCompileMs = 250;
    fake_set_compile_ms(kCompileMs);
    writeData(16);
    const std::uint8_t draws[kCompiles] = {0xC0, 0xC0, 0xC0, 0xC0};
    fake_fifo_write(draws, sizeof(draws));
    fake_fifo_publish_all();
    sleepMs(20);  // the processor is now inside the batch, compiling
    check(fake_compiles() == 0 && fake_fifo_processed() < fake_fifo_written(), "the processor is inside the batch");

    resetGaps();
    const auto start = Clock::now();
    writeData(1 << 20);  // grows the 64 KiB buffer several times
    const double writeMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    fake_fifo_drain();  // yields the CPU until everything is processed
    const double burstMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    const double maxGapMs = gMaxGapUs.load() / 1000.0;
    std::printf("write %.1f ms, burst %.1f ms, audio wakes %d, largest audio gap %.1f ms\n", writeMs, burstMs,
                gWakes.load(), maxGapMs);

    check(fake_compiles() == kCompiles, "every compile ran");
    check(burstMs >= kCompiles * kCompileMs - 30, "the burst lasted the compile time");
    check(writeMs < 150.0, "a game thread's FIFO write does not wait for the batch being processed");
    check(maxGapMs < 150.0 && gWakes.load() > 100, "the audio thread kept running during the compile burst");
    check(fake_data_bytes() == gExpectedBytes && fake_data_sum() == gExpectedSum,
          "the processor saw exactly the bytes written, across buffer growth");
    check(fake_fifo_processed() == fake_fifo_written(), "drain processed everything");
}

// After drain rebased the buffer, positions and content stay consistent.
void testAfterRebase() {
    fake_set_compile_ms(1);
    for (int round = 0; round < 20; ++round) {
        writeData(100000 + round * 7919);
        const std::uint8_t draw = 0xC0;
        fake_fifo_write(&draw, 1);
        if (round % 3 == 0) {
            fake_fifo_publish_all();  // processing overlaps the next writes
        }
    }
    fake_fifo_drain();
    check(fake_data_bytes() == gExpectedBytes && fake_data_sum() == gExpectedSum,
          "content intact over rebases and overlapping batches");
}

}  // namespace

int main() {
    __OSThreadInit();
    OSInitMessageQueue(&gAudioQueue, gAudioSlots, 64);
    static OSThread audio;
    alignas(32) static std::uint8_t stack[0x8000];
    OSCreateThread(&audio, audioThread, nullptr, stack + sizeof(stack), sizeof(stack), 2, 0);
    OSResumeThread(&audio);
    std::thread interrupts([] {
        while (!gStopInterrupts.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            OSSendMessage(&gAudioQueue, nullptr, OS_MESSAGE_NOBLOCK);  // as a DMA interrupt handler does
        }
    });
    fake_fifo_init();

    testCompileBurstKeepsGameThreadsRunning();
    testAfterRebase();

    gStopInterrupts = true;
    interrupts.join();
    OSSendMessage(&gAudioQueue, reinterpret_cast<OSMessage>(1), OS_MESSAGE_BLOCK);
    OSJoinThread(&audio, nullptr);
    fake_fifo_shutdown();
    OSReport("GX FIFO processor tests passed (%d checks)\n", checks);
    return 0;
}
