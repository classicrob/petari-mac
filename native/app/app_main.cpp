// Native entry point: window, platform start-up, then the game's main loop
// on this (the macOS main) thread. See native/app/INTEGRATION_PLAN.md.

#include <aurora/aurora.h>
#include <aurora/main.h>
#include <aurora/event.h>
#include <aurora/gfx.h>

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <petari/test_fixture.hpp>

#include "host.hpp"

extern "C" void petari_gx_pipeline_background_begin();

namespace App = PetariNative::App;

namespace {

void logMessage(AuroraLogLevel level, const char* module, const char* message, unsigned int length) {
    std::fprintf(stderr, "[%s] %.*s\n", module, static_cast<int>(length), message);
    if (level == LOG_FATAL) {
        std::abort();
    }
}

void usage() {
    std::fputs("Usage: petari [--disc DIR] [--user DIR] [--test-fixture observatory|stage]\n"
               "  --disc DIR  extracted disc (containing files/); default: $PETARI_GAME_DIR,\n"
               "              else build/game-data/RMGE01 under the working directory\n"
               "  --user DIR  saves, settings, controls and crash reports;\n"
               "              default: ~/Library/Application Support/Petari\n"
               "  --test-fixture observatory  post-tutorial test progression; requires a marked isolated --user\n"
               "  --test-fixture stage  synthetic entry to $PETARI_STAGE scenario $PETARI_SCENARIO after the file\n"
               "              loads (observatory progression otherwise); requires --user marked \"stage\"\n",
               stderr);
}

// --test-fixture stage: PETARI_STAGE (a stage directory name) and PETARI_SCENARIO
// (1..8). Existence of the stage on the disc is checked once the disc is known.
bool stageFixtureFromEnvironment() {
    const char* stage = std::getenv("PETARI_STAGE");
    const char* scenario = std::getenv("PETARI_SCENARIO");
    if (stage == nullptr || stage[0] == '\0' || scenario == nullptr || scenario[0] == '\0') {
        std::fputs("petari: --test-fixture stage requires PETARI_STAGE and PETARI_SCENARIO\n", stderr);
        return false;
    }
    for (const char* c = stage; *c != '\0'; ++c) {
        if (!std::isalnum(static_cast<unsigned char>(*c))) {
            std::fprintf(stderr, "petari: PETARI_STAGE \"%s\" is not a stage name\n", stage);
            return false;
        }
    }
    char* end = nullptr;
    const long number = std::strtol(scenario, &end, 10);
    if (end == nullptr || *end != '\0' || number < 1 || number > 8) {
        std::fprintf(stderr, "petari: PETARI_SCENARIO \"%s\" is not a scenario number (1..8)\n", scenario);
        return false;
    }
    PetariNative::TestFixture::stage = stage;
    PetariNative::TestFixture::stageScenario = static_cast<int>(number);
    return true;
}

bool resolvePaths(int argc, char** argv, App::Paths* paths) {
    bool fixture = false;
    bool stageFixture = false;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--disc") == 0 && i + 1 < argc) {
            paths->disc = argv[++i];
        } else if (std::strcmp(argv[i], "--user") == 0 && i + 1 < argc) {
            paths->user = argv[++i];
        } else if (std::strcmp(argv[i], "--test-fixture") == 0 && i + 1 < argc &&
                   std::strcmp(argv[i + 1], "observatory") == 0) {
            ++i;
            fixture = true;
        } else if (std::strcmp(argv[i], "--test-fixture") == 0 && i + 1 < argc &&
                   std::strcmp(argv[i + 1], "stage") == 0) {
            ++i;
            fixture = stageFixture = true;
        } else {
            return false;
        }
    }
    if (fixture) {
        std::ifstream marker(paths->user / ".petari-test-fixture");
        std::string kind;
        std::getline(marker, kind);
        const char* home = std::getenv("HOME");
        const bool normalUser = home && !paths->user.empty() &&
            std::filesystem::weakly_canonical(paths->user) ==
            std::filesystem::weakly_canonical(std::filesystem::path(home) / "Library/Application Support/Petari");
        const char* wanted = stageFixture ? "stage" : "observatory";
        if (paths->user.empty() || normalUser || kind != wanted) {
            std::fprintf(stderr, "petari: fixture requires explicit isolated --user with .petari-test-fixture containing %s\n", wanted);
            return false;
        }
        if (stageFixture && !stageFixtureFromEnvironment()) {
            return false;
        }
        PetariNative::TestFixture::observatory = true;
        std::fputs("PETARI FIXTURE: post-tutorial observatory progression; not earned progression\n", stderr);
    }
    // The stage smoke script needs the stage fixture's entry.
    const char* smoke = std::getenv("PETARI_SMOKE");
    if (smoke != nullptr && std::strcmp(smoke, "stage") == 0 && !stageFixture) {
        std::fputs("petari: PETARI_SMOKE=stage requires --test-fixture stage\n", stderr);
        return false;
    }
    if (paths->disc.empty()) {
        const char* env = std::getenv("PETARI_GAME_DIR");
        paths->disc = env != nullptr && env[0] != '\0' ? std::filesystem::path(env)
                                                      : std::filesystem::current_path() / "build/game-data/RMGE01";
    }
    if (stageFixture) {
        const std::string& stage = PetariNative::TestFixture::stage;
        if (!std::filesystem::is_regular_file(paths->disc / "files/StageData" / stage / (stage + "Scenario.arc"))) {
            std::fprintf(stderr, "petari: PETARI_STAGE %s has no StageData/%s/%sScenario.arc on the disc\n",
                         stage.c_str(), stage.c_str(), stage.c_str());
            return false;
        }
        std::fprintf(stderr, "PETARI FIXTURE: synthetic stage entry requested: %s scenario %d (test entry through the "
                     "after-loading galaxy move; no progression claims)\n", stage.c_str(),
                     PetariNative::TestFixture::stageScenario);
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

bool prepareKnownPipelines(SDL_Window* window) {
    // Aurora queues its persisted configurations during initialization. Finish
    // those before the game starts, instead of competing with the first draw.
    const auto pending = [] {
        return __atomic_load_n(&aurora_get_stats()->queuedPipelines, __ATOMIC_ACQUIRE);
    };
    const unsigned initial = pending();
    if (initial == 0) return true;
    const Uint64 start = SDL_GetTicks();
    unsigned displayed = initial + 1;
    while (const unsigned remaining = pending()) {
        if (remaining != displayed) {
            char title[128];
            std::snprintf(title, sizeof(title), "Super Mario Galaxy — Preparing shaders (%u remaining)", remaining);
            SDL_SetWindowTitle(window, title);
            displayed = remaining;
        }
        for (const AuroraEvent* event = aurora_update(); event && event->type != AURORA_NONE; ++event) {
            if (event->type == AURORA_EXIT) return false;
        }
        SDL_Delay(10);
    }
    SDL_SetWindowTitle(window, "Super Mario Galaxy");
    std::fprintf(stderr, "[gx warmup] prepared %u queued pipelines before gameplay in %.2f s\n",
                 initial, (SDL_GetTicks() - start) / 1000.0);
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
    if (!prepareKnownPipelines(static_cast<SDL_Window*>(info.window))) {
        aurora_shutdown();
        return 0;
    }
    // Opt-in (PETARI_PIPELINE_GLOBAL_PRECOMPILE=background): queue every known pipeline at
    // background priority so it compiles while the game plays. A no-op otherwise.
    petari_gx_pipeline_background_begin();
    // Audio opens on a game thread when the game starts AI DMA; initialize
    // SDL's audio subsystem here, on the main thread, first. The sink's own
    // initialization then only adds a reference.
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "petari: SDL audio: %s\n", SDL_GetError());
    }

    App::Seam::attach(info.window);
    App::Seam::setPhaseProbe(App::Host::framePhase);
    App::Host::startOS();
    App::Seam::openFirstFrame();
    App::Host::runGame();
}
