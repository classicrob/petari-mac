// Native WPAD: the SDK's Wii Remote API for virtual remotes. Replaces
// src/RVL_SDK/wpad (and the WUD/BTE Bluetooth stack under it). The SDK's
// KPAD.c runs unchanged on top.
//
// Every 5 ms a report tick runs with interrupts disabled, as WPAD's
// Bluetooth receive path does on the Wii:
// 1. host requests (connect, disconnect, Nunchuk) are applied;
// 2. connection and extension changes complete, calling the connect and
//    extension callbacks;
// 3. one queued command completes (DPD, data format, LED, speaker, status),
//    calling its callback;
// 4. a queued speaker packet is delivered;
// 5. the channel's report is built and the sampling callback runs (KPAD's
//    KPADiSamplingCallback, which calls WPADRead).
// 6. activity keeps the screen from dimming, as WPADiCheckContInputs does on
//    the Wii (see noteActivity).

#include <revolution/kpad.h>
#include <revolution/os.h>
#include <revolution/sc.h>
#include <revolution/vi.h>
#include <revolution/wpad.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include "petari/host_allocation.hpp"
#include "petari/input.hpp"
#include "remote_model.hpp"

namespace PetariNative::Input {
namespace {

using Detail::Report;

static_assert(WPAD_BUTTON_A == 0x0800 && WPAD_BUTTON_B == 0x0400 && WPAD_BUTTON_PLUS == 0x0010 &&
              WPAD_BUTTON_MINUS == 0x1000 && WPAD_BUTTON_HOME == 0x8000 && WPAD_BUTTON_1 == 0x0200 &&
              WPAD_BUTTON_2 == 0x0100 && WPAD_BUTTON_UP == 0x0008 && WPAD_BUTTON_DOWN == 0x0004 &&
              WPAD_BUTTON_LEFT == 0x0001 && WPAD_BUTTON_RIGHT == 0x0002 && WPAD_BUTTON_C == 0x4000 &&
              WPAD_BUTTON_Z == 0x2000,
              "remote_model.cpp button bits");

constexpr int kChannels = WPAD_MAX_CONTROLLERS;
constexpr int kCommandCapacity = WPAD_COMMAND_CMD_MAX_LEN;
// Reports between a remote becoming available and its connect callback: the
// Bluetooth reconnection time. The game registers its callbacks after
// KPADInit, so the connection must not complete inside WPADInit.
constexpr int kConnectReports = 40;
// Reports between the connection and the extension being identified.
constexpr int kExtensionReports = 4;
constexpr u16 kNunchukButtons = WPAD_BUTTON_C | WPAD_BUTTON_Z;
constexpr u16 kDotSize = 3;
// kp_obj_interval / 0.383864, KPAD's dist_vv1 before its first reset.
constexpr float kDefaultDistanceFactor = 0.2f / 0.383864f;

enum class CommandKind : u8 { Dpd, DataFormat, Info, Led, SpeakerOn, SpeakerOff, SpeakerMute, SpeakerUnmute, SpeakerPlay, Disconnect };

struct Command {
    CommandKind kind;
    u32 argument;
    WPADCallback callback;
    WPADInfo* info;
};

union ReportBuffer {
    WPADStatus core;
    WPADFSStatus fs;
    WPADCLStatus cl;
    WPADStatusEx ex;
};

struct Channel {
    bool connected = false;
    int connectCountdown = -1;
    bool poweredOff = false;  // after WPADDisconnect, until an input reconnects it
    bool extensionPresent = false;
    int extensionCountdown = -1;
    u8 devType = WPAD_DEV_NOT_FOUND;
    u32 dataFormat = WPAD_FMT_CORE;
    u8 dpdCommand = 0;
    bool dpdEnabled = false;
    bool motor = false;
    bool speakerEnabled = false;
    bool speakerMuted = true;
    bool speakerPlaying = false;
    u8 led = 0;
    bool infoLocked = false;
    WPADConnectCallback connectCallback = nullptr;
    WPADExtensionCallback extensionCallback = nullptr;
    WPADSamplingCallback samplingCallback = nullptr;
    Command commands[kCommandCapacity];
    int commandHead = 0;
    int commandCount = 0;
    ReportBuffer report{};
    u8 stream[20];
    u16 streamLength = 0;
    bool streamPending = false;
    // KPAD's pointer calibration as of the KPAD thread's last WPADProbe
    // (see snapshotCalibration). No dots until the first one.
    Detail::PointerCalibration calibration = {0.0f, 0.0f, 0.0f, 0.0f};
};

// Device and SDK state: guarded by the interrupt lock.
Channel gChannels[kChannels];
bool gInitialized = false;
OSAlarm gAlarm;
bool gAlarmRunning = false;
u8 gSensorBarPosition = WPAD_SENSOR_BAR_POS_BOTTOM;
u8 gDpdSensitivity = 3;
u8 gSpeakerVolume = 0x58;
bool gMotorEnabled = true;
u8 gAutoSleepMinutes = 0;
WPADAlloc gAlloc = nullptr;
WPADFree gFree = nullptr;
// The thread that called WPADInit (KPADInit's caller): the only thread that
// writes KPAD's calibration.
std::thread::id gKpadThread;

// Host requests, applied at the next report tick.
std::atomic<bool> gWantConnected[kChannels] = {true, false, false, false};
std::atomic<bool> gWantNunchuk[kChannels] = {true, true, true, true};
std::atomic<bool> gRumble[kChannels] = {};
std::atomic<Clock> gClock{Clock::Alarm};
// A host input event (any key, including unbound keys and OS repeats, a mouse
// button, mouse motion) since the last report tick.
std::atomic<bool> gHostActivity{false};
// The previous report of channel 0, for the change test (interrupt lock).
Report gLastReport;

// Host input: guarded by gHostMutex. Lock order: interrupt lock, then
// gHostMutex; host event functions take only gHostMutex.
std::mutex gHostMutex;
SpeakerSink gSpeakerSink = nullptr;
void* gSpeakerUser = nullptr;

Detail::RemoteModel& model() {
    static Detail::RemoteModel* instance = [] {
        HostAllocationScope scope;
        return new Detail::RemoteModel();
    }();
    return *instance;
}

[[noreturn]] void misuse(const char* function, const char* message) {
    std::fprintf(stderr, "%s: %s\n", function, message);
    std::fflush(stderr);
    std::abort();
}

Channel& channel(const char* function, s32 chan) {
    if (chan < 0 || chan >= kChannels) {
        misuse(function, "channel out of range");
    }
    return gChannels[chan];
}

class Interrupts {
public:
    Interrupts() : mEnabled(OSDisableInterrupts()) {}
    ~Interrupts() { OSRestoreInterrupts(mEnabled); }
    Interrupts(const Interrupts&) = delete;
    Interrupts& operator=(const Interrupts&) = delete;

private:
    BOOL mEnabled;
};

bool isCoreFormat(u32 fmt) {
    return fmt == WPAD_FMT_CORE || fmt == WPAD_FMT_CORE_ACC || fmt == WPAD_FMT_CORE_ACC_DPD;
}
bool isFsFormat(u32 fmt) {
    return fmt == WPAD_FMT_FREESTYLE || fmt == WPAD_FMT_FREESTYLE_ACC || fmt == WPAD_FMT_FREESTYLE_ACC_DPD;
}
bool isClFormat(u32 fmt) {
    return fmt == WPAD_FMT_CLASSIC || fmt == WPAD_FMT_CLASSIC_ACC || fmt == WPAD_FMT_CLASSIC_ACC_DPD;
}
bool hasAcc(u32 fmt) {
    return fmt != WPAD_FMT_CORE && fmt != WPAD_FMT_FREESTYLE && fmt != WPAD_FMT_CLASSIC;
}
bool hasDpd(u32 fmt) {
    return fmt == WPAD_FMT_CORE_ACC_DPD || fmt == WPAD_FMT_FREESTYLE_ACC_DPD || fmt == WPAD_FMT_CLASSIC_ACC_DPD ||
           fmt == WPAD_FMT_CORE_ACC_DPD_FULL;
}
size_t formatSize(u32 fmt) {
    if (isCoreFormat(fmt)) {
        return sizeof(WPADStatus);
    }
    if (isFsFormat(fmt)) {
        return sizeof(WPADFSStatus);
    }
    if (isClFormat(fmt)) {
        return sizeof(WPADCLStatus);
    }
    return sizeof(WPADStatusEx);
}

s16 accCounts(float g, int unit) {
    const long counts = std::lround(g * unit);
    return static_cast<s16>(std::clamp(counts, -511L, 511L));
}

// Result of an operation that needs a remote. A virtual remote is set up as
// soon as it connects, so WPAD_ERR_BUSY for "still being set up" never arises.
s32 readiness(const Channel& ch) {
    return ch.connected ? WPAD_ERR_NONE : WPAD_ERR_NO_CONTROLLER;
}

bool pushCommand(Channel& ch, CommandKind kind, u32 argument, WPADCallback callback, WPADInfo* info = nullptr) {
    if (ch.commandCount >= kCommandCapacity) {
        return false;
    }
    Command& command = ch.commands[(ch.commandHead + ch.commandCount) % kCommandCapacity];
    command = {kind, argument, callback, info};
    ++ch.commandCount;
    return true;
}

void fillInfo(const Channel& ch, WPADInfo* info) {
    if (info == nullptr) {
        return;
    }
    info->dpd = ch.dpdEnabled;
    info->speaker = ch.speakerEnabled;
    info->attach = ch.extensionPresent;
    info->lowBat = FALSE;
    info->nearempty = FALSE;
    info->battery = WPAD_BATTERY_LEVEL_MAX;  // a keyboard and mouse have no battery to run down
    info->led = ch.led;
    info->protocol = 0;
    info->firmware = 0;
}

void resetDevice(Channel& ch) {
    ch.devType = WPAD_DEV_NOT_FOUND;
    ch.dataFormat = WPAD_FMT_CORE;
    ch.dpdCommand = 0;
    ch.dpdEnabled = false;
    ch.motor = false;
    ch.speakerEnabled = false;
    ch.speakerMuted = true;
    ch.speakerPlaying = false;
    ch.infoLocked = false;
    ch.streamPending = false;
    ch.extensionCountdown = -1;
    std::memset(&ch.report, 0, sizeof(ch.report));
}

void disconnect(s32 chan, Channel& ch) {
    // Commands still queued fail as they do when a remote drops.
    Command pending[kCommandCapacity];
    const int count = ch.commandCount;
    for (int i = 0; i < count; ++i) {
        pending[i] = ch.commands[(ch.commandHead + i) % kCommandCapacity];
    }
    ch.commandCount = 0;
    ch.connected = false;
    resetDevice(ch);
    gRumble[chan] = false;
    for (int i = 0; i < count; ++i) {
        if (pending[i].callback != nullptr) {
            pending[i].callback(chan, WPAD_ERR_NO_CONTROLLER);
        }
    }
    if (ch.connectCallback != nullptr) {
        ch.connectCallback(chan, WPAD_ERR_NO_CONTROLLER);
    }
}

void completeCommand(s32 chan, Channel& ch) {
    if (ch.commandCount == 0) {
        return;
    }
    const Command command = ch.commands[ch.commandHead];
    ch.commandHead = (ch.commandHead + 1) % kCommandCapacity;
    --ch.commandCount;

    switch (command.kind) {
    case CommandKind::Dpd:
        ch.dpdEnabled = command.argument != 0;
        break;
    case CommandKind::DataFormat:
        break;  // the format took effect when requested, as in the SDK
    case CommandKind::Info:
        fillInfo(ch, command.info);
        ch.infoLocked = false;
        break;
    case CommandKind::Led:
        ch.led = static_cast<u8>(command.argument);
        break;
    case CommandKind::SpeakerOn:
        ch.speakerEnabled = true;
        ch.speakerMuted = false;
        ch.speakerPlaying = false;
        break;
    case CommandKind::SpeakerOff:
        ch.speakerEnabled = false;
        ch.speakerPlaying = false;
        break;
    case CommandKind::SpeakerMute:
        ch.speakerMuted = true;
        break;
    case CommandKind::SpeakerUnmute:
        ch.speakerMuted = false;
        break;
    case CommandKind::SpeakerPlay:
        ch.speakerPlaying = true;
        break;
    case CommandKind::Disconnect:
        if (command.callback != nullptr) {
            command.callback(chan, WPAD_ERR_NONE);
        }
        ch.poweredOff = true;
        disconnect(chan, ch);
        return;
    }
    if (command.callback != nullptr) {
        command.callback(chan, WPAD_ERR_NONE);
    }
}

void buildReport(Channel& ch, const Report& input) {
    const u32 fmt = ch.dataFormat;
    ReportBuffer& r = ch.report;
    std::memset(&r, 0, sizeof(r));
    const bool nunchuk = ch.devType == WPAD_DEV_FREESTYLE;

    u16 buttons = input.buttons;
    if (!(nunchuk && isFsFormat(fmt))) {
        buttons &= static_cast<u16>(~kNunchukButtons);
    }
    r.core.button = buttons;
    if (hasAcc(fmt)) {
        // KPAD: acc.x = -accX, acc.y = -accZ, acc.z = accY (read_kpad_acc).
        r.core.accX = accCounts(-input.acc[0], Detail::kCoreGravityCounts);
        r.core.accY = accCounts(input.acc[2], Detail::kCoreGravityCounts);
        r.core.accZ = accCounts(-input.acc[1], Detail::kCoreGravityCounts);
    }
    if (hasDpd(fmt) && ch.dpdEnabled) {
        for (int i = 0; i < input.dotCount; ++i) {
            r.core.obj[i].x = input.dots[i].x;
            r.core.obj[i].y = input.dots[i].y;
            r.core.obj[i].size = kDotSize;
            r.core.obj[i].traceId = static_cast<u8>(i);
        }
    }
    r.core.dev = ch.devType;
    r.core.err = WPAD_ERR_NONE;
    if (nunchuk && isFsFormat(fmt)) {
        if (hasAcc(fmt)) {
            r.fs.fsAccX = accCounts(-input.nunchukAcc[0], Detail::kNunchukGravityCounts);
            r.fs.fsAccY = accCounts(input.nunchukAcc[2], Detail::kNunchukGravityCounts);
            r.fs.fsAccZ = accCounts(-input.nunchukAcc[1], Detail::kNunchukGravityCounts);
        }
        r.fs.fsStickX = input.stickX;
        r.fs.fsStickY = input.stickY;
    }
}

// KPAD owns the pointer calibration in inside_kpads, and the KPAD thread
// changes it without the interrupt lock (KPADInit after WPADInit has started
// the reports; KPADSetSensorHeight). Reading it in the report tick would race
// with those writes. KPADRead calls WPADProbe on that thread with interrupts
// disabled every frame, so the calibration is copied there, in order with the
// writes, and the tick reads the copy under the same lock. A change reaches
// the reports at the next KPADRead, before KPAD next uses the dots.
void snapshotCalibration(s32 chan, Channel& ch) {
    if (std::this_thread::get_id() != gKpadThread) {
        return;  // the report tick (KPAD's sampling callback) or another thread
    }
    const KPADInsideStatus& kp = inside_kpads[chan];
    Detail::PointerCalibration c;
    c.centerX = kp.center_org.x;
    c.centerY = kp.center_org.y;
    c.dpdToPosScale = kp.dpd2pos_scale;
    c.distanceFactor = kp.dist_vv1 > 0.0f ? kp.dist_vv1 : kDefaultDistanceFactor;
    ch.calibration = c;
}

bool sameReport(const Report& a, const Report& b) {
    if (a.buttons != b.buttons || a.stickX != b.stickX || a.stickY != b.stickY || a.dotCount != b.dotCount) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        if (a.acc[i] != b.acc[i] || a.nunchukAcc[i] != b.nunchukAcc[i]) {
            return false;
        }
    }
    for (int i = 0; i < a.dotCount; ++i) {
        if (a.dots[i].x != b.dots[i].x || a.dots[i].y != b.dots[i].y) {
            return false;
        }
    }
    return true;
}

// The screen saver. On the Wii, WPADiCheckContInputs compares each report
// with the previous one and, on any change, calls __VIResetRFIdle, so VI
// never dims while the remote is in use. Here a changed report of the virtual
// remote does the same, and so does any host input event, which also covers
// keys the bindings do not use and mouse motion over the letterbox bars.
// VIResetDimmingCount takes effect at the next retrace.
void noteActivity(const Report& report) {
    const bool changed = !sameReport(report, gLastReport);
    gLastReport = report;
    if (gHostActivity.exchange(false) || changed) {
        VIResetDimmingCount();
    }
}

void tickChannel(s32 chan) {
    Channel& ch = gChannels[chan];

    Report input;
    SpeakerSink sink = nullptr;
    void* sinkUser = nullptr;
    bool activity = false;
    {
        std::lock_guard<std::mutex> lock(gHostMutex);
        sink = gSpeakerSink;
        sinkUser = gSpeakerUser;
        if (chan == 0) {
            input = model().nextReport(ch.calibration);
            activity = model().takeActivity();
        }
    }

    if (chan == 0) {
        noteActivity(input);
    }

    if (activity && ch.poweredOff) {
        ch.poweredOff = false;  // pressing a button turns a remote back on
    }
    const bool want = gWantConnected[chan] && !ch.poweredOff;
    if (!want && (ch.connected || ch.connectCountdown >= 0)) {
        ch.connectCountdown = -1;
        if (ch.connected) {
            disconnect(chan, ch);
        }
    } else if (want && !ch.connected && ch.connectCountdown < 0) {
        ch.connectCountdown = kConnectReports;
    }

    if (ch.connectCountdown >= 0 && --ch.connectCountdown < 0) {
        resetDevice(ch);
        ch.connected = true;
        ch.devType = WPAD_DEV_CORE;
        ch.extensionPresent = false;
        if (ch.connectCallback != nullptr) {
            ch.connectCallback(chan, WPAD_ERR_NONE);
        }
    }
    if (!ch.connected) {
        return;
    }

    if (gWantNunchuk[chan] != ch.extensionPresent && ch.extensionCountdown < 0) {
        ch.extensionCountdown = kExtensionReports;
    }
    if (ch.extensionCountdown >= 0 && --ch.extensionCountdown < 0) {
        ch.extensionPresent = gWantNunchuk[chan];
        ch.devType = ch.extensionPresent ? WPAD_DEV_FREESTYLE : WPAD_DEV_CORE;
        if (ch.extensionCallback != nullptr) {
            ch.extensionCallback(chan, ch.devType);
        }
        if (!ch.connected) {
            return;
        }
    }

    completeCommand(chan, ch);
    if (!ch.connected) {
        return;
    }

    if (ch.streamPending) {
        ch.streamPending = false;
        if (sink != nullptr && ch.speakerEnabled && ch.speakerPlaying && !ch.speakerMuted) {
            sink(chan, ch.stream, ch.streamLength, sinkUser);
        }
    }

    buildReport(ch, input);
    if (ch.samplingCallback != nullptr) {
        ch.samplingCallback(chan);
    }
}

void tick() {
    for (s32 chan = 0; chan < kChannels; ++chan) {
        tickChannel(chan);
    }
}

void alarmHandler(OSAlarm*, OSContext*) {
    tick();
}

}  // namespace

