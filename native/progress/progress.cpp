#include "petari/progress.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

#include "petari/host_allocation.hpp"

namespace PetariNative::Progress {
namespace {

namespace fs = std::filesystem;

std::mutex gMutex;
std::map<std::string, Mission> gMissions;  // key "Stage:mission"
fs::path gFile;
std::string (*gClock)() = nullptr;

// Badges: published during one frame, read by the overlay.
struct BadgeStore {
    std::string stage;
    std::vector<std::pair<int, std::pair<float, float>>> stars;
    unsigned long stamp = 0;  // gFrames when last published
    bool any = false;
} gBadges;
unsigned long gFrames = 0;
bool gBadgesOn = true;

fs::path settingsFile() {
    return gFile.empty() ? fs::path() : gFile.parent_path() / "progress_settings.txt";
}

// The run being timed.
struct RunState {
    bool active = false;
    std::string stage;
    int scenario = 0;
    unsigned long sceneId = 0;
    unsigned long frames = 0;
    int deaths = 0;
    bool wasDead = false;
    int coins = 0, starBits = 0, purpleCoins = 0;
} gRun;

std::string key(const std::string& stage, int mission) {
    return stage + ":" + std::to_string(mission);
}

std::string systemNow() {
    const std::time_t t = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&t, &utc);
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return text;
}

std::string now() {
    return gClock ? gClock() : systemNow();
}

// --- tiny JSON: objects, strings, numbers, booleans, null (all this file needs) ---
struct Json {
    enum Kind { Null, Bool, Number, String, Object } kind = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::map<std::string, Json> o;
};

struct Reader {
    const std::string& t;
    size_t i = 0;
    std::string error;
    void ws() { while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) ++i; }
    bool fail(const std::string& m) { if (error.empty()) error = m + " at byte " + std::to_string(i); return false; }
    bool str(std::string& out) {
        if (i >= t.size() || t[i] != '"') return fail("expected a string");
        ++i;
        out.clear();
        while (i < t.size() && t[i] != '"') {
            if (t[i] == '\\') {
                if (++i >= t.size()) break;
                switch (t[i]) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'u': out += '?'; i += 4; break;  // never written by this file
                default: out += t[i];
                }
            } else {
                out += t[i];
            }
            ++i;
        }
        if (i >= t.size()) return fail("unterminated string");
        ++i;
        return true;
    }
    bool value(Json& out, int depth = 0) {
        if (depth > 16) return fail("nested too deeply");
        ws();
        if (i >= t.size()) return fail("unexpected end");
        if (t[i] == '{') {
            ++i;
            out.kind = Json::Object;
            ws();
            if (i < t.size() && t[i] == '}') { ++i; return true; }
            while (true) {
                ws();
                std::string name;
                if (!str(name)) return false;
                ws();
                if (i >= t.size() || t[i] != ':') return fail("expected ':'");
                ++i;
                if (!value(out.o[name], depth + 1)) return false;
                ws();
                if (i < t.size() && t[i] == ',') { ++i; continue; }
                if (i < t.size() && t[i] == '}') { ++i; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (t[i] == '"') { out.kind = Json::String; return str(out.s); }
        if (t.compare(i, 4, "true") == 0) { out.kind = Json::Bool; out.b = true; i += 4; return true; }
        if (t.compare(i, 5, "false") == 0) { out.kind = Json::Bool; out.b = false; i += 5; return true; }
        if (t.compare(i, 4, "null") == 0) { out.kind = Json::Null; i += 4; return true; }
        char* end = nullptr;
        const double v = std::strtod(t.c_str() + i, &end);
        if (end == t.c_str() + i) return fail("unexpected character");
        out.kind = Json::Number;
        out.n = v;
        i = static_cast<size_t>(end - t.c_str());
        return true;
    }
};

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (static_cast<unsigned char>(c) < 0x20) out += ' ';
        else out += c;
    }
    return out + "\"";
}

std::string num(double v) {
    char text[40];
    std::snprintf(text, sizeof(text), "%.3f", v);
    return text;
}

