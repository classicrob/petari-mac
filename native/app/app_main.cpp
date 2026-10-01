// Native entry point: window, platform start-up, then the game's main loop
// on this (the macOS main) thread. See docs/dev/INTEGRATION_PLAN.md.

#include <aurora/aurora.h>
#include <aurora/main.h>
#include <aurora/event.h>
#include <aurora/gfx.h>

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_hints.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread/qos.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <petari/test_fixture.hpp>
#include <petari/unlocked_save.hpp>
#include <petari/pipeline_startup.hpp>
#include <petari/mods.hpp>
#include <petari/progress.hpp>
#include <petari/camera_settings.hpp>
#include <petari/input.hpp>

#include "host.hpp"
#include "smoke_background.hpp"
#include "smoke_storage.hpp"
#include "pipeline_cache_dir.hpp"
#include "latency_report.hpp"
#include <petari/launch_stage.hpp>
#include <petari/player_launch.hpp>
#include <petari/host_allocation.hpp>

extern "C" void petari_gx_pipeline_background_begin();
extern "C" void petari_gx_pipeline_set_gameplay(bool gameplay);

namespace App = PetariNative::App;

namespace {

void logMessage(AuroraLogLevel level, const char* module, const char* message, unsigned int length) {
    std::fprintf(stderr, "[%s] %.*s\n", module, static_cast<int>(length), message);
    if (level == LOG_FATAL) {
        std::abort();
    }
}

void usage() {
    std::fputs("Usage: petari [--disc DIR] [--user DIR] [--stage GALAXY [--scenario N]] [--test-fixture observatory|stage]\n"
               "  --disc DIR  extracted disc (containing files/); default: $PETARI_GAME_DIR,\n"
               "              else build/game-data/RMGE01 under the working directory\n"
               "  --user DIR  saves, settings, controls and crash reports;\n"
               "              default: ~/Library/Application Support/Petari\n"
               "  --stage GALAXY [--scenario N]  after you load a file (past the tutorial), go straight to\n"
               "              this galaxy and mission (default 1) instead of the observatory; once per session.\n"
               "              GALAXY: an alias such as good-egg, or an internal name. --stage list prints them\n"
               "  --test-fixture observatory  post-tutorial test progression; requires a marked isolated --user\n"
               "  --test-fixture stage  synthetic entry to $PETARI_STAGE scenario $PETARI_SCENARIO after the file\n"
               "              loads (observatory progression otherwise); requires --user marked \"stage\"\n"
               "  --make-unlocked-save all-missions|complete-luigi|grand-finale  unlock file 1 of an isolated\n"
               "              --user holding .petari-make-unlocked-save, save, reload and verify it, then exit\n"
               "              (native/SAVES.md; use native/tools/make_unlocked_save.py)\n",
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
    std::string unlockedSave;
    std::string launchStage;
    int launchScenario = 1;
    bool launchScenarioGiven = false;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--stage") == 0 && i + 1 < argc) {
            launchStage = argv[++i];
        } else if (std::strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
            char* end = nullptr;
            const long value = std::strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || value < 1 || value > 99) {
                std::fprintf(stderr, "petari: --scenario expects a mission number, got %s\n", argv[i]);
                return false;
            }
            launchScenario = static_cast<int>(value);
            launchScenarioGiven = true;
        } else if (std::strcmp(argv[i], "--disc") == 0 && i + 1 < argc) {
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
        } else if (std::strcmp(argv[i], "--make-unlocked-save") == 0 && i + 1 < argc &&
                   (std::strcmp(argv[i + 1], "all-missions") == 0 || std::strcmp(argv[i + 1], "complete-luigi") == 0 ||
                    std::strcmp(argv[i + 1], "grand-finale") == 0)) {
            unlockedSave = argv[++i];
        } else {
            return false;
        }
    }
    if (launchScenarioGiven && launchStage.empty()) {
        std::fputs("petari: --scenario needs --stage\n", stderr);
        return false;
    }
    if (!launchStage.empty()) {
        if (fixture || !unlockedSave.empty()) {
            std::fputs("petari: --stage is for normal play; test fixtures use PETARI_STAGE\n", stderr);
            return false;
        }
        std::string stage, error;
        if (!App::LaunchStage::resolve(launchStage, launchScenario, &stage, &error)) {
            std::fprintf(stderr, "petari: --stage: %s\n", error.c_str());
            return false;
        }
        PetariNative::PlayerLaunch::stage = stage;
        PetariNative::PlayerLaunch::scenario = launchScenario;
        std::fprintf(stderr, "PETARI LAUNCH: after a file loads, going to %s mission %d\n", stage.c_str(), launchScenario);
    }
    if (!unlockedSave.empty()) {
        // Rewrites a saved file: only an explicit, marked, isolated directory.
        const char* home = std::getenv("HOME");
        const bool normalUser = home && !paths->user.empty() &&
            std::filesystem::weakly_canonical(paths->user) ==
            std::filesystem::weakly_canonical(std::filesystem::path(home) / "Library/Application Support/Petari");
        if (paths->user.empty() || normalUser || fixture ||
            !std::filesystem::is_regular_file(paths->user / ".petari-make-unlocked-save")) {
            std::fputs("petari: --make-unlocked-save requires an explicit isolated --user holding "
                       ".petari-make-unlocked-save, and no --test-fixture\n", stderr);
            return false;
        }
        PetariNative::UnlockedSave::variant = unlockedSave;
        std::fprintf(stderr, "PETARI UNLOCKED SAVE: requested %s for file %d in %s\n", unlockedSave.c_str(),
                     PetariNative::UnlockedSave::slot, paths->user.string().c_str());
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
    // The blocking preparation screen is opt-in (PETARI_PIPELINE_GLOBAL_PRECOMPILE=1).
    // By default the game starts at once; the scene being loaded compiles first and
    // everything else compiles in the background while playing.
    if (!petari_gx_pipeline_full_preparation()) return true;
    return App::preparePipelines(window);
}

