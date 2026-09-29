#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/main.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

extern "C" void petari_probe_draw(unsigned frame);
extern "C" bool petari_probe_check_draws();
extern "C" bool petari_probe_check_sync();
extern "C" bool petari_probe_check_readback(unsigned frame);
extern "C" bool petari_probe_load_model(const char* path);
extern "C" void petari_probe_draw_model(unsigned frame);
extern "C" void petari_probe_disable_window_restoration();
extern "C" void petari_probe_start_watchdog();
extern "C" void petari_probe_watchdog_progress();
extern "C" void petari_probe_stop_watchdog();
extern "C" void petari_probe_init_heaps();
extern "C" void petari_probe_check_heap(unsigned frame);
extern "C" void petari_probe_finish_heaps();
extern "C" void petari_probe_init_vi();
extern "C" unsigned petari_probe_wait_vi();
extern "C" void petari_shutdown_vi_renderer();

static void logMessage(AuroraLogLevel level, const char* module, const char* message, unsigned int length) {
    std::fprintf(stderr, "[%s] %.*s\n", module, static_cast<int>(length), message);
    if (level == LOG_FATAL) std::abort();
}

int main(int argc, char** argv) {
    unsigned frameLimit = 0;
    const char* modelPath = nullptr;
    bool readbackCheck = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            char* end;
            const auto value = std::strtoul(argv[++i], &end, 10);
            if (*end || value == 0 || value > 100000) return 2;
            frameLimit = static_cast<unsigned>(value);
        } else if (std::strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            modelPath = argv[++i];
        } else if (std::strcmp(argv[i], "--readback-check") == 0) {
            readbackCheck = true;
        } else {
            std::fprintf(stderr, "Usage: petari_gx_probe [--frames COUNT] [--model ARCHIVE] [--readback-check]\n");
            return 2;
        }
    }
    petari_probe_disable_window_restoration();
    const auto statePath = (std::filesystem::current_path() / "build/gx-probe-state").string();
    std::filesystem::create_directories(statePath);
    AuroraConfig config{};
    config.appName = "Petari - GX/Metal integration probe";
    config.userPath = statePath.c_str();
    config.cachePath = statePath.c_str();
    config.desiredBackend = BACKEND_METAL;
    config.windowWidth = 960;
    config.windowHeight = 720;
    config.vsync = true;
    config.logCallback = logMessage;
    config.logLevel = LOG_INFO;
    const auto info = aurora_initialize(1, argv, &config);
    if (!info.window || aurora_get_backend() != BACKEND_METAL) {
        std::fprintf(stderr, "Metal initialization failed\n");
        aurora_shutdown();
        return 1;
    }

    if (frameLimit) petari_probe_start_watchdog();
    petari_probe_init_vi();
    petari_probe_init_heaps();
    std::fprintf(stderr, "Probe: game heaps ready\n");
    if (modelPath && !petari_probe_load_model(modelPath)) {
        std::fprintf(stderr, "Model initialization failed\n");
        petari_probe_stop_watchdog();
        petari_shutdown_vi_renderer();
        aurora_shutdown();
        petari_probe_finish_heaps();
        return 1;
    }
    unsigned retraces = 0;
    unsigned frames = 0;
    bool exiting = false;
    while (!exiting && (!frameLimit || frames < frameLimit)) {
        if (!frames) std::fprintf(stderr, "Probe: update\n");
        for (const AuroraEvent* event = aurora_update(); event && event->type != AURORA_NONE; ++event)
            if (event->type == AURORA_EXIT) exiting = true;
        if (!frames) std::fprintf(stderr, "Probe: begin frame\n");
        if (exiting || !aurora_begin_frame()) continue;

        if (!frames) std::fprintf(stderr, "Probe: wait VI\n");
        retraces = petari_probe_wait_vi();
        if (!frames) std::fprintf(stderr, "Probe: draw\n");
        if (modelPath) petari_probe_draw_model(frames);
        else petari_probe_draw(frames);
        if (readbackCheck && !petari_probe_check_readback(frames)) std::abort();
        if (readbackCheck && frames == 0 && !petari_probe_check_sync()) std::abort();
        if (!frames) std::fprintf(stderr, "Probe: end frame\n");
        aurora_end_frame();
        ++frames;
        petari_probe_watchdog_progress();
        petari_probe_check_heap(frames);
        if (frames % 60 == 0) std::fprintf(stderr, "Probe: %u frames, scene heap intact\n", frames);
    }
    const bool drewGeometry = frames && petari_probe_check_draws();
    petari_probe_stop_watchdog();
    petari_shutdown_vi_renderer();
    aurora_shutdown();
    petari_probe_finish_heaps();
    std::printf("Native VI retraces: %u.\n", retraces);
    std::printf("Rendered %u frames through native GX/Metal (%s).\n", frames, modelPath ? "J3D model" : "quaternion triangle");
    return !drewGeometry || (frameLimit && frames < frameLimit) ? 1 : 0;
}
