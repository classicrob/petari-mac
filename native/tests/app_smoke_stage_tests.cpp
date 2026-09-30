// The stage smoke script (native/app/smoke_stage.hpp) against a small fake
// world: Mario moves with the held stick keys, jumps on A, the pause menu
// opens after a 12-frame Plus hold and closes on the next Plus press.

#include "../app/smoke_stage.hpp"

#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <string>

using namespace PetariNative::App::Smoke;

namespace {

int gFailures = 0;

void expect(bool condition, const std::string& what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++gFailures;
    }
}

struct World {
    std::string stage = "EggStarGalaxy";
    int selected = 2, placed = 2;
    bool held[11] = {};
    unsigned long plusHeld = 0;
    bool paused = false, pauseHandled = false;
    float x = 0, y = 0, z = 0, vy = 0;
    unsigned long frame = 0;
    unsigned long loadFrames = 30;
    bool dieAt = false;
    unsigned long dieFrame = 0;
    bool promptAt = false;
    unsigned long physicalAt = 0;
    bool noMove = false;
    // Camera: D-pad left/right turn the view 45 degrees where the camera allows
    // rotation; the game sees the trigger unless presses are lost.
    enum class Camera { Rotates, Fixed, InputLost } camera = Camera::Rotates;
    float yaw = 0.0f;
    bool leftTrigger = false, rightTrigger = false;
    std::vector<std::string> pending;

    Observation observe() {
        ++frame;
        Observation o;
        o.scene = "Game";
        o.stage = stage;
        o.scenario = placed;
        o.selectedScenario = selected;
        o.sceneReady = frame > loadFrames;
        if (frame == 1) o.milestones.push_back("FileSelector.DemoStartWait");
        o.milestones.insert(o.milestones.end(), pending.begin(), pending.end());
        pending.clear();
        o.playerValid = o.sceneReady;
        o.playerX = x;
        o.playerY = y;
        o.playerZ = z;
        o.playerOnGround = y <= 0.0f;
        o.pausePermitted = !paused;
        o.playerDead = dieAt && frame >= dieFrame;
        if (promptAt && frame == loadFrames + 100) o.prompts.push_back({"System_Test", 2});
        if (physicalAt != 0 && frame >= physicalAt) o.physical.gameplay = 1;
        o.camZx = std::sin(yaw);
        o.camZz = std::cos(yaw);
        o.padLeftTrigger = leftTrigger;
        o.padRightTrigger = rightTrigger;
        o.camRoundLeft = o.camRoundRight = camera == Camera::Rotates;
        if ((leftTrigger || rightTrigger) && camera == Camera::Rotates) yaw += leftTrigger ? 0.785f : -0.785f;
        leftTrigger = rightTrigger = false;
        return o;
    }
    void apply(const Step& step) {
        for (const Press& p : step.presses) {
            const int b = static_cast<int>(p.button);
            if (b == static_cast<int>(Button::Plus) && p.down && paused) {
                paused = false;
                pending.push_back("PauseMenu.Close");
            }
            if (b == static_cast<int>(Button::A) && p.down && y <= 0.0f && !paused) vy = 20.0f;
            if (p.down && camera != Camera::InputLost && !held[b]) {
                leftTrigger = leftTrigger || b == static_cast<int>(Button::CameraLeft);
                rightTrigger = rightTrigger || b == static_cast<int>(Button::CameraRight);
            }
            held[b] = p.down;
        }
        plusHeld = held[static_cast<int>(Button::Plus)] ? plusHeld + 1 : 0;
        if (plusHeld == 12 && !paused) {
            paused = true;
            pending.push_back("PauseMenu.Open");
        }
        if (paused || noMove) return;
        const float speed = 5.0f;
        if (held[static_cast<int>(Button::StickUp)]) z += speed;
        if (held[static_cast<int>(Button::StickDown)]) z -= speed;
        if (held[static_cast<int>(Button::StickRight)]) x += speed;
        if (held[static_cast<int>(Button::StickLeft)]) x -= speed;
        if (vy != 0.0f || y > 0.0f) {
            y += vy;
            vy -= 1.0f;
            if (y <= 0.0f) {
                y = 0.0f;
                vy = 0.0f;
            }
        }
    }
};