// --- Host API ---

BoundState boundState() {
    std::lock_guard<std::mutex> lock(gHostMutex);
    return model().boundState();
}

void setBindings(const Bindings& value) {
    HostAllocationScope scope;
    std::lock_guard<std::mutex> lock(gHostMutex);
    model().setBindings(value);
}

Bindings bindings() {
    HostAllocationScope scope;
    std::lock_guard<std::mutex> lock(gHostMutex);
    return model().bindings();
}

void setSettings(const Settings& value) {
    std::lock_guard<std::mutex> lock(gHostMutex);
    model().setSettings(value);
}

Settings settings() {
    std::lock_guard<std::mutex> lock(gHostMutex);
    return model().settings();
}

void keyEvent(KeyCode code, bool down, bool repeat) {
    gHostActivity = true;
    std::lock_guard<std::mutex> lock(gHostMutex);
    model().keyEvent(code, down, repeat);
}

void mouseButtonEvent(MouseButton button, bool down) {
    gHostActivity = true;
    std::lock_guard<std::mutex> lock(gHostMutex);
    model().mouseButtonEvent(button, down);
}

void padButtonEvent(PadButton button, bool down) {
    gHostActivity = true;
    std::lock_guard<std::mutex> lock(gHostMutex);
    model().padButtonEvent(button, down);
}

