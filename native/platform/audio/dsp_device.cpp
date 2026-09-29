// Native audio DSP device: mailbox, task boot, and the command/render state
// machine of the JAudio2 microcode (Zelda ucode family, default protocol,
// flags NO_ARAM | MAKE_DOLBY_LOUDER | COMBINED_CMD_0D). Replaces
// src/RVL_SDK/dsp/{dsp,dsp_task}.c.
//
// Provenance: the protocol (mail states, command numbers and arguments, sync
// mails, acknowledgement mails, frame completion) follows the behaviour of
// Dolphin's ZeldaUCode (Source/Core/Core/HW/DSPHLE/UCodes/Zelda.cpp, revision
// 5102a0339c2177575378107b76541e47cc52122d), reimplemented here; no Dolphin
// code is copied.

#include <revolution/dsp.h>
#include <revolution/os.h>

#include <dispatch/dispatch.h>
#include <pthread.h>
#include <pthread/qos.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>

#include "dsp_renderer.hpp"
#include "os_internal.hpp"
#include "petari/host_allocation.hpp"
#include "petari/platform/aram.hpp"
#include "petari/platform/dsp.hpp"

// Defined by JAudio2 (osdsp_task.cpp), replacing the SDK's handler as on the Wii.
extern "C" void __DSPHandler(__OSInterrupt interrupt, OSContext* context);

extern "C" {
DSPTaskInfo* __DSP_first_task;
DSPTaskInfo* __DSP_curr_task;
}

namespace OS = PetariNative::Platform::OS;

namespace PetariNative::Platform::DSP {
namespace {

constexpr std::uint32_t kSmgMicrocode = 0xD643001F;

// DSP -> CPU task mails.
constexpr std::uint32_t kDspInit = 0xDCD10000;
constexpr std::uint32_t kDspSync = 0xDCD10004;
constexpr std::uint32_t kDspFrameEnd = 0xDCD10005;
// CPU -> DSP task mails.
constexpr std::uint32_t kTaskMailMask = 0xFFFF0000;
constexpr std::uint32_t kTaskMailToDsp = 0xCDD10000;
constexpr std::uint32_t kMailResume = 0xCDD10000;
constexpr std::uint32_t kMailNewUcode = 0xCDD10001;
constexpr std::uint32_t kMailReset = 0xCDD10002;
constexpr std::uint32_t kMailContinue = 0xCDD10003;

constexpr int kMaxHandles = 64;
constexpr int kVoiceGroups = 4;  // 64 voices, synced in groups of 16

enum class MailState { Waiting, Rendering, WritingCommand, Halted };

struct Device {
    std::mutex lock;  // mail queues, handles
    std::condition_variable toDspChanged;
    std::deque<std::uint32_t> toDsp;
    std::deque<std::uint32_t> fromDsp;
    std::array<std::uintptr_t, kMaxHandles> handles{};
    int handleCount = 0;

    bool initialized = false;
    bool booted = false;
    bool stop = false;
    pthread_t worker{};
    pthread_t interruptThread{};
    dispatch_semaphore_t interruptSignal = nullptr;
    std::atomic<int> pendingInterrupts{0};
    std::atomic<bool> stopInterrupts{false};

