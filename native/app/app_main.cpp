// Native entry point: window, platform start-up, then the game's main loop
// on this (the macOS main) thread. See native/app/INTEGRATION_PLAN.md.

#include <aurora/aurora.h>
#include <aurora/main.h>

#include <SDL3/SDL_init.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "host.hpp"

namespace App = PetariNative::App;

namespace {

void logMessage(AuroraLogLevel level, const char* module, const char* message, unsigned int length) {
    std::fprintf(stderr, "[%s] %.*s\n", module, static_cast<int>(length), message);
    if (level == LOG_FATAL) {
        std::abort();
    }
}

void usage() {
    std::fputs("Usage: petari [--disc DIR] [--user DIR]\n"
               "  --disc DIR  extracted disc (containing files/); default: $PETARI_GAME_DIR,\n"
               "              else build/game-data/RMGE01 under the working directory\n"
               "  --user DIR  saves, settings, controls and crash reports;\n"
               "              default: ~/Library/Application Support/Petari\n",
               stderr);
}

bool resolvePaths(int argc, char** argv, App::Paths* paths) {
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--disc") == 0 && i + 1 < argc) {
            paths->disc = argv[++i];
        } else if (std::strcmp(argv[i], "--user") == 0 && i + 1 < argc) {
            paths->user = argv[++i];
        } else {
            return false;
        }
    }
    if (paths->disc.empty()) {
        const char* env = std::getenv("PETARI_GAME_DIR");
        paths->disc = env != nullptr && env[0] != '\0' ? std::filesystem::path(env)
                                                      : std::filesystem::current_path() / "build/game-data/RMGE01";
    }
    if (paths->user.empty()) {
        const char* home = std::getenv("HOME");
        if (home == nullptr || home[0] == '\0') {
            std::fputs("petari: HOME is not set; pass --user\n", stderr);
            return false;
        }
        paths->user = std::filesystem::path(home) / "Library/Application Support/Petari";
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    App::Paths paths;
    if (!resolvePaths(argc, argv, &paths)) {
        usage();
        return 2;
    }

    std::string error;
    if (!App::Host::preparePlatform(paths, &error)) {
        std::fprintf(stderr, "petari: %s\n", error.c_str());
        return 1;
    }
    if (!App::Events::loadControls(paths.user / "controls.txt", &error)) {
        std::fprintf(stderr, "petari: controls: %s\n", error.c_str());
        return 1;
    }

    const std::string userPath = paths.user.string();
    AuroraConfig config{};
    config.appName = "Super Mario Galaxy";
    config.userPath = userPath.c_str();
    config.cachePath = userPath.c_str();
    config.desiredBackend = BACKEND_METAL;
    // The game paces itself on VI retraces; a blocking present would add a
    // second wait per frame.
    config.vsync = false;
    // The game keeps running without focus, as on the console. Input releases
    // held keys on focus loss.
    config.pauseOnFocusLost = false;
    config.windowWidth = 1280;
    config.windowHeight = 720;
    config.logCallback = logMessage;
    config.logLevel = LOG_INFO;
    const AuroraInfo info = aurora_initialize(1, argv, &config);
    if (info.window == nullptr || aurora_get_backend() != BACKEND_METAL) {
        std::fputs("petari: Metal initialization failed\n", stderr);
        aurora_shutdown();
        return 1;
    }
    // Audio opens on a game thread when the game starts AI DMA; initialize
    // SDL's audio subsystem here, on the main thread, first. The sink's own
    // initialization then only adds a reference.
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "petari: SDL audio: %s\n", SDL_GetError());
    }

    App::Seam::attach(info.window);
    App::Host::startOS();
    App::Seam::openFirstFrame();
    App::Host::runGame();
}