void padAxisEvent(PadAxis axis, float value) {
    // A resting stick's noise is not activity; a deliberate push is.
    if (std::fabs(value) > 0.25f) {
        gHostActivity = true;
    }
    std::lock_guard<std::mutex> lock(gHostMutex);
    model().padAxisEvent(axis, value);
}

void padDisconnected() {
    std::lock_guard<std::mutex> lock(gHostMutex);
    model().padDisconnected();
}

void mouseMoved(float x, float y) {
    gHostActivity = true;
    std::lock_guard<std::mutex> lock(gHostMutex);
    model().mouseMoved(x, y);
}

void mouseLeft() {
    std::lock_guard<std::mutex> lock(gHostMutex);
    model().mouseLeft();
}

void setViewport(const Viewport& viewport) {
    std::lock_guard<std::mutex> lock(gHostMutex);
    model().setViewport(viewport);
}

void focusChanged(bool focused) {
    std::lock_guard<std::mutex> lock(gHostMutex);
    model().focusChanged(focused);
}

void setPosture(Posture value) {
    std::lock_guard<std::mutex> lock(gHostMutex);
    model().setPosture(value);
}

Posture posture() {
    std::lock_guard<std::mutex> lock(gHostMutex);
    return model().posture();
}

void titlePromptShown() {
    std::lock_guard<std::mutex> lock(gHostMutex);
    model().titlePromptShown();
}

