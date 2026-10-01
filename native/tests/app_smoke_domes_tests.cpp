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
    std::fputs("100,0,100,Jump\n150,100,100,Walk\n300,100,100,Hop\n310,400,100,Kick\n310,600,300,Kick\n500,700,300,Walk\n900,700,300,Walk\n1300,700,300,Walk\n", routeFile);
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
    jumpObs.playerX = 300;
    int settle = 0;
    bool steered = false, released = false;
    for (; settle < 15 && !logged("jump and spin at waypoint 2"); ++settle)
        for (const auto& press : jumper.step(jumpObs).presses) steered |= press.button != Button::A && press.down;
    check(logged("facing the wall") && settle >= 10, "the chain's hop first walks into the wall, then jumps");
    jumpObs.playerOnGround = false;
    bool kicked = false;
    for (int i = 0; i < 6; ++i) {  // rising: moving, so no kick yet
        jumpObs.playerY = 150 + 40 * i; jumpObs.playerX = 300 + i;
        const auto rising = jumper.step(jumpObs);
        kicked |= pressedA(rising);
        for (const auto& press : rising.presses) released |= press.button != Button::A && !press.down;
    }
    check(!kicked && !logged("wall kick at"), "no wall kick while Mario still moves");
    check(steered && !released, "the stick pushes into the wall from the settle through the rise");
    jumpObs.playerX = 310; jumpObs.playerY = 400; jumper.step(jumpObs);
    const auto cling = jumper.step(jumpObs);
    bool releasedA = false;
    for (const auto& press : cling.presses) releasedA |= press.button == Button::A && !press.down;
    check(releasedA && !pressedA(cling) && logged("wall kick at waypoint 3"), "clinging releases the hop's still-held A");
    bool repressed = false;
    for (int i = 0; i < 3; ++i) repressed |= pressedA(jumper.step(jumpObs));
    check(repressed, "the wall kick presses A again while clinging");
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
        for (int i = 0; i < 60 && jumper.result() == Result::Running; ++i) jumper.step(jumpObs);
    }
    check(jumper.result() == Result::Fail, "repeated missed kicks fail instead of looping");
    // Kicked onto the landing level early (over the next wall's top): the chain is done.
    DomesDriver lander(10000, config); Observation landObs;
    enterDome(lander, landObs, true);
    lander.step(landObs);
    landObs.playerX = 150; landObs.playerY = 100; landObs.playerOnGround = false;
    for (int i = 0; i < 3; ++i) lander.step(landObs);
    landObs.playerOnGround = true; lander.step(landObs);
    landObs.playerX = 300;
    const auto landerLogged = [&](const char* text) {
        for (const auto& line : lander.log()) if (line.find(text) != std::string::npos) return true;
        return false;
    };
    for (int i = 0; i < 15 && !landerLogged("jump and spin at waypoint 2"); ++i) lander.step(landObs);
    landObs.playerOnGround = false; landObs.playerY = 500; lander.step(landObs);
    landObs.playerX = 320; landObs.playerY = 690; landObs.playerZ = 400; landObs.playerOnGround = true;
    bool done = false;
    for (int i = 0; i < 5 && !done; ++i) {
        lander.step(landObs);
        done = landerLogged("reached its landing level");
    }
    check(done && lander.result() == Result::Running, "landing on the chain's destination level skips the remaining kicks");
    check(landerLogged("continuing to waypoint 5"), "resumes at the nearest point on the landing level");
    // Walked off the landing level soon after (a corner kick that landed on a cut-off
    // ledge): retry the chain from its hop instead of looping on the lower level.
    landObs.playerY = 100; landObs.playerOnGround = true;
    lander.step(landObs);
    check(landerLogged("retrying the chain from waypoint 2") && lander.result() == Result::Running,
          "falling off the chain's landing level retries the chain from its hop");
    DomesDriver farLander(10000, config); Observation farObs;
    enterDome(farLander, farObs, true);
    farLander.step(farObs);
    farObs.playerX = 150; farObs.playerY = 100; farObs.playerOnGround = false;
    for (int i = 0; i < 3; ++i) farLander.step(farObs);
    farObs.playerOnGround = true; farLander.step(farObs);
    farObs.playerX = 300;
    const auto farLogged = [&](const char* text) {
        for (const auto& line : farLander.log()) if (line.find(text) != std::string::npos) return true;
        return false;
    };
    for (int i = 0; i < 15 && !farLogged("jump and spin at waypoint 2"); ++i) farLander.step(farObs);
    farObs.playerOnGround = false; farObs.playerY = 500; farLander.step(farObs);
    farObs.playerX = 1250; farObs.playerY = 700; farObs.playerZ = 300; farObs.playerOnGround = true;
    bool farDone = false;
    for (int i = 0; i < 5 && !farDone; ++i) {
        farLander.step(farObs);
        farDone = farLogged("continuing to waypoint 7");
    }
    check(farDone, "a ledge grab on the far side resumes at the nearest later point");
    // Live-verified routes (native/SAVES.md evidence): a regenerated plan must
    // never replace or drop them. Update these only when promoting a new route.
    const struct { int dome; size_t points; int hops, kicks, launches; } verified[] = {
        {1, 56, 0, 0, 0}, {2, 103, 0, 0, 0}, {3, 119, 0, 0, 0}, {4, 106, 0, 0, 0}, {5, 307, 3, 2, 1}, {6, 117, 0, 0, 0}};
    for (const auto& expected : verified) {
        const auto& route = verifiedDomeRoute(expected.dome);
        check(route.size() == expected.points, "verified dome route present with its promoted size");
        check(&domeRoute(expected.dome) == &route, "domeRoute uses the verified route");
        int hops = 0, kicks = 0, launches = 0;
        for (const auto& point : route) {
            hops += point.action == DomeWaypoint::Hop;
            kicks += point.action == DomeWaypoint::Kick;
            launches += point.action == DomeWaypoint::Launch;
        }
        check(hops == expected.hops && kicks == expected.kicks && launches == expected.launches, "verified route keeps its actions");
    }
    unlink(routePath);
    std::printf("%d dome/finale checks passed\n", checks);
}