// Frame classification also tells the shader pool when speculative compiles
// must yield to gameplay (menus, file select, loading and pause run them at full speed).
App::FrameStats::Phase framePhaseForPipelines() {
    const App::FrameStats::Phase phase = App::Host::framePhase();
    const bool gameplay = phase == App::FrameStats::Phase::Gameplay && !App::Host::pauseMenuActive();
    petari_gx_pipeline_set_gameplay(gameplay);
    App::LatencyReport::frame(gameplay);
    // Test driver (PETARI_MODS_AUTOPRESS=<frames>): during gameplay, press the
    // collect mod's button every <frames> frames and the shoot mod's halfway
    // between. The mods still decide whether a press does anything.
    static const long period = [] {
        const char* value = std::getenv("PETARI_MODS_AUTOPRESS");
        return value != nullptr ? std::strtol(value, nullptr, 10) : 0L;
    }();
    // Test driver (PETARI_TEST_WARP=<galaxy>:<mission>): once, 30 gameplay frames
    // in, queue the warp the Home menu's Level Select Go would (its UI is
    // covered by native_home_menu); the game takes it at its next permitted frame.
    static std::string testWarp = [] {
        const char* value = std::getenv("PETARI_TEST_WARP");
        return std::string(value != nullptr ? value : "");
    }();
    static long warpFrames = 0;
    if (!testWarp.empty() && gameplay && ++warpFrames == 30) {
        const auto colon = testWarp.find(':');
        std::string stage, error;
        const int mission = colon == std::string::npos ? 1 : std::atoi(testWarp.c_str() + colon + 1);
        if (App::LaunchStage::resolve(testWarp.substr(0, colon), mission, &stage, &error)) {
            std::snprintf(PetariNative::PlayerLaunch::warpStage, sizeof(PetariNative::PlayerLaunch::warpStage), "%s", stage.c_str());
            PetariNative::PlayerLaunch::warpScenario = mission;
            PetariNative::PlayerLaunch::warpPending.store(true);
            std::fprintf(stderr, "[level-select-test] queued warp to %s mission %d\n", stage.c_str(), mission);
        } else {
            std::fprintf(stderr, "[level-select-test] %s\n", error.c_str());
        }
        testWarp.clear();
    }
    // Test driver (PETARI_CAMERA_TEST=1, with the Odyssey camera mod on): a fixed
    // script of right-stick orbit, pitch, zoom, mouse drag and recentre during gameplay.
    static const bool cameraTest = std::getenv("PETARI_CAMERA_TEST") != nullptr;
    static long cameraFrames = 0;
    if (cameraTest && gameplay && PetariNative::CameraSettings::enabled()) {
        namespace Camera = PetariNative::CameraSettings;
        const long f = ++cameraFrames % 900;
        if (f == 60) std::fputs("[camera-test] orbit right\n", stderr);
        if (f == 300) std::fputs("[camera-test] pitch, zoom, drag, recentre\n", stderr);
        Camera::stick(f >= 60 && f < 180 ? 0.7f : 0.0f, f >= 240 && f < 300 ? 0.6f : 0.0f);
        if (f == 320) Camera::zoomSteps(4.0f);
        if (f == 380) Camera::zoomSteps(-6.0f);
        if (f >= 420 && f < 450) Camera::mouseDrag(-8.0f, 2.0f);
        if (f == 600) Camera::recenter();
    }
    static long gameplayFrames = 0;
    if (period > 1 && gameplay) {
        ++gameplayFrames;
        if (gameplayFrames % period == 0) {
            PetariNative::Input::injectActionPress(PetariNative::Input::Action::ModCollectStarBits);
            std::fprintf(stderr, "[mods-test] collect press at gameplay frame %ld\n", gameplayFrames);
        } else if (gameplayFrames % period == period / 2) {
            PetariNative::Input::injectActionPress(PetariNative::Input::Action::ModShootEnemy);
            std::fprintf(stderr, "[mods-test] shoot press at gameplay frame %ld\n", gameplayFrames);
        }
    }
    return phase;
}

}  // namespace

