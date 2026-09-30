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
    std::fputs("100,0,100,Jump\n150,100,100,Walk\n300,100,100,Hop\n310,400,100,Kick\n310,600,300,Kick\n500,700,300,Walk\n", routeFile);
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
    const auto logged = [&](const char* text) {
        for (const auto& line : jumper.log()) if (line.find(text) != std::string::npos) return true;
        return false;
    };
    const auto pressedA = [](const Step& step) {
        for (const auto& press : step.presses) if (press.button == Button::A && press.down) return true;
        return false;
    };
    jumpObs.playerX = 300; jumper.step(jumpObs);
    check(logged("jump and spin at waypoint 2"), "hop before the wall-kick chain");
    jumpObs.playerOnGround = false;
    bool kicked = false, steered = false;
    for (int i = 0; i < 6; ++i) {  // rising: moving, so no kick yet
        jumpObs.playerY = 150 + 40 * i; jumpObs.playerX = 300 + i;
        const auto rising = jumper.step(jumpObs);
        kicked |= pressedA(rising);
        for (const auto& press : rising.presses) steered |= press.button != Button::A && press.down;
    }
    check(!kicked && !logged("wall kick"), "no wall kick while Mario still moves");
    check(steered, "wall-kick leg steers into the wall");
    jumpObs.playerX = 310; jumpObs.playerY = 400; jumper.step(jumpObs);
    const auto cling = jumper.step(jumpObs);
    check(pressedA(cling) && logged("wall kick at waypoint 3"), "clinging at the contact presses A");
    jumpObs.playerX = 1000; jumpObs.playerY = 100; jumper.step(jumpObs);
    check(!pressedA(jumper.step(jumpObs)), "a stall far from the next contact is not a kick");
    jumpObs.playerX = 300; jumpObs.playerOnGround = true;
    bool retried = false;
    for (int i = 0; i < 25 && !retried; ++i) {
        jumper.step(jumpObs);
        retried = logged("fell short");
    }
    check(retried && jumper.result() == Result::Running, "a missed kick retries from the chain's hop");
    for (int attempt = 0; attempt < 4 && jumper.result() == Result::Running; ++attempt) {
        jumpObs.playerX = 300; jumpObs.playerY = 100; jumpObs.playerOnGround = true;
        for (int i = 0; i < 30 && jumper.result() == Result::Running; ++i) jumper.step(jumpObs);
    }
    check(jumper.result() == Result::Fail, "repeated missed kicks fail instead of looping");
    unlink(routePath);
    std::printf("%d dome/finale checks passed\n", checks);
}
