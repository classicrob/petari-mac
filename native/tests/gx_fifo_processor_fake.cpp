// Aurora side of gx_fifo_processor_tests: a fake command processor for the
// patched Aurora FIFO (patch_aurora_sync.py's fifo.cpp, compiled as generated
// into the same test), and a plain C interface for the game-side test, which
// uses SDK headers that do not mix with Aurora's.
//
// Commands: 0xC0 is a draw that needs a first-use pipeline (it blocks for the
// configured compile time, as Aurora's blocking pipeline_ref does); 0x61 and a
// big-endian u32 is a BP write (draw done 0x45 and PE tokens 0x47/0x48 end the
// call, as the patched command processor does); any other byte is data, summed
// so the test can check the processor saw exactly the bytes written.

#include "command_processor.hpp"
#include "fifo.hpp"

#include "../thread.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

extern "C" {
void petari_aurora_fifo_publish_all(void);
uint64_t petari_aurora_fifo_processed_position(void);
uint64_t petari_aurora_fifo_write_position(void);
}

namespace aurora {
AuroraConfig g_config{};
void log_internal(AuroraLogLevel, const char* module, const char* message, unsigned int len) noexcept {
    std::fprintf(stderr, "[%s] %.*s\n", module, static_cast<int>(len), message);
}
namespace thread {
void set_current(const Options&) noexcept {}
}  // namespace thread
}  // namespace aurora

namespace {
std::atomic<int> gCompileMs{0};
std::atomic<int> gCompiles{0};
std::atomic<uint64_t> gDataSum{0};
std::atomic<uint64_t> gDataBytes{0};
}  // namespace

namespace aurora::gx::fifo {

uint32_t petariSyncBp = 0;
std::atomic<bool> petariAbortRequested{false};

ProcessResult process(const uint8_t* data, uint32_t size) noexcept {
    uint32_t i = 0;
    while (i < size) {
        if (i != 0 && petariAbortRequested.load(std::memory_order_acquire)) {
            return {i, false};
        }
        const uint8_t cmd = data[i];
        if (cmd == 0xC0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(gCompileMs.load()));
            ++gCompiles;
            ++i;
        } else if (cmd == 0x61) {
            const uint32_t value = uint32_t(data[i + 1]) << 24 | uint32_t(data[i + 2]) << 16 |
                                   uint32_t(data[i + 3]) << 8 | data[i + 4];
            i += 5;
            const uint32_t reg = value >> 24;
            if (reg == 0x45 || reg == 0x47 || reg == 0x48) {
                petariSyncBp = value;
                return {i, reg == 0x45};
            }
        } else {
            gDataSum += cmd;
            ++gDataBytes;
            ++i;
        }
    }
    return {size, false};
}

void clear_draw_cache() noexcept {}

}  // namespace aurora::gx::fifo

extern "C" {

void fake_fifo_init(void) { aurora::gx::fifo::init(); }
void fake_fifo_shutdown(void) { aurora::gx::fifo::shutdown(); }
void fake_fifo_write(const void* data, uint32_t size) { aurora::gx::fifo::write_data(data, size); }
void fake_fifo_publish_all(void) { petari_aurora_fifo_publish_all(); }
void fake_fifo_drain(void) { aurora::gx::fifo::drain(); }
uint64_t fake_fifo_processed(void) { return petari_aurora_fifo_processed_position(); }
uint64_t fake_fifo_written(void) { return petari_aurora_fifo_write_position(); }
void fake_set_compile_ms(int ms) { gCompileMs = ms; }
int fake_compiles(void) { return gCompiles.load(); }
uint64_t fake_data_sum(void) { return gDataSum.load(); }
uint64_t fake_data_bytes(void) { return gDataBytes.load(); }

}  // extern "C"