double numberOr(const Json& o, const char* name, double fallback) {
    const auto it = o.o.find(name);
    return it != o.o.end() && it->second.kind == Json::Number ? it->second.n : fallback;
}
std::string stringOr(const Json& o, const char* name) {
    const auto it = o.o.find(name);
    return it != o.o.end() && it->second.kind == Json::String ? it->second.s : std::string();
}

std::string serializeLocked() {
    std::ostringstream out;
    out << "{\n  \"version\": 1,\n  \"missions\": {";
    bool first = true;
    for (const auto& [k, m] : gMissions) {
        out << (first ? "\n" : ",\n") << "    " << quote(k) << ": {"
            << "\"stage\": " << quote(m.stage) << ", \"mission\": " << m.mission << ", \"grand\": " << (m.grand ? "true" : "false")
            << ", \"first_clear\": " << quote(m.firstClear) << ", \"last_clear\": " << quote(m.lastClear)
            << ", \"clears\": " << m.clears << ", \"best_time_s\": " << num(m.bestTimeS)
            << ", \"fewest_deaths\": " << m.fewestDeaths << ", \"best_coins\": " << m.bestCoins
            << ", \"best_star_bits\": " << m.bestStarBits << ", \"best_purple_coins\": " << m.bestPurpleCoins
            << ", \"last_run\": {\"time_s\": " << num(m.last.timeS) << ", \"deaths\": " << m.last.deaths
            << ", \"coins\": " << m.last.coins << ", \"star_bits\": " << m.last.starBits
            << ", \"purple_coins\": " << m.last.purpleCoins << "}}";
        first = false;
    }
    out << (first ? "}\n}\n" : "\n  }\n}\n");
    return out.str();
}

bool parseLocked(const std::string& text, std::map<std::string, Mission>& out, std::string* error) {
    Reader reader{text};
    Json root;
    if (!reader.value(root)) {
        *error = reader.error;
        return false;
    }
    if (root.kind != Json::Object) {
        *error = "expected an object";
        return false;
    }
    const auto version = root.o.find("version");
    if (version == root.o.end() || version->second.kind != Json::Number || version->second.n != 1) {
        *error = "unsupported or missing version";
        return false;
    }
    const auto missions = root.o.find("missions");
    if (missions == root.o.end() || missions->second.kind != Json::Object) {
        *error = "missing \"missions\"";
        return false;
    }
    for (const auto& [k, j] : missions->second.o) {
        if (j.kind != Json::Object) {
            *error = "mission " + k + " is not an object";
            return false;
        }
        Mission m;
        m.stage = stringOr(j, "stage");
        m.mission = static_cast<int>(numberOr(j, "mission", 0));
        if (m.stage.empty() || m.mission < 1) {
            *error = "mission " + k + " lacks stage or mission";
            return false;
        }
        const auto grand = j.o.find("grand");
        m.grand = grand != j.o.end() && grand->second.kind == Json::Bool && grand->second.b;
        m.firstClear = stringOr(j, "first_clear");
        m.lastClear = stringOr(j, "last_clear");
        m.clears = static_cast<int>(numberOr(j, "clears", 0));
        m.bestTimeS = numberOr(j, "best_time_s", 0);
        m.fewestDeaths = static_cast<int>(numberOr(j, "fewest_deaths", 0));
        m.bestCoins = static_cast<int>(numberOr(j, "best_coins", 0));
        m.bestStarBits = static_cast<int>(numberOr(j, "best_star_bits", 0));
        m.bestPurpleCoins = static_cast<int>(numberOr(j, "best_purple_coins", 0));
        const auto last = j.o.find("last_run");
        if (last != j.o.end() && last->second.kind == Json::Object) {
            m.last.timeS = numberOr(last->second, "time_s", 0);
            m.last.deaths = static_cast<int>(numberOr(last->second, "deaths", 0));
            m.last.coins = static_cast<int>(numberOr(last->second, "coins", 0));
            m.last.starBits = static_cast<int>(numberOr(last->second, "star_bits", 0));
            m.last.purpleCoins = static_cast<int>(numberOr(last->second, "purple_coins", 0));
        }
        out[key(m.stage, m.mission)] = std::move(m);
    }
    return true;
}

