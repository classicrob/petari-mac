#include "petari/platform/mod_folder.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace PetariNative::Platform::ModFolder {
namespace {

std::string trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r");
    if (begin == std::string::npos) return "";
    return s.substr(begin, s.find_last_not_of(" \t\r") - begin + 1);
}

std::string upper(std::string s) {
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - ('a' - 'A'));
    }
    return s;
}

bool isHostMetadata(const std::string& name) {
    return name == ".DS_Store" || name.rfind("._", 0) == 0 || name == "Thumbs.db";
}

void readModText(const fs::path& file, ModInfo& mod) {
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = upper(trim(line.substr(0, eq)));
        const std::string value = trim(line.substr(eq + 1));
        if (key == "NAME") mod.title = value;
        else if (key == "DESCRIPTION") mod.description = value;
        else if (key == "PRIORITY") {
            char* end = nullptr;
            const long parsed = std::strtol(value.c_str(), &end, 10);
            if (end != value.c_str() && *end == '\0') mod.priority = static_cast<int>(std::clamp(parsed, -1000000L, 1000000L));
        }
    }
}

// Calls visit(relative path, host path) for every regular file below root.
template <class Visit>
void walk(const fs::path& root, Visit&& visit) {
    std::error_code ec;
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (isHostMetadata(it->path().filename().string())) continue;
        std::error_code status;
        if (it->is_regular_file(status)) files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    for (const fs::path& file : files) {
        visit(fs::relative(file, root).generic_string(), file);
    }
}

}  // namespace

std::string normalizePath(const std::string& path) {
    std::vector<std::string> parts;
    std::string current;
    auto flush = [&]() -> bool {
        if (current.empty() || current == ".") {
            current.clear();
            return true;
        }
        if (current == "..") return false;
        parts.push_back(current);
        current.clear();
        return true;
    };
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!flush()) return "";
        } else {
            current += c;
        }
    }
    if (!flush() || parts.empty()) return "";
    std::string out;
    for (const std::string& part : parts) {
        if (!out.empty()) out += '/';
        out += part;
    }
    return out;
}

std::vector<ModInfo> discover(const std::vector<fs::path>& directories) {
    std::vector<ModInfo> mods;
    std::set<std::string> seen;
    for (const fs::path& directory : directories) {
        std::error_code ec;
        std::vector<fs::path> folders;
        for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code status;
            if (it->is_directory(status) && !isHostMetadata(it->path().filename().string())) folders.push_back(it->path());
        }
        std::sort(folders.begin(), folders.end());
        for (const fs::path& folder : folders) {
            ModInfo mod;
            mod.name = folder.filename().string();
            // A name with '=' or a newline cannot be a mods.txt key.
            if (mod.name.empty() || mod.name[0] == '.' || mod.name.find_first_of("=\r\n#") != std::string::npos) continue;
            if (!seen.insert(upper(mod.name)).second) continue;
            mod.title = mod.name;
            mod.root = folder;
            mod.source = directory.string();
            readModText(folder / "mod.txt", mod);
            if (mod.title.empty()) mod.title = mod.name;
            const fs::path files = folder / "files";
            std::error_code dirError;
            if (fs::is_directory(files, dirError)) {
                walk(files, [&](const std::string& relative, const fs::path&) {
                    if (!normalizePath(relative).empty()) ++mod.fileCount;
                });
            }
            mods.push_back(std::move(mod));
        }
    }
    return mods;
}

std::map<std::string, bool> parseStates(const std::string& text) {
    std::map<std::string, bool> states;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.rfind("Folder.", 0) != 0) continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string name = trim(line.substr(7, eq - 7));
        const std::string value = trim(line.substr(eq + 1));
        if (!name.empty() && (value == "on" || value == "off")) states[name] = value == "on";
    }
    return states;
}

std::map<std::string, bool> readStates(const fs::path& modsFile) {
    std::ifstream in(modsFile);
    if (!in) return {};
    std::stringstream text;
    text << in.rdbuf();
    return parseStates(text.str());
}