bool titlePromptActive() {
    std::lock_guard<std::mutex> lock(gHostMutex);
    return model().titlePromptActive();
}

void motionControlShown(Steering steering) {
    std::lock_guard<std::mutex> lock(gHostMutex);
    model().motionControlShown(steering);
}

void setConnected(int chan, bool connected) {
    if (chan < 0 || chan >= kChannels) {
        misuse("setConnected", "channel out of range");
    }
    gWantConnected[chan] = connected;
    if (connected) {
        Interrupts guard;
        gChannels[chan].poweredOff = false;
    }
}

void setNunchukAttached(int chan, bool attached) {
    if (chan < 0 || chan >= kChannels) {
        misuse("setNunchukAttached", "channel out of range");
    }
    gWantNunchuk[chan] = attached;
}

bool rumbleActive(int chan) {
    return chan >= 0 && chan < kChannels && gRumble[chan];
}

void setSpeakerSink(SpeakerSink sink, void* user) {
    std::lock_guard<std::mutex> lock(gHostMutex);
    gSpeakerSink = sink;
    gSpeakerUser = user;
}

void setClock(Clock clock) {
    Interrupts guard;
    if (gInitialized) {
        misuse("setClock", "select the clock before WPADInit");
    }
    gClock = clock;
}

void pumpReports(int count) {
    Interrupts guard;
    if (!gInitialized) {
        misuse("pumpReports", "WPADInit has not run");
    }
    if (gClock != Clock::Manual) {
        misuse("pumpReports", "the report clock is not Clock::Manual");
    }
    for (int i = 0; i < count; ++i) {
        tick();
    }
}