bool saveLocked(std::string* error) {
    if (gFile.empty()) return true;
    std::error_code ec;
    if (gFile.has_parent_path()) fs::create_directories(gFile.parent_path(), ec);
    // Atomic: a crash while saving never leaves a half-written record.
    const fs::path temporary = fs::path(gFile.string() + ".tmp");
    {
        std::ofstream out(temporary, std::ios::trunc);
        out << serializeLocked();
        if (!out) {
            if (error) *error = "cannot write " + temporary.string();
            return false;
        }
    }
    fs::rename(temporary, gFile, ec);
    if (ec) {
        if (error) *error = "cannot replace " + gFile.string() + ": " + ec.message();
        return false;
    }
    return true;
}

}  // namespace

bool find(const std::string& stage, int mission, Mission* out) {
    std::lock_guard<std::mutex> lock(gMutex);
    const auto it = gMissions.find(key(stage, mission));
    if (it == gMissions.end()) return false;
    if (out) *out = it->second;
    return true;
}

int clearedCount() {
    std::lock_guard<std::mutex> lock(gMutex);
    return static_cast<int>(gMissions.size());
}

std::vector<Mission> all() {
    std::lock_guard<std::mutex> lock(gMutex);
    std::vector<Mission> out;
    for (const auto& [k, m] : gMissions) out.push_back(m);
    return out;
}

void reset() {
    HostAllocationScope host;
    std::lock_guard<std::mutex> lock(gMutex);
    gMissions.clear();
    std::error_code ec;
    if (!gFile.empty()) fs::remove(gFile, ec);
}

bool load(const fs::path& file, std::string* error) {
    HostAllocationScope host;
    std::lock_guard<std::mutex> lock(gMutex);
    std::map<std::string, Mission> loaded;
    std::ifstream in(file);
    if (in) {
        std::stringstream text;
        text << in.rdbuf();
        std::string problem;
        if (!parseLocked(text.str(), loaded, &problem)) {
            if (error) *error = file.string() + ": " + problem;
            return false;
        }
    }
    gMissions = std::move(loaded);
    gFile = file;
    gBadgesOn = true;
    std::ifstream settings(settingsFile());
    std::string line;
    while (std::getline(settings, line)) {
        if (line.rfind("badges=", 0) == 0) gBadgesOn = line.substr(7, 3) != "off";
    }
    return true;
}

bool save(std::string* error) {
    HostAllocationScope host;
    std::lock_guard<std::mutex> lock(gMutex);
    return saveLocked(error);
}

std::string serialize() {
    std::lock_guard<std::mutex> lock(gMutex);
    return serializeLocked();
}

bool parse(const std::string& text, std::string* error) {
    HostAllocationScope host;
    std::map<std::string, Mission> parsed;
    std::string problem;
    if (!parseLocked(text, parsed, &problem)) {
        if (error) *error = problem;
        return false;
    }
    std::lock_guard<std::mutex> lock(gMutex);
    gMissions = std::move(parsed);
    return true;
}

void frame(const FrameState& s) {
    std::lock_guard<std::mutex> lock(gMutex);
    ++gFrames;
    if (!s.inGame) {
        gRun.active = false;  // the next Game scene starts a new run
        return;
    }
    // A new entry: another stage or mission, or the scene created again (retry, return from a result).
    if (!gRun.active || gRun.stage != s.stage || gRun.scenario != s.scenario || gRun.sceneId != s.sceneId) {
        gRun = RunState{};
        gRun.active = true;
        gRun.stage = s.stage;
        gRun.scenario = s.scenario;
        gRun.sceneId = s.sceneId;
    }
    if (!s.paused && !s.demo) ++gRun.frames;
    if (s.dead && !gRun.wasDead) ++gRun.deaths;
    gRun.wasDead = s.dead;
    gRun.coins = s.coins;
    gRun.starBits = s.starBits;
    gRun.purpleCoins = s.purpleCoins;
}