int main(int argc, char** argv) {
    // The allocation-site check's symbol table, before any game thread runs
    // (building it on a first game allocation would be a startup hitch).
    PetariNative::prepareAllocationSymbols();
    App::Paths paths;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--stage") == 0 && std::strcmp(argv[i + 1], "list") == 0) {
            App::LaunchStage::printList(stdout);
            return 0;
        }
    }
    if (!resolvePaths(argc, argv, &paths)) {
        usage();
        return 2;
    }

    std::string error;
    App::SmokeBackground::enabled = App::SmokeBackground::requested(
        std::getenv("PETARI_SMOKE"), PetariNative::TestFixture::observatory,
        std::getenv("PETARI_SMOKE_BACKGROUND"));
    App::SmokeStorage smokeStorage;
    if (App::SmokeBackground::enabled && !smokeStorage.prepare(paths.user, std::getenv("PETARI_CACHE_DIR"), &error)) {
        std::fprintf(stderr, "petari: background storage: %s\n", error.c_str());
        return 1;
    }
    if (!App::Host::preparePlatform(paths, &error)) {
        std::fprintf(stderr, "petari: %s\n", error.c_str());
        return 1;
    }
    if (!App::Events::loadControls(paths.user / "controls.txt", &error)) {
        std::fprintf(stderr, "petari: controls: %s\n", error.c_str());
        return 1;
    }
    // The personal progress record (native/PROGRESS.md): beside the save, never inside it. A damaged
    // file is reported and left alone; progress is then not recorded this run.
    if (!PetariNative::Progress::load(paths.user / "progress.json", &error)) {
        std::fprintf(stderr, "petari: progress: %s (not recording progress this run)\n", error.c_str());
    }
    if (!PetariNative::Mods::load(paths.user / "mods.txt", &error)) {
        std::fprintf(stderr, "petari: mods: %s\n", error.c_str());
        return 1;
    }
    if (!PetariNative::CameraSettings::load(paths.user / "camera.txt", &error)) {
        std::fprintf(stderr, "petari: camera: %s\n", error.c_str());
        return 1;
    }
    if (PetariNative::CameraSettings::enabled()) std::fputs("PETARI MODS: OdysseyCamera on\n", stderr);
    for (int m = 0; m < static_cast<int>(PetariNative::Mods::Mod::Count); ++m) {
        const auto mod = static_cast<PetariNative::Mods::Mod>(m);
        if (PetariNative::Mods::enabled(mod)) std::fprintf(stderr, "PETARI MODS: %s on\n", PetariNative::Mods::name(mod));
    }

    SDL_SetHintWithPriority("PETARI_SMOKE_BACKGROUND", App::SmokeBackground::enabled ? "1" : "0", SDL_HINT_OVERRIDE);
    if (App::SmokeBackground::enabled) {
        SDL_SetHintWithPriority(SDL_HINT_MAC_BACKGROUND_APP, "1", SDL_HINT_OVERRIDE);
        SDL_SetHintWithPriority(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0", SDL_HINT_OVERRIDE);
        SDL_SetHintWithPriority(SDL_HINT_WINDOW_ACTIVATE_WHEN_RAISED, "0", SDL_HINT_OVERRIDE);
        SDL_SetEventFilter(App::SmokeBackground::eventFilter, nullptr);
        // Let SDL install its Cocoa application/delegate first, before creating
        // any window. Accessory policy avoids normal foreground application activation.
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            std::fprintf(stderr, "petari: background video initialization: %s\n", SDL_GetError());
            return 1;
        }
        const auto sendObject = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend);
        const id application = sendObject(reinterpret_cast<id>(objc_getClass("NSApplication")),
                                          sel_registerName("sharedApplication"));
        constexpr long backgroundActivation = 1; // NSApplicationActivationPolicyAccessory
        const auto setPolicy = reinterpret_cast<BOOL (*)(id, SEL, long)>(objc_msgSend);
        if (!application || !setPolicy(application, sel_registerName("setActivationPolicy:"), backgroundActivation)) {
            std::fputs("petari: could not set background app activation policy\n", stderr);
            return 1;
        }
        // Opt-in (PETARI_SMOKE_QOS=1): declare the hidden process user-initiated and latency
        // critical (no App Nap) and raise this thread's QoS (later threads inherit it). The A/B
        // on EggStarGalaxy s1 (qos-on-v2 / qos-off-v2) showed no benefit once machine load was
        // controlled, so it is not the default.
        if (const char* qosEnv = std::getenv("PETARI_SMOKE_QOS"); !qosEnv || qosEnv[0] != '1') {
            std::fputs("PETARI SMOKE QOS: default (PETARI_SMOKE_QOS=1 enables activity + QoS boost)\n", stderr);
        } else {
            const auto sendClass = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend);
            const auto makeString = reinterpret_cast<id (*)(id, SEL, const char*)>(objc_msgSend);
            const auto beginActivity = reinterpret_cast<id (*)(id, SEL, unsigned long long, id)>(objc_msgSend);
            const id processInfo = sendClass(reinterpret_cast<id>(objc_getClass("NSProcessInfo")), sel_registerName("processInfo"));
            const id reason = makeString(reinterpret_cast<id>(objc_getClass("NSString")), sel_registerName("stringWithUTF8String:"),
                                         "Petari background smoke test");
            constexpr unsigned long long userInitiated = 0x00FFFFFFULL, latencyCritical = 0xFF00000000ULL;
            id activity = processInfo ? beginActivity(processInfo, sel_registerName("beginActivityWithOptions:reason:"),
                                                      userInitiated | latencyCritical, reason) : nullptr;
            if (activity) sendClass(activity, sel_registerName("retain")); // held until process exit
            const int qos = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
            std::fprintf(stderr, "PETARI SMOKE QOS: activity=%s thread_qos=%s\n", activity ? "user-initiated+latency-critical" : "unavailable",
                         qos == 0 ? "user-interactive" : "unavailable");
        }
        std::fputs("PETARI SMOKE BACKGROUND: enabled; physical input isolated; nonfocusable background window; VI pacing unchanged\n", stderr);
    }

    const std::string userPath = paths.user.string();
    AuroraConfig config{};
    config.appName = "Super Mario Galaxy";
    config.userPath = userPath.c_str();
    App::PipelineCacheDir pipelineCache;
    if (!App::SmokeBackground::enabled) {
        const char* smoke = std::getenv("PETARI_SMOKE");
        std::error_code markerError;
        const bool testRun = PetariNative::TestFixture::observatory || (smoke && smoke[0] && smoke[0] != '0') ||
            !PetariNative::UnlockedSave::variant.empty() ||
            std::filesystem::exists(paths.user / ".petari-test-fixture", markerError);
        pipelineCache.prepare(paths.user, testRun, std::getenv("PETARI_CACHE_DIR"));
    }
    const std::string cachePath = App::SmokeBackground::enabled ? smokeStorage.cache.string()
                                                                : pipelineCache.path.string();
    config.cachePath = cachePath.c_str();
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
    if (App::SmokeBackground::enabled) {
        auto* window = static_cast<SDL_Window*>(info.window);
        if (!SDL_SetWindowFocusable(window, false)) {
            std::fprintf(stderr, "petari: could not isolate background window focus: %s\n", SDL_GetError());
            aurora_shutdown();
            return 1;
        }
        if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN)) {
            std::fputs("petari: background window unexpectedly visible\n", stderr);
            aurora_shutdown();
            return 1;
        }
        int x = 0, y = 0;
        SDL_GetWindowPosition(window, &x, &y);
        std::fprintf(stderr, "PETARI SMOKE BACKGROUND: accessory activation policy; hidden window at %d,%d; flags=0x%llx\n",
                     x, y, static_cast<unsigned long long>(SDL_GetWindowFlags(window)));
        App::Events::assertFocus();
    }
    App::LatencyReport::configureLayer(info.window);
    if (!prepareKnownPipelines(static_cast<SDL_Window*>(info.window))) {
        aurora_shutdown();
        return 0;
    }
    // Default (unset or PETARI_PIPELINE_GLOBAL_PRECOMPILE=background): queue the global seed
    // at background priority so it compiles while the game plays. A no-op for 0 or 1.
    petari_gx_pipeline_background_begin();
    // Audio opens on a game thread when the game starts AI DMA; initialize
    // SDL's audio subsystem here, on the main thread, first. The sink's own
    // initialization then only adds a reference.
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "petari: SDL audio: %s\n", SDL_GetError());
    }

    App::Seam::attach(info.window);
    App::Seam::setPhaseProbe(framePhaseForPipelines);
    App::LatencyReport::startTestDriver();
    App::Host::startOS();
    App::Seam::openFirstFrame();
    App::Host::runGame();
}
