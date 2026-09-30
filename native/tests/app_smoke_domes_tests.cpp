#include "../app/smoke_domes.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

using namespace PetariNative::App::Smoke;
static int checks;
static void check(bool ok, const char* message) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
static void enterDome(DomesDriver& driver, Observation& o, bool routeOnly = false) {
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
    if (routeOnly) return;
    o.stage = "AstroDome"; o.scenario = 2;
    o.playerX = 0; o.playerY = -705; o.playerZ = 721;
    driver.step(o);
    for (int i = 0; i < 70 && std::string(driver.phase()) != "domes: pointing at the Blue Star"; ++i) driver.step(o);
    check(std::string(driver.phase()) == "domes: pointing at the Blue Star", "dome ready");
}
int main() {
    char routePath[] = "/tmp/petari-dome-route-XXXXXX";
    const int routeFd = mkstemp(routePath);
    check(routeFd >= 0, "temporary route created");
    FILE* routeFile = fdopen(routeFd, "w");
    std::fputs("100,0,100,Jump\n150,100,100,Walk\n300,100,100,Walk\n", routeFile);
    std::fclose(routeFile);
    setenv("PETARI_DOME_ROUTE", routePath, 1);

    setenv("PETARI_SMOKE", "domes", 1);
    setenv("PETARI_DOME", "2", 1);
    DomesConfig config;
    check(domesEnabledFromEnvironment(&config), "test route loaded through environment setup");
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
    DomesConfig finaleConfig; finaleConfig.dome = 7;
    DomesDriver finale(10000, finaleConfig); Observation finalObs;
    finalObs.scene = "Game"; finalObs.stage = "AstroGalaxy"; finalObs.sceneReady = true;
    finalObs.milestones = {"FileSelector.DemoStartWait"}; finale.step(finalObs); finalObs.milestones.clear();
    finalObs.scene = "ScenarioSelect"; finalObs.stage = "PeachCastleFinalGalaxy";
    finalObs.targets = {{"Scenario.Star", 1, .5f, .5f, kTargetSelectable | kTargetPointing}};
    finale.step(finalObs);
    check(std::string(finale.phase()) == "domes: selecting the mission", "finale route reaches real scenario UI");
    bool selected = false;
    for (int i = 0; i < 4; ++i)
        for (const auto& press : finale.step(finalObs).presses) selected |= press.button == Button::A && press.down;
    check(selected, "finale scenario selected with A");
    check(std::string(finale.phase()) == "domes: loading the mission", "finale awaits selected mission load");
    DomesDriver bypass(10000, finaleConfig);
    finalObs.scene = "Game"; finalObs.stage = "AstroGalaxy";
    finalObs.milestones = {"FileSelector.DemoStartWait"}; bypass.step(finalObs); finalObs.milestones.clear();
    finalObs.stage = "PeachCastleFinalGalaxy"; finalObs.targets.clear(); bypass.step(finalObs);
    check(bypass.result() == Result::Fail, "finale cannot pass an unobserved direct stage entry");
    DomesDriver jumper(10000, config); Observation jumpObs;
    enterDome(jumper, jumpObs, true);
    jumper.step(jumpObs);
    jumpObs.playerX = 150; jumpObs.playerY = 100; jumpObs.playerOnGround = false;
    const auto advanced = [&]() {
        for (const auto& line : jumper.log()) if (line.find("route waypoint 2 at") != std::string::npos) return true;
        return false;
    };
    for (int i = 0; i < 10; ++i) {
        jumper.step(jumpObs);
        check(!advanced(), "airborne jump arrival does not advance the route");
    }
    jumpObs.playerOnGround = true; jumper.step(jumpObs);
    check(advanced(), "grounded jump arrival advances the route");
    unlink(routePath);
    std::printf("%d dome/finale checks passed\n", checks);
}