void resetForTesting() {
    {
        Interrupts guard;
        if (gAlarmRunning) {
            OSCancelAlarm(&gAlarm);
            gAlarmRunning = false;
        }
        for (Channel& ch : gChannels) {
            ch = Channel{};
        }
        gInitialized = false;
        gSensorBarPosition = WPAD_SENSOR_BAR_POS_BOTTOM;
        gDpdSensitivity = 3;
        gSpeakerVolume = 0x58;
        gMotorEnabled = true;
        gAutoSleepMinutes = 0;
        gAlloc = nullptr;
        gFree = nullptr;
        gKpadThread = std::thread::id();
        for (int i = 0; i < kChannels; ++i) {
            gWantConnected[i] = i == 0;
            gWantNunchuk[i] = true;
            gRumble[i] = false;
        }
        gClock = Clock::Alarm;
        gHostActivity = false;
        gLastReport = Report();
    }
    HostAllocationScope scope;
    std::lock_guard<std::mutex> lock(gHostMutex);
    model() = Detail::RemoteModel();
    gSpeakerSink = nullptr;
    gSpeakerUser = nullptr;
}

}  // namespace PetariNative::Input

using namespace PetariNative::Input;

// --- SDK API ---

extern "C" {

void WPADInit(void) {
    Interrupts guard;
    if (gInitialized) {
        return;
    }
    gInitialized = true;
    gKpadThread = std::this_thread::get_id();
    gSensorBarPosition = SCGetWpadSensorBarPosition();
    gDpdSensitivity = static_cast<u8>(SCGetBtDpdSensibility());
    gSpeakerVolume = SCGetWpadSpeakerVolume();
    gMotorEnabled = SCGetWpadMotorMode() != 0;
    if (gClock == Clock::Alarm) {
        const OSTime period = OSMillisecondsToTicks(5);
        OSCreateAlarm(&gAlarm);
        OSSetPeriodicAlarm(&gAlarm, OSGetTime() + period, period, alarmHandler);
        gAlarmRunning = true;
    }
}

s32 WPADGetStatus(void) {
    Interrupts guard;
    return gInitialized ? WPAD_STATE_SETUP : WPAD_STATE_DISABLED;
}

void WPADRegisterAllocator(WPADAlloc alloc, WPADFree free) {
    Interrupts guard;
    gAlloc = alloc;
    gFree = free;
}

// The Wii's allocator serves the Bluetooth stack's device records. The host
// needs none.
u32 WPADGetWorkMemorySize(void) {
    return 0;
}

u8 WPADGetSensorBarPosition(void) {
    Interrupts guard;
    return gSensorBarPosition;
}

u8 WPADGetDpdSensitivity(void) {
    Interrupts guard;
    return gDpdSensitivity;
}

s32 WPADProbe(s32 chan, u32* type) {
    Interrupts guard;
    Channel& ch = channel("WPADProbe", chan);
    snapshotCalibration(chan, ch);
    if (type != nullptr) {
        *type = ch.devType;
    }
    return readiness(ch);
}

WPADSamplingCallback WPADSetSamplingCallback(s32 chan, WPADSamplingCallback callback) {
    Interrupts guard;
    Channel& ch = channel("WPADSetSamplingCallback", chan);
    const WPADSamplingCallback previous = ch.samplingCallback;
    ch.samplingCallback = callback;
    return previous;
}

WPADConnectCallback WPADSetConnectCallback(s32 chan, WPADConnectCallback callback) {
    Interrupts guard;
    Channel& ch = channel("WPADSetConnectCallback", chan);
    const WPADConnectCallback previous = ch.connectCallback;
    ch.connectCallback = callback;
    return previous;
}

WPADExtensionCallback WPADSetExtensionCallback(s32 chan, WPADExtensionCallback callback) {
    Interrupts guard;
    Channel& ch = channel("WPADSetExtensionCallback", chan);
    const WPADExtensionCallback previous = ch.extensionCallback;
    ch.extensionCallback = callback;
    return previous;
}

u32 WPADGetDataFormat(s32 chan) {
    Interrupts guard;
    return channel("WPADGetDataFormat", chan).dataFormat;
}

s32 WPADSetDataFormat(s32 chan, u32 fmt) {
    Interrupts guard;
    Channel& ch = channel("WPADSetDataFormat", chan);
    s32 result = readiness(ch);
    if (result == WPAD_ERR_NONE && ch.dataFormat != fmt) {
        if (fmt > WPAD_FMT_CORE_ACC_DPD_FULL) {
            misuse("WPADSetDataFormat", "unknown data format");
        }
        if (pushCommand(ch, CommandKind::DataFormat, fmt, nullptr)) {
            ch.dataFormat = fmt;
        } else {
            result = WPAD_ERR_BUSY;
        }
    }
    return result;
}

void WPADRead(s32 chan, void* status) {
    Interrupts guard;
    const Channel& ch = channel("WPADRead", chan);
    const size_t size = formatSize(ch.dataFormat);
    if (ch.connected) {
        std::memcpy(status, &ch.report, size);
    } else {
        std::memset(status, 0, size);
        static_cast<WPADStatus*>(status)->err = WPAD_ERR_NO_CONTROLLER;
    }
}

void WPADGetAccGravityUnit(s32 chan, u32 type, WPADAcc* acc) {
    Interrupts guard;
    channel("WPADGetAccGravityUnit", chan);
    if (acc == nullptr) {
        return;
    }
    switch (type) {
    case WPAD_DEV_CORE:
        acc->x = acc->y = acc->z = PetariNative::Input::Detail::kCoreGravityCounts;
        break;
    case WPAD_DEV_FREESTYLE:
        acc->x = acc->y = acc->z = PetariNative::Input::Detail::kNunchukGravityCounts;
        break;
    }
}

BOOL WPADIsDpdEnabled(s32 chan) {
    Interrupts guard;
    return channel("WPADIsDpdEnabled", chan).dpdEnabled;
}

s32 WPADControlDpd(s32 chan, u32 command, WPADCallback callback) {
    s32 result;
    {
        Interrupts guard;
        Channel& ch = channel("WPADControlDpd", chan);
        result = readiness(ch);
        if (result == WPAD_ERR_NONE && (command == 0 ? ch.dpdEnabled : command != ch.dpdCommand)) {
            if (pushCommand(ch, CommandKind::Dpd, command, callback)) {
                ch.dpdCommand = static_cast<u8>(command);
                return WPAD_ERR_NONE;
            }
            result = WPAD_ERR_BUSY;
        }
    }
    if (callback != nullptr) {
        callback(chan, result);
    }
    return result;
}

s32 WPADGetInfoAsync(s32 chan, WPADInfo* info, WPADCallback callback) {
    s32 result;
    {
        Interrupts guard;
        Channel& ch = channel("WPADGetInfoAsync", chan);
        result = readiness(ch);
        if (result == WPAD_ERR_NONE) {
            if (!ch.infoLocked && pushCommand(ch, CommandKind::Info, 0, callback, info)) {
                ch.infoLocked = true;
                return WPAD_ERR_NONE;
            }
            result = WPAD_ERR_BUSY;
        }
    }
    if (callback != nullptr) {
        callback(chan, result);
    }
    return result;
}

s32 WPADControlLed(s32 chan, u8 pattern, WPADCallback callback) {
    s32 result;
    {
        Interrupts guard;
        Channel& ch = channel("WPADControlLed", chan);
        result = readiness(ch);
        if (result == WPAD_ERR_NONE) {
            if (pushCommand(ch, CommandKind::Led, pattern, callback)) {
                return WPAD_ERR_NONE;
            }
            result = WPAD_ERR_BUSY;
        }
    }
    if (callback != nullptr) {
        callback(chan, result);
    }
    return result;
}

void WPADControlMotor(s32 chan, u32 command) {
    Interrupts guard;
    Channel& ch = channel("WPADControlMotor", chan);
    if (!ch.connected) {
        return;
    }
    if (!gMotorEnabled && (command != WPAD_MOTOR_STOP || !ch.motor)) {
        return;
    }
    ch.motor = command != WPAD_MOTOR_STOP;
    gRumble[chan] = ch.motor;
}

void WPADEnableMotor(BOOL enable) {
    Interrupts guard;
    gMotorEnabled = enable != FALSE;
}

BOOL WPADIsMotorEnabled(void) {
    Interrupts guard;
    return gMotorEnabled;
}

void WPADDisconnect(s32 chan) {
    Interrupts guard;
    Channel& ch = channel("WPADDisconnect", chan);
    if (ch.connected) {
        // The SDK turns the LEDs off, then closes the link.
        pushCommand(ch, CommandKind::Disconnect, 0, nullptr);
    }
}

// Remotes turn themselves off after this many idle minutes on the Wii. A
// keyboard and mouse stay connected; the setting is kept but has no effect.
void WPADSetAutoSleepTime(u8 minute) {
    Interrupts guard;
    gAutoSleepMinutes = minute;
}

BOOL WPADIsSpeakerEnabled(s32 chan) {
    Interrupts guard;
    return channel("WPADIsSpeakerEnabled", chan).speakerEnabled;
}

s32 WPADControlSpeaker(s32 chan, u32 command, WPADCallback callback) {
    s32 result;
    {
        Interrupts guard;
        Channel& ch = channel("WPADControlSpeaker", chan);
        result = readiness(ch);
        if (result == WPAD_ERR_NONE) {
            CommandKind kind;
            bool queue = true;
            switch (command) {
            case 0:
                kind = CommandKind::SpeakerOff;
                queue = ch.speakerEnabled;
                break;
            case 1:
            case 5:
                kind = CommandKind::SpeakerOn;
                break;
            case 2:
                kind = CommandKind::SpeakerMute;
                break;
            case 3:
                kind = CommandKind::SpeakerUnmute;
                break;
            case 4:
                kind = CommandKind::SpeakerPlay;
                break;
            default:
                queue = false;  // the SDK ignores other commands and reports success
                kind = CommandKind::SpeakerOff;
                break;
            }
            if (queue) {
                if (pushCommand(ch, kind, 0, callback)) {
                    return WPAD_ERR_NONE;
                }
                result = WPAD_ERR_BUSY;
            }
        }
    }
    if (callback != nullptr) {
        callback(chan, result);
    }
    return result;
}

u8 WPADGetSpeakerVolume(void) {
    Interrupts guard;
    return gSpeakerVolume;
}

void WPADSetSpeakerVolume(u8 volume) {
    Interrupts guard;
    gSpeakerVolume = std::min<u8>(volume, 127);
}

// Mii data stored in a Wii Remote's memory (RFL_Controller.c). A keyboard
// and mouse have no such memory: nothing can be read from it, so the read is
// refused at once as an invalid request and no callback follows. RFL then
// ends the controller load with RFLErrcode_Controllerfail.
s32 WPADReadFaceData(s32 chan, void* dst, u32 size, u32 src, WPADCallback callback) {
    Interrupts guard;
    const Channel& ch = channel("WPADReadFaceData", chan);
    (void)dst;
    (void)size;
    (void)src;
    (void)callback;
    if (!ch.connected) {
        return WPAD_ERR_NO_CONTROLLER;
    }
    return WPAD_ERR_INVALID;
}

BOOL WPADCanSendStreamData(s32 chan) {
    Interrupts guard;
    const Channel& ch = channel("WPADCanSendStreamData", chan);
    return ch.connected && !ch.streamPending && ch.commandCount < kCommandCapacity - 3;
}

s32 WPADSendStreamData(s32 chan, void* buffer, u16 length) {
    Interrupts guard;
    Channel& ch = channel("WPADSendStreamData", chan);
    if (!ch.connected) {
        return WPAD_ERR_NO_CONTROLLER;
    }
    if (length > sizeof(ch.stream)) {
        misuse("WPADSendStreamData", "a speaker packet holds at most 20 bytes");
    }
    if (ch.streamPending || ch.commandCount >= kCommandCapacity - 3) {
        return WPAD_ERR_BUSY;
    }
    std::memcpy(ch.stream, buffer, length);
    ch.streamLength = length;
    ch.streamPending = true;
    return WPAD_ERR_NONE;
}

}  // extern "C"