    // Worker-thread state (the microcode's).
    MailState state = MailState::Waiting;
    std::uint32_t expectedCommandMails = 0;
    std::deque<std::uint32_t> commandBuffer;
    std::uint32_t pendingCommands = 0;
    bool canExecute = true;
    std::uint16_t voicesPerFrame = 0;
    std::uint8_t* voiceBase = nullptr;
    std::uint32_t requestedFrames = 0;
    std::uint32_t currentFrame = 0;
    std::uint32_t currentVoice = 0;
    std::uint32_t syncMaxVoice = 0;
    std::array<std::uint16_t, kVoiceGroups> syncFlags{};
    std::uint16_t outputVolume = 0;
    std::int16_t* outLeft = nullptr;
    std::int16_t* outRight = nullptr;
    Renderer renderer;
    Tables tables;
};

Device& device() {
    static Device* instance = [] {
        PetariNative::HostAllocationScope hostAllocations;  // first use may be on a game thread
        return new Device;
    }();
    return *instance;
}

void* resolve(Device& d, std::uint32_t handle) {
    std::lock_guard<std::mutex> guard(d.lock);
    if (handle == 0 || handle > static_cast<std::uint32_t>(d.handleCount)) {
        OSPanic(__FILE__, __LINE__, "DSP: mail carries address %08x that was not registered with addressHandle()", handle);
    }
    return reinterpret_cast<void*>(d.handles[handle - 1]);
}

// DSP -> CPU. interrupts is the number of interrupting mails among them; all
// are queued before any interrupt is raised, so the interrupt thread services
// them back to back.
void pushMails(Device& d, std::initializer_list<std::uint32_t> mails, int interrupts) {
    {
        PetariNative::HostAllocationScope hostAllocations;
        std::lock_guard<std::mutex> guard(d.lock);
        for (std::uint32_t m : mails) {
            d.fromDsp.push_back(m);
        }
    }
    if (interrupts > 0) {
        d.pendingInterrupts.fetch_add(interrupts, std::memory_order_acq_rel);
        dispatch_semaphore_signal(d.interruptSignal);
    }
}

void ackStandard(Device& d, std::uint16_t sync) {
    pushMails(d, {kDspSync, 0xF3550000u | sync}, 1);
}

bool renderingInProgress(const Device& d) {
    return d.currentFrame < d.requestedFrames;
}

std::uint32_t readCommand(Device& d) {
    if (d.commandBuffer.empty()) {
        OSPanic(__FILE__, __LINE__, "DSP: command read past the queued command mails");
    }
    const std::uint32_t v = d.commandBuffer.front();
    d.commandBuffer.pop_front();
    return v;
}

void renderAudio(Device& d) {
    while (renderingInProgress(d)) {
        if (d.currentVoice == 0) {
            d.renderer.prepareFrame();
        }
        const std::uint32_t voices = std::min<std::uint32_t>(d.voicesPerFrame, kVoiceGroups * 16);
        while (d.currentVoice < voices) {
            if (d.currentVoice >= d.syncMaxVoice) {
                return;  // wait for the next group to be released
            }
            const std::uint16_t flags = d.syncFlags[d.currentVoice >> 4];
            if (flags & (1u << (15 - (d.currentVoice & 15)))) {
                d.renderer.addVoice(d.voiceBase + d.currentVoice * kVoiceBlockSize);
            }
            ++d.currentVoice;
        }
        // The output is complete before the subframe is acknowledged. After
        // the last subframe the frame-end mail is posted together with the
        // acknowledgement: on the console that interrupt is serviced (and
        // answered with MAIL_CONTINUE) before the audio thread runs again and
        // releases the next frame's voices.
        d.renderer.finalizeFrame(d.outLeft, d.outRight, d.outputVolume);
        d.outLeft += kSubframeSamples;
        d.outRight += kSubframeSamples;
        const std::uint16_t sync = static_cast<std::uint16_t>(0xFF00 | d.currentFrame);
        d.currentVoice = 0;
        d.syncMaxVoice = 0;
        ++d.currentFrame;
        if (renderingInProgress(d)) {
            ackStandard(d, sync);
        } else {
            d.canExecute = false;  // until the CPU answers with MAIL_CONTINUE
            pushMails(d, {kDspSync, 0xF3550000u | sync, kDspFrameEnd}, 2);
        }
    }
}

void runPendingCommands(Device& d) {
    if (renderingInProgress(d) || !d.canExecute) {
        return;
    }
    while (d.pendingCommands) {
        const std::uint32_t mail = readCommand(d);
        if ((mail & 0x80000000) == 0) {
            continue;
        }
        const std::uint32_t command = (mail >> 24) & 0x7F;
        const std::uint16_t sync = static_cast<std::uint16_t>(mail >> 16);
        const std::uint16_t extra = static_cast<std::uint16_t>(mail & 0xFFFF);
        --d.pendingCommands;
        switch (command) {
        case 0x00:
        case 0x03:
        case 0x0A:
        case 0x0B:
        case 0x0C:
        case 0x0D:
        case 0x0F:
            ackStandard(d, sync);
            break;
        case 0x01: {  // setup: voice blocks, resampling/pattern/sine tables, AFC table, reverb blocks
            d.voicesPerFrame = extra;
            d.voiceBase = static_cast<std::uint8_t*>(resolve(d, readCommand(d)));
            const auto* resFilter = static_cast<const std::uint32_t*>(resolve(d, readCommand(d)));
            const auto* afc = static_cast<const std::uint8_t*>(resolve(d, readCommand(d)));
            d.tables.load(resFilter, afc);
            d.renderer.setTables(d.tables);
            d.renderer.setReverbBlocks(static_cast<ReverbBlock*>(resolve(d, readCommand(d))));
            ackStandard(d, sync);
            break;
        }
        case 0x02:  // render frames; acknowledged by sync mails, not a command ack
            d.requestedFrames = (mail >> 16) & 0xFF;
            d.outputVolume = extra;
            d.outLeft = static_cast<std::int16_t*>(resolve(d, readCommand(d)));
            d.outRight = static_cast<std::int16_t*>(resolve(d, readCommand(d)));
            readCommand(d);  // COMBINED_CMD_0D: two command-0D arguments, unused
            readCommand(d);
            d.currentFrame = 0;
            d.currentVoice = 0;
            renderAudio(d);
            return;
        case 0x0E: {  // ARAM base (Wii: MEM2 alternate ARAM)
            auto* base = static_cast<const std::uint8_t*>(resolve(d, readCommand(d)));
            if (reinterpret_cast<std::uintptr_t>(base) != Platform::ARAM::base()) {
                OSPanic(__FILE__, __LINE__, "DSP: VARAM %p is not the initialised ARAM base", base);
            }
            d.renderer.setAram(base, Platform::ARAM::size());
            ackStandard(d, sync);
            break;
        }
        default:
            OSReport("DSP: command %02x halts the microcode\n", command);
            d.state = MailState::Halted;
            return;
        }
    }
}

void handleMail(Device& d, std::uint32_t mail) {
    switch (d.state) {
    case MailState::Waiting:
        if (mail & 0x80000000) {
            mail = kTaskMailToDsp | (mail & ~kTaskMailMask);
            switch (mail) {
            case kMailContinue:
                d.canExecute = true;
                runPendingCommands(d);
                break;
            case kMailNewUcode:
            case kMailReset:
                OSPanic(__FILE__, __LINE__, "DSP: microcode replacement/reset (%08x) is not supported natively", mail);
                break;
            case kMailResume:
            default:
                d.state = MailState::Halted;
                break;
            }
        } else if ((mail & 0xFFFF) == 0) {
            d.state = renderingInProgress(d) ? MailState::Rendering : MailState::Halted;
        } else {
            d.state = MailState::WritingCommand;
            d.expectedCommandMails = mail & 0xFFFF;
        }
        break;
    case MailState::Rendering: {
        const std::uint32_t group = (mail >> 16) & 0xFF;
        if (group >= kVoiceGroups) {
            OSPanic(__FILE__, __LINE__, "DSP: voice group %u out of range", group);
        }
        d.syncMaxVoice = (((mail >> 16) & 0xF) + 1) << 4;
        d.syncFlags[group] = static_cast<std::uint16_t>(mail & 0xFFFF);
        renderAudio(d);
        d.state = MailState::Waiting;
        break;
    }
    case MailState::WritingCommand:
        d.commandBuffer.push_back(mail);
        if (--d.expectedCommandMails == 0) {
            ++d.pendingCommands;
            d.state = MailState::Waiting;
            runPendingCommands(d);
        }
        break;
    case MailState::Halted:
        break;
    }
}

void* workerMain(void*) {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);  // renders each audio frame on demand
    PetariNative::HostAllocationScope hostAllocations;
    Device& d = device();
    std::unique_lock<std::mutex> lock(d.lock);
    while (true) {
        d.toDspChanged.wait(lock, [&] { return d.stop || !d.toDsp.empty(); });
        if (d.stop) {
            return nullptr;
        }
        const std::uint32_t mail = d.toDsp.front();
        d.toDsp.pop_front();
        lock.unlock();
        handleMail(d, mail);
        lock.lock();
    }
}

void* interruptMain(void*) {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    Device& d = device();
    while (true) {
        dispatch_semaphore_wait(d.interruptSignal, DISPATCH_TIME_FOREVER);
        if (d.stopInterrupts.load(std::memory_order_acquire)) {
            return nullptr;
        }
        // Pending DSP interrupts are all serviced before thread code resumes.
        BOOL enabled = OSDisableInterrupts();
        while (!d.stopInterrupts.load(std::memory_order_acquire) && d.pendingInterrupts.load(std::memory_order_acquire) > 0) {
            d.pendingInterrupts.fetch_sub(1, std::memory_order_acq_rel);
            __DSPHandler(__OS_INTERRUPT_DSP_DSP, nullptr);
        }
        OSRestoreInterrupts(enabled);
    }
}

}  // namespace

