#include "petari/platform/mod_folder.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <system_error>
#include <vector>

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

namespace {
std::uint32_t be32(const unsigned char* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

std::string extensionOf(const std::string& path) {
    const auto dot = path.rfind('.');
    return dot == std::string::npos ? "" : upper(path.substr(dot));
}

// The first `want` bytes of a Yaz0 payload (after its 16-byte header), or fewer if it is malformed.
std::vector<unsigned char> yaz0Head(const std::vector<unsigned char>& in, std::size_t want) {
    std::vector<unsigned char> out;
    std::size_t at = 16;
    while (out.size() < want && at < in.size()) {
        const unsigned char flags = in[at++];
        for (int bit = 7; bit >= 0 && out.size() < want; --bit) {
            if (flags & (1 << bit)) {
                if (at >= in.size()) return out;
                out.push_back(in[at++]);
            } else {
                if (at + 1 >= in.size()) return out;
                const unsigned char a = in[at++], b = in[at++];
                std::size_t length = a >> 4;
                const std::size_t distance = (std::size_t(a & 15) << 8 | b) + 1;
                if (length == 0) {
                    if (at >= in.size()) return out;
                    length = std::size_t(in[at++]) + 18;
                } else {
                    length += 2;
                }
                if (distance > out.size()) return out;  // a back-reference before the start
                for (std::size_t i = 0; i < length && out.size() < want; ++i) out.push_back(out[out.size() - distance]);
            }
        }
    }
    return out;
}

// A RARC header (0x20 bytes) at `h`, whose archive is `available` bytes long.
bool rarcOk(const unsigned char* h, std::size_t available) {
    if (available < 0x40 || std::memcmp(h, "RARC", 4) != 0) return false;
    const std::uint32_t size = be32(h + 4), headerLength = be32(h + 8), dataOffset = be32(h + 12), dataLength = be32(h + 16);
    return headerLength == 0x20 && size >= 0x40 && size <= available && dataOffset >= 0x20 && dataOffset <= size &&
           dataLength <= size && std::uint64_t(dataOffset) + 0x20 <= size;
}

// A U8 archive header (0x20 bytes; used by the HomeButton and other system archives): magic 55 AA 38 2D, the root node at
// 0x20, a node table that fits the file, and a data offset inside it.
bool u8Ok(const unsigned char* h, std::size_t available) {
    if (available < 0x30 || be32(h) != 0x55AA382Du) return false;
    const std::uint32_t rootOffset = be32(h + 4), tableSize = be32(h + 8), dataOffset = be32(h + 12);
    return rootOffset == 0x20 && tableSize >= 12 && std::uint64_t(rootOffset) + tableSize <= available && dataOffset >= rootOffset &&
           dataOffset <= available;
}

bool archiveOk(const unsigned char* h, std::size_t available) {
    return rarcOk(h, available) || u8Ok(h, available);
}
}  // namespace

std::string invalidKind(const fs::path& host, const std::string& discPath) {
    const std::string extension = extensionOf(discPath);
    const bool archive = extension == ".ARC" || extension == ".SZS";
    const bool message = extension == ".BMG";
    const bool stream = extension == ".BRSTM";
    if (!archive && !message && !stream) return "";
    std::error_code ec;
    const std::uintmax_t length = fs::file_size(host, ec);
    std::ifstream in(host, std::ios::binary);
    if (ec || !in) return archive ? "archive" : message ? "message file" : "stream";
    const std::size_t want = static_cast<std::size_t>(std::min<std::uintmax_t>(length, archive ? 0x2000 : 16));
    std::vector<unsigned char> head(want);
    in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(want));
    head.resize(static_cast<std::size_t>(std::max<std::streamsize>(in.gcount(), 0)));
    if (message) return head.size() >= 16 && std::memcmp(head.data(), "MESGbmg1", 8) == 0 ? "" : "message file";
    if (stream) return head.size() >= 16 && std::memcmp(head.data(), "RSTM", 4) == 0 ? "" : "stream";
    if (head.size() >= 16 && std::memcmp(head.data(), "Yaz0", 4) == 0) {
        const std::uint32_t decompressed = be32(head.data() + 4);
        if (decompressed < 0x40 || decompressed > (256u << 20) || length < 17) return "archive";
        const auto first = yaz0Head(head, 0x20);
        if (first.size() != 0x20) return "archive";
        // .szs also holds non-archive Yaz0 data (the JAudio sound archives); only .arc must decompress to RARC or U8.
        return extension == ".SZS" || archiveOk(first.data(), decompressed) ? "" : "archive";
    }
    return archiveOk(head.data(), static_cast<std::size_t>(length)) ? "" : "archive";
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
    struct Rejected { std::string mod, path, kind; };
    std::vector<Rejected> rejected;
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
            if (const std::string kind = invalidKind(host, path); !kind.empty()) {
                rejected.push_back({mod.name, path, kind});
                return;  // a broken file must not replace a working one
            }
            const auto [it, inserted] = owner.emplace(upper(path), out.files.size());
            if (inserted) {
                out.files.push_back({path, host, mod.name});
            } else {
                out.log.push_back("conflict " + path + ": " + out.files[it->second].mod + " wins over " + mod.name);
            }
        });
    }
    for (const Rejected& r : rejected) {
        const auto it = owner.find(upper(r.path));
        out.log.push_back(r.mod + ": " + r.path + " is not a valid " + r.kind + "; using " +
                          (it == owner.end() ? std::string("the disc file") : "the file from mod " + out.files[it->second].mod));
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
