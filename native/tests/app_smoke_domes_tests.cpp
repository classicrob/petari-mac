#include "../app/smoke_domes.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace PetariNative::App::Smoke;
static int checks;
static void check(bool ok, const char* message) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
static void enterDome(DomesDriver& driver, Observation& o) {
    o.scene = "Game"; o.stage = "AstroGalaxy"; o.scenario = 5;
    o.sceneReady = o.playerValid = o.pausePermitted = o.playerOnGround = true;
    o.gravityY = -1; o.camXx = 1; o.camZz = 1;
    o.milestones = {"FileSelector.DemoStartWait"};
    driver.step(o); o.milestones.clear();
    for (int i = 0; i < 70 && std::string(driver.phase()) != "domes: calibrating the stick"; ++i) driver.step(o);
    check(std::string(driver.phase()) == "domes: calibrating the stick", "reached calibration");
    for (int i = 1; i <= 41; ++i) {
        if (i == 21) o.playerZ += 100;
        if (i == 41) o.playerX += 100;
        driver.step(o);
    }
    check(std::string(driver.phase()) == "domes: walking to the dome", "calibration finished");
    o.stage = "AstroDome"; o.scenario = 2;
    o.playerX = 0; o.playerY = -705; o.playerZ = 721;
    driver.step(o);
    for (int i = 0; i < 70 && std::string(driver.phase()) != "domes: pointing at the Blue Star"; ++i) driver.step(o);
    check(std::string(driver.phase()) == "domes: pointing at the Blue Star", "dome ready");
}
int main() {
    DomesConfig config; config.dome = 2;
    DomesDriver driver(10000, config); Observation o;
    enterDome(driver, o);
    bool moved = false;
    for (int i = 0; i < 30; ++i) {
        for (const auto& press : driver.step(o).presses)
            moved |= press.button == Button::StickDown && press.down;
    }
    check(moved, "offscreen star causes real pad approach toward room centre");
    o.targets = {{"Dome.BlueStar", 0, .5f, .4f, kTargetSelectable | kTargetPointing}};
    const auto visible = driver.step(o);
    bool stopped = false;
    for (const auto& press : visible.presses) stopped |= press.button == Button::StickDown && !press.down;
    check(stopped, "visible star stops approach");
    check(visible.pointer, "visible star uses observed pointer position");
    driver.step(o); driver.step(o);
    check(std::string(driver.phase()) == "domes: selecting a galaxy", "click still requires pointing target");
    DomesDriver missing(10000, config); Observation absent;
    enterDome(missing, absent);
    for (int i = 0; i < 901 && missing.result() == Result::Running; ++i) missing.step(absent);
    check(missing.result() == Result::Fail, "approach does not waive missing-target timeout");
    std::printf("%d dome approach checks passed\n", checks);
}