void starGet(int mission, bool grand) {
    HostAllocationScope host;
    std::lock_guard<std::mutex> lock(gMutex);
    if (!gRun.active || mission < 1) return;  // not in a tracked stage entry
    Mission& m = gMissions[key(gRun.stage, mission)];
    const bool first = m.clears == 0;
    const double time = static_cast<double>(gRun.frames) / kFramesPerSecond;
    const std::string stamp = now();
    if (first) {
        m.stage = gRun.stage;
        m.mission = mission;
        m.firstClear = stamp;
        m.bestTimeS = time;
        m.fewestDeaths = gRun.deaths;
    } else {
        m.bestTimeS = std::min(m.bestTimeS, time);
        m.fewestDeaths = std::min(m.fewestDeaths, gRun.deaths);
    }
    m.grand = grand;
    m.lastClear = stamp;
    ++m.clears;
    m.bestCoins = std::max(m.bestCoins, gRun.coins);
    m.bestStarBits = std::max(m.bestStarBits, gRun.starBits);
    m.bestPurpleCoins = std::max(m.bestPurpleCoins, gRun.purpleCoins);
    m.last = {time, gRun.deaths, gRun.coins, gRun.starBits, gRun.purpleCoins};
    std::string error;
    if (!saveLocked(&error)) std::fprintf(stderr, "[progress] %s\n", error.c_str());
    std::fprintf(stderr, "[progress] cleared %s mission %d: %.2f s, %d deaths, %d coins, %d star bits, %d purple coins (clear %d)\n",
                 gRun.stage.c_str(), mission, time, gRun.deaths, gRun.coins, gRun.starBits, gRun.purpleCoins, m.clears);
}

double currentRunTimeS() {
    std::lock_guard<std::mutex> lock(gMutex);
    return static_cast<double>(gRun.frames) / kFramesPerSecond;
}

int currentRunDeaths() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gRun.deaths;
}

void publishBadge(const char* stage, int mission, float u, float v) {
    HostAllocationScope host;
    std::lock_guard<std::mutex> lock(gMutex);
    if (!gBadges.any || gBadges.stamp != gFrames || gBadges.stage != stage) {
        gBadges.stage = stage;
        gBadges.stars.clear();
        gBadges.stamp = gFrames;
        gBadges.any = true;
    }
    for (auto& star : gBadges.stars) {
        if (star.first == mission) {
            star.second = {u, v};
            return;
        }
    }
    gBadges.stars.push_back({mission, {u, v}});
}

bool currentBadges(std::vector<BadgeStar>* out, std::string* stage) {
    std::lock_guard<std::mutex> lock(gMutex);
    static const bool trace = std::getenv("PETARI_PROGRESS_TRACE") != nullptr;
    if (trace && gBadges.any && gFrames - gBadges.stamp <= 2) {
        static unsigned long lastReport = 0;
        if (gFrames - lastReport > 120) {
            lastReport = gFrames;
            std::fprintf(stderr, "[progress] badges: %zu stars of %s, on=%d, record has %zu missions\n", gBadges.stars.size(),
                         gBadges.stage.c_str(), gBadgesOn ? 1 : 0, gMissions.size());
        }
    }
    if (!gBadgesOn || !gBadges.any || gFrames - gBadges.stamp > 2) return false;
    out->clear();
    if (stage) *stage = gBadges.stage;
    for (const auto& [mission, position] : gBadges.stars) {
        BadgeStar star;
        star.mission = mission;
        star.u = position.first;
        star.v = position.second;
        const auto it = gMissions.find(key(gBadges.stage, mission));
        if (it != gMissions.end()) {
            star.cleared = true;
            star.bestTimeS = it->second.bestTimeS;
            star.clears = it->second.clears;
        }
        out->push_back(star);
    }
    return !out->empty();
}

bool badgesEnabled() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gBadgesOn;
}

void setBadgesEnabled(bool on) {
    HostAllocationScope host;
    std::lock_guard<std::mutex> lock(gMutex);
    gBadgesOn = on;
    const fs::path file = settingsFile();
    if (file.empty()) return;
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::trunc);
    out << "badges=" << (on ? "on" : "off") << "\n";
}

void setClockForTesting(std::string (*clock)()) {
    std::lock_guard<std::mutex> lock(gMutex);
    gClock = clock;
}

void resetForTesting() {
    std::lock_guard<std::mutex> lock(gMutex);
    gMissions.clear();
    gFile.clear();
    gRun = RunState{};
    gClock = nullptr;
    gBadges = BadgeStore{};
    gFrames = 0;
    gBadgesOn = true;
}

}  // namespace PetariNative::Progress