std::uint32_t addressHandle(std::uintptr_t address) {
    if (address == 0) {
        return 0;  // "no buffer" stays zero
    }
    Device& d = device();
    std::lock_guard<std::mutex> guard(d.lock);
    for (int i = 0; i < d.handleCount; ++i) {
        if (d.handles[i] == address) {
            return static_cast<std::uint32_t>(i + 1);
        }
    }
    if (d.handleCount == kMaxHandles) {
        OSPanic(__FILE__, __LINE__, "DSP: more than %d addresses registered", kMaxHandles);
    }
    d.handles[d.handleCount] = address;
    return static_cast<std::uint32_t>(++d.handleCount);
}

std::uint32_t microcodeHash(const std::uint16_t* iram, std::uint32_t lengthBytes) {
    std::uint32_t crc = 0;
    for (std::uint32_t i = 0; i < lengthBytes; ++i) {
        const std::uint16_t word = iram[i / 2];
        const std::uint8_t byte = (i & 1) ? static_cast<std::uint8_t>(word) : static_cast<std::uint8_t>(word >> 8);
        crc ^= byte;
        crc = (crc << 3) | (crc >> 29);
    }
    return crc;
}

void shutdown() {
    Device& d = device();
    bool running;
    {
        std::lock_guard<std::mutex> guard(d.lock);
        running = d.initialized;
        d.stop = true;
    }
    d.toDspChanged.notify_all();
    if (running) {
        pthread_join(d.worker, nullptr);
        d.stopInterrupts.store(true, std::memory_order_release);
        dispatch_semaphore_signal(d.interruptSignal);
        pthread_join(d.interruptThread, nullptr);
    }
    PetariNative::HostAllocationScope hostAllocations;
    std::lock_guard<std::mutex> guard(d.lock);
    d.toDsp.clear();
    d.fromDsp.clear();
    d.handleCount = 0;
    d.initialized = false;
    d.booted = false;
    d.stop = false;
    d.stopInterrupts.store(false);
    d.pendingInterrupts.store(0);
    d.state = MailState::Waiting;
    d.expectedCommandMails = 0;
    d.commandBuffer.clear();
    d.pendingCommands = 0;
    d.canExecute = true;
    d.requestedFrames = d.currentFrame = d.currentVoice = d.syncMaxVoice = 0;
    d.syncFlags.fill(0);
    d.renderer = Renderer{};
    __DSP_first_task = nullptr;
    __DSP_curr_task = nullptr;
}

}  // namespace PetariNative::Platform::DSP