std::vector<std::string> gLog;

StageDriver run(World& world, StageConfig config = {"EggStarGalaxy", 2, 100}) {
    StageDriver driver(20000, config);
    gLog.clear();
    for (int i = 0; i < 20000 && driver.result() == Result::Running; ++i) {
        world.apply(driver.step(world.observe()));
        gLog.insert(gLog.end(), driver.log().begin(), driver.log().end());
    }
    return driver;
}

// The logged detail of a stage check ("stage check <name>: <status> <detail>").
std::string checkLine(const std::string& name) {
    for (const std::string& line : gLog) {
        if (line.rfind("stage check " + name + ":", 0) == 0) return line;
    }
    return "<no " + name + " check>";
}

}  // namespace

int main() {
    {
        World world;
        const StageDriver driver = run(world);
        expect(driver.result() == Result::Pass, "happy path passes: " + driver.reason());
        expect(driver.reason().find("walks responsive 4/4") != std::string::npos, "all walks responsive: " + driver.reason());
        expect(!world.paused, "pause closed at the end");
    }
    {
        World world;
        world.noMove = true;
        const StageDriver driver = run(world);
        expect(driver.result() == Result::Pass, "unresponsive stick is a warning, not a failure: " + driver.reason());
        expect(driver.reason().find("walks responsive 0/4") != std::string::npos, "walk count reported: " + driver.reason());
    }
    {
        World world;
        world.stage = "AstroGalaxy";
        const StageDriver driver = run(world);
        expect(driver.result() == Result::Fail && driver.reason().find("entry was not applied") != std::string::npos,
               "wrong stage fails: " + driver.reason());
    }
    {
        World world;
        world.selected = 1;
        const StageDriver driver = run(world);
        expect(driver.result() == Result::Fail && driver.reason().find("scenario mismatch") != std::string::npos,
               "wrong scenario fails: " + driver.reason());
    }
    {
        World world;
        world.dieAt = true;
        world.dieFrame = 400;
        const StageDriver driver = run(world);
        expect(driver.result() == Result::Fail && driver.reason().find("died:") != std::string::npos,
               "death fails with died: " + driver.reason());
    }
    {
        World world;
        world.promptAt = true;
        const StageDriver driver = run(world);
        expect(driver.result() == Result::Blocked, "a system prompt blocks: " + driver.reason());
    }
    {
        World world;
        world.physicalAt = 300;
        const StageDriver driver = run(world);
        expect(driver.result() == Result::Assisted, "physical input makes it assisted: " + driver.reason());
    }
    {
        World world;
        world.loadFrames = 100000;
        const StageDriver driver = run(world);
        expect(driver.result() == Result::Fail && driver.reason().find("not ready:") != std::string::npos,
               "load timeout fails: " + driver.reason());
    }
    // Camera checks say why a view did not turn.
    {
        World world;
        run(world);
        expect(checkLine("camera_left").find(": ok view turned 45") != std::string::npos,
               "rotating camera: ok (" + checkLine("camera_left") + ")");
    }
    {
        World world;
        world.camera = World::Camera::Fixed;
        const StageDriver driver = run(world);
        expect(driver.result() == Result::Pass, "fixed camera still passes: " + driver.reason());
        expect(checkLine("camera_right").find(": warn view turned 0.0 degrees; fixed camera") != std::string::npos,
               "fixed camera: warn, explained (" + checkLine("camera_right") + ")");
    }
    {
        World world;
        world.camera = World::Camera::InputLost;
        run(world);
        expect(checkLine("camera_left").find(": warn view turned 0.0 degrees; INPUT: the game never saw") != std::string::npos,
               "lost press: warn, blamed on input (" + checkLine("camera_left") + ")");
    }
    if (gFailures == 0) std::puts("app smoke stage tests passed");
    return gFailures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