bool writeState(const fs::path& modsFile, const std::string& name, bool on, std::string* error) {
    std::vector<std::string> lines;
    {
        std::ifstream in(modsFile);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }
    const std::string key = "Folder." + name;
    const std::string replacement = key + (on ? "=on" : "=off");
    bool found = false;
    for (std::string& line : lines) {
        const std::string t = trim(line);
        if (t.rfind("Folder.", 0) == 0) {
            const auto eq = t.find('=');
            if (eq != std::string::npos && trim(t.substr(0, eq)) == key) {
                line = replacement;
                found = true;
            }
        }
    }
    if (!found) lines.push_back(replacement);
    std::error_code ec;
    if (modsFile.has_parent_path()) fs::create_directories(modsFile.parent_path(), ec);
    std::ofstream out(modsFile, std::ios::trunc);
    for (const std::string& line : lines) out << line << '\n';
    if (!out) {
        if (error) *error = "cannot write " + modsFile.string();
        return false;
    }
    return true;
}

std::map<std::string, bool> effectiveStates(const fs::path& modsFile) {
    std::map<std::string, bool> states = readStates(modsFile);
    if (const char* env = std::getenv("PETARI_MOD_FOLDERS"); env != nullptr && env[0] != '\0') {
        for (auto& [name, on] : states) on = false;
        std::istringstream names(env);
        std::string item;
        while (std::getline(names, item, ',')) {
            item = trim(item);
            if (!item.empty() && item != "none") states[item] = true;
        }
    }
    return states;
}

Resolved resolve(const std::vector<ModInfo>& mods, const std::map<std::string, bool>& states) {
    Resolved out;
    std::vector<const ModInfo*> enabled;
    for (const ModInfo& mod : mods) {
        const auto it = states.find(mod.name);
        if (it != states.end() && it->second) enabled.push_back(&mod);
    }
    std::stable_sort(enabled.begin(), enabled.end(), [](const ModInfo* a, const ModInfo* b) {
        return a->priority != b->priority ? a->priority > b->priority : a->name < b->name;
    });
    std::map<std::string, std::size_t> owner;  // upper-cased normalized path -> index in out.files
    for (std::size_t rank = 0; rank < enabled.size(); ++rank) {
        const ModInfo& mod = *enabled[rank];
        out.log.push_back("mod " + mod.name + " (priority " + std::to_string(mod.priority) + ", rank " +
                          std::to_string(rank + 1) + " of " + std::to_string(enabled.size()) + "): " +
                          std::to_string(mod.fileCount) + " files");
        const fs::path files = mod.root / "files";
        std::error_code ec;
        if (!fs::is_directory(files, ec)) {
            out.log.push_back("mod " + mod.name + ": no files/ directory, nothing to apply");
            continue;
        }
        walk(files, [&](const std::string& relative, const fs::path& host) {
            const std::string path = normalizePath(relative);
            if (path.empty()) return;
            const auto [it, inserted] = owner.emplace(upper(path), out.files.size());
            if (inserted) {
                out.files.push_back({path, host, mod.name});
            } else {
                out.log.push_back("conflict " + path + ": " + out.files[it->second].mod + " wins over " + mod.name);
            }
        });
    }
    return out;
}

std::vector<fs::path> searchDirectories(const fs::path& userDirectory) {
    std::vector<fs::path> directories{userDirectory / "mods"};
    if (const char* env = std::getenv("PETARI_MOD_DIRS"); env != nullptr && env[0] != '\0') {
        std::istringstream list(env);
        std::string item;
        while (std::getline(list, item, ':')) {
            if (!item.empty()) directories.emplace_back(item);
        }
    }
    return directories;
}

namespace {
std::mutex gUserMutex;
fs::path gUser;
}  // namespace

Resolved resolveForUser(const fs::path& userDirectory) {
    {
        std::lock_guard<std::mutex> lock(gUserMutex);
        gUser = userDirectory;
    }
    const auto mods = discover(searchDirectories(userDirectory));
    return resolve(mods, effectiveStates(userDirectory / "mods.txt"));
}

std::vector<ModInfo> detected() {
    fs::path user;
    {
        std::lock_guard<std::mutex> lock(gUserMutex);
        user = gUser;
    }
    return user.empty() ? std::vector<ModInfo>{} : discover(searchDirectories(user));
}

std::map<std::string, bool> detectedStates() {
    std::lock_guard<std::mutex> lock(gUserMutex);
    return gUser.empty() ? std::map<std::string, bool>{} : effectiveStates(gUser / "mods.txt");
}

bool setEnabled(const std::string& name, bool on, std::string* error) {
    std::lock_guard<std::mutex> lock(gUserMutex);
    if (gUser.empty()) {
        if (error) *error = "no user directory";
        return false;
    }
    return writeState(gUser / "mods.txt", name, on, error);
}

}  // namespace PetariNative::Platform::ModFolder