using namespace PetariNative::Platform::DSP;

extern "C" {

u32 PetariNativeDspAddressHandle(uintptr_t address) {
    return addressHandle(address);
}

void DSPInit(void) {
    Device& d = device();
    std::lock_guard<std::mutex> guard(d.lock);
    if (d.initialized) {
        return;
    }
    PetariNative::HostAllocationScope hostAllocations;
    if (d.interruptSignal == nullptr) {
        d.interruptSignal = dispatch_semaphore_create(0);
    }
    d.initialized = true;
    if (pthread_create(&d.worker, nullptr, workerMain, nullptr) != 0 ||
        pthread_create(&d.interruptThread, nullptr, interruptMain, nullptr) != 0) {
        OS::fatal("cannot start the DSP threads");
    }
}

// Mails are queued as they are written, so the CPU never waits for the DSP to
// take one; the order is preserved.
u32 DSPCheckMailToDSP(void) {
    return 0;
}

void DSPSendMailToDSP(u32 mail) {
    Device& d = device();
    {
        // Game thread; the DSP worker frees the deque's blocks.
        PetariNative::HostAllocationScope hostAllocations;
        std::lock_guard<std::mutex> guard(d.lock);
        d.toDsp.push_back(mail);
    }
    d.toDspChanged.notify_one();
}

// The DSP consumes mails as soon as they are queued; the interrupt that
// tells it to look is implicit.
void DSPAssertInt(void) {}

u32 DSPCheckMailFromDSP(void) {
    Device& d = device();
    std::lock_guard<std::mutex> guard(d.lock);
    return d.fromDsp.empty() ? 0 : 1;
}

u32 DSPReadMailFromDSP(void) {
    Device& d = device();
    std::lock_guard<std::mutex> guard(d.lock);
    if (d.fromDsp.empty()) {
        OSPanic(__FILE__, __LINE__, "DSPReadMailFromDSP() with no mail from the DSP");
    }
    const u32 mail = d.fromDsp.front();
    d.fromDsp.pop_front();
    return mail;
}

// Boots a task's microcode. Only the JAudio2 microcode used by the game is
// known natively; anything else is refused.
void __DSP_boot_task(DSPTaskInfo* task) {
    Device& d = device();
    if (!d.initialized) {
        OSPanic(__FILE__, __LINE__, "__DSP_boot_task() before DSPInit()");
    }
    const std::uint32_t hash = microcodeHash(task->iram_mmem_addr, task->iram_length);
    if (hash != kSmgMicrocode) {
        OSPanic(__FILE__, __LINE__, "DSP microcode %08x has no native implementation (expected %08x)", hash, kSmgMicrocode);
    }
    __DSP_curr_task = task;
    d.booted = true;
    pushMails(d, {kDspInit, 0xF3551111}, 1);  // init + handshake
}

void __DSP_exec_task(DSPTaskInfo*, DSPTaskInfo*) {
    OSPanic(__FILE__, __LINE__, "switching DSP tasks is not supported natively (single audio microcode)");
}

void __DSP_remove_task(DSPTaskInfo*) {
    OSPanic(__FILE__, __LINE__, "removing DSP tasks is not supported natively (single audio microcode)");
}

}  // extern "C"
