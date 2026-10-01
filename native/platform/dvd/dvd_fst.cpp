#include "dvd_fst.hpp"

#include <algorithm>
#include <cstring>
#include <system_error>


namespace fs = std::filesystem;

namespace PetariNative::Platform::DVD {
namespace {

constexpr std::uint32_t kEntrySize = 12;
constexpr std::uint32_t kMaxStringOffset = 0x00FFFFFF;
constexpr std::uint64_t kSyntheticDataStart = 0x00100000;
constexpr std::uint64_t kSyntheticAlignment = 0x8000;
constexpr std::uint64_t kMaxDiscBytes = std::uint64_t(0xFFFFFFFF) << 2;
constexpr int kMaxDirectoryDepth = 64;
constexpr std::size_t kMaxReportedProblems = 16;

// The SDK lower-cases with the "C" locale table, which only maps ASCII.
int asciiLower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

std::string asciiUpper(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - ('a' - 'A'));
        }
    }
    return out;
}

bool isSame(const char* path, const char* str) {
    while (*str != '\0') {
        if (asciiLower(static_cast<unsigned char>(*path++)) != asciiLower(static_cast<unsigned char>(*str++))) {
            return false;
        }
    }
    return *path == '/' || *path == '\0';
}

// Rejects names that would escape the files/ directory or cannot be host names.
bool isSafeComponent(const char* name) {
    return name[0] != '\0' && std::strcmp(name, ".") != 0 && std::strcmp(name, "..") != 0 && std::strchr(name, '/') == nullptr;
}

bool isHostMetadata(const std::string& name) {
    return name == ".DS_Store" || name.rfind("._", 0) == 0;
}

class Problems {
public:
    void add(std::string message) {
        if (mList.size() < kMaxReportedProblems) {
            mList.push_back(std::move(message));
        }
        ++mCount;
    }
    bool empty() const { return mCount == 0; }
    std::string describe(const char* heading) const {
        std::string out = heading;
        for (const std::string& line : mList) {
            out += "\n  ";
            out += line;
        }
        if (mCount > mList.size()) {
            out += "\n  ... and " + std::to_string(mCount - mList.size()) + " more";
        }
        return out;
    }

private:
    std::vector<std::string> mList;
    std::size_t mCount = 0;
};

void setError(std::string* error, std::string message) {
    if (error) {
        *error = std::move(message);
    }
}

}  // namespace

bool Fst::fromDiscImage(const std::vector<std::uint8_t>& image, const fs::path& filesDirectory, Fst& out, std::string* error) {
    Fst fst;
    if (image.size() < kEntrySize) {
        setError(error, "fst.bin is too small to contain a root entry");
        return false;
    }
    if (image[0] == 0) {
        setError(error, "fst.bin root entry is not a directory");
        return false;
    }
    const std::uint32_t count = readU32BE(&image[8]);
    if (count == 0 || std::uint64_t(count) * kEntrySize > image.size()) {
        setError(error, "fst.bin entry count " + std::to_string(count) + " does not fit in " + std::to_string(image.size()) + " bytes");
        return false;
    }

    const std::size_t stringStart = std::size_t(count) * kEntrySize;
    fst.mStrings.assign(image.begin() + static_cast<std::ptrdiff_t>(stringStart), image.end());
    fst.mStrings.push_back('\0');  // guarantees termination of a final unterminated name
    const std::size_t stringBytes = image.size() - stringStart;

    fst.mEntries.resize(count);
    fst.mHostPaths.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* raw = &image[std::size_t(i) * kEntrySize];
        FstEntry& e = fst.mEntries[i];
        e.isDir = raw[0] != 0;
        e.nameOffset = readU24BE(raw + 1);
        e.parentOrPosition = readU32BE(raw + 4);
        e.nextOrLength = readU32BE(raw + 8);
        if (i != 0 && e.nameOffset >= stringBytes) {
            setError(error, "fst.bin entry " + std::to_string(i) + " has a name outside the string table");
            return false;
        }
    }
    fst.mEntries[0].nameOffset = static_cast<std::uint32_t>(fst.mStrings.size() - 1);  // root has no name
    fst.mEntries[0].parentOrPosition = 0;

    // Walk in disc order, tracking the open directories to derive each entry's
    // parent (files do not store one) and host path.
    struct Open {
        std::uint32_t entry;
        fs::path path;
    };
    std::vector<Open> stack{{0, filesDirectory}};
    Problems problems;
    for (std::uint32_t i = 1; i < count; ++i) {
        while (stack.size() > 1 && i >= fst.mEntries[stack.back().entry].nextOrLength) {
            stack.pop_back();
        }
        FstEntry& e = fst.mEntries[i];
        const char* name = fst.name(i);
        if (!isSafeComponent(name)) {
            setError(error, "fst.bin entry " + std::to_string(i) + " has an invalid name '" + name + "'");
            return false;
        }
        fs::path host = stack.back().path / name;
        if (e.isDir) {
            const std::uint32_t parentNext = fst.mEntries[stack.back().entry].nextOrLength;
            if (e.parentOrPosition != stack.back().entry || e.nextOrLength <= i || e.nextOrLength > parentNext) {
                setError(error, "fst.bin directory entry " + std::to_string(i) + " ('" + name + "') has an inconsistent parent or extent");
                return false;
            }
            if (stack.size() > kMaxDirectoryDepth) {
                setError(error, "fst.bin nests directories deeper than " + std::to_string(kMaxDirectoryDepth));
                return false;
            }
            std::error_code ec;
            if (!fs::is_directory(host, ec)) {
                problems.add("missing directory " + host.string());
            }
            stack.push_back({i, host});
        } else {
            std::error_code ec;
            const fs::file_status status = fs::status(host, ec);
            if (!fs::is_regular_file(status)) {
                problems.add("missing file " + host.string());
            } else {
                const std::uintmax_t size = fs::file_size(host, ec);
                if (ec) {
                    problems.add("cannot stat " + host.string() + ": " + ec.message());
                } else if (size != e.nextOrLength) {
                    problems.add(host.string() + " is " + std::to_string(size) + " bytes; the disc FST lists " + std::to_string(e.nextOrLength));
                }
            }
            fst.mHostPaths[i] = std::move(host);
        }
    }
    if (!problems.empty()) {
        setError(error, problems.describe("extracted files do not match sys/fst.bin (mount with ignoreDiscFst to use a modified tree):"));
        return false;
    }
    if (!fst.finish(error)) {
        return false;
    }
    out = std::move(fst);
    return true;
}

bool Fst::fromDirectory(const fs::path& filesDirectory, Fst& out, std::string* error) {
    Fst fst;
    std::string strings;
    std::uint64_t cursor = kSyntheticDataStart;
    Problems problems;

    auto addName = [&](const std::string& name) -> std::uint32_t {
        const std::size_t offset = strings.size();
        strings += name;
        strings += '\0';
        return static_cast<std::uint32_t>(offset);
    };

    fst.mEntries.push_back({true, 0, 0, 0});
    fst.mHostPaths.emplace_back();

    auto scan = [&](auto&& self, const fs::path& dir, std::uint32_t dirEntry, int depth) -> void {
        if (depth > kMaxDirectoryDepth) {
            problems.add("directory nesting deeper than " + std::to_string(kMaxDirectoryDepth) + " at " + dir.string());
            return;
        }
        std::vector<std::string> names;
        std::error_code ec;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            std::string name = it->path().filename().string();
            if (!isHostMetadata(name)) {
                names.push_back(std::move(name));
            }
        }
        if (ec) {
            problems.add("cannot list " + dir.string() + ": " + ec.message());
            return;
        }
        std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
            const std::string ua = asciiUpper(a);
            const std::string ub = asciiUpper(b);
            return ua == ub ? a < b : ua < ub;
        });
        for (const std::string& name : names) {
            const fs::path host = dir / name;
            const fs::file_status status = fs::status(host, ec);
            const std::uint32_t index = static_cast<std::uint32_t>(fst.mEntries.size());
            if (fs::is_directory(status)) {
                fst.mEntries.push_back({true, addName(name), dirEntry, 0});
                fst.mHostPaths.emplace_back();
                self(self, host, index, depth + 1);
                fst.mEntries[index].nextOrLength = static_cast<std::uint32_t>(fst.mEntries.size());
            } else if (fs::is_regular_file(status)) {
                const std::uintmax_t size = fs::file_size(host, ec);
                if (ec) {
                    problems.add("cannot stat " + host.string() + ": " + ec.message());
                    continue;
                }
                if (size > 0xFFFFFFFFu) {
                    problems.add(host.string() + " exceeds the 4 GiB DVD file limit");
                    continue;
                }
                fst.mEntries.push_back({false, addName(name), static_cast<std::uint32_t>(cursor >> 2), static_cast<std::uint32_t>(size)});
                fst.mHostPaths.push_back(host);
                cursor += (size + kSyntheticAlignment - 1) / kSyntheticAlignment * kSyntheticAlignment;
                if (cursor > kMaxDiscBytes) {
                    problems.add("extracted files exceed the addressable disc size");
                    return;
                }
            } else {
                problems.add(host.string() + " is neither a regular file nor a directory");
            }
        }
    };

    std::error_code ec;
    if (!fs::is_directory(filesDirectory, ec)) {
        setError(error, filesDirectory.string() + " is not a directory");
        return false;
    }
    scan(scan, filesDirectory, 0, 0);
    if (!problems.empty()) {
        setError(error, problems.describe("cannot build an FST from the extracted files:"));
        return false;
    }

    // The root's name is the empty string at the end of the table.
    fst.mEntries[0].nameOffset = addName("");
    if (strings.size() > kMaxStringOffset) {
        setError(error, "file names exceed the 16 MiB FST string table limit");
        return false;
    }
    fst.mEntries[0].nextOrLength = static_cast<std::uint32_t>(fst.mEntries.size());
    fst.mStrings.assign(strings.begin(), strings.end());
    if (!fst.finish(error)) {
        return false;
    }
    out = std::move(fst);
    return true;
}

namespace {
struct OverlayNode {
    std::string name;
    bool isDir = false;
    std::uint32_t position = 0;  // words
    std::uint32_t length = 0;
    fs::path host;
    std::string mod;
    std::vector<OverlayNode> children;
};

bool sameName(const std::string& a, const std::string& b) {
    return asciiUpper(a) == asciiUpper(b);
}
}  // namespace

bool Fst::applyOverlay(const std::vector<OverlayFile>& files, OverlayResult& result, std::string* error) {
    OverlayNode root;
    root.isDir = true;
    // Existing tree, in FST order.
    auto build = [&](auto&& self, std::uint32_t dir, OverlayNode& node) -> void {
        for (std::uint32_t i = dir + 1; i < mEntries[dir].nextOrLength;) {
            const FstEntry& e = mEntries[i];
            OverlayNode child;
            child.name = name(i);
            child.isDir = e.isDir;
            if (e.isDir) {
                self(self, i, child);
                i = e.nextOrLength;
            } else {
                child.position = e.parentOrPosition;
                child.length = e.nextOrLength;
                child.host = mHostPaths[i];
                if (i < mOverlayMods.size()) child.mod = mOverlayMods[i];
                ++i;
            }
            node.children.push_back(std::move(child));
        }
    };
    build(build, 0, root);

    std::uint64_t cursor = mDiscEnd;  // new and grown files go after the last file
    for (const OverlayFile& overlayFile : files) {
        const std::string& path = overlayFile.path;
        const fs::path& host = overlayFile.host;
        std::vector<std::string> parts;
        for (std::size_t at = 0; at <= path.size();) {
            const std::size_t slash = std::min(path.find('/', at), path.size());
            parts.push_back(path.substr(at, slash - at));
            at = slash + 1;
        }
        bool valid = !parts.empty();
        for (const std::string& part : parts) {
            valid = valid && isSafeComponent(part.c_str());
        }
        if (!valid) {
            result.skipped.push_back(path + ": invalid path");
            continue;
        }
        std::error_code ec;
        const std::uintmax_t size = fs::file_size(host, ec);
        if (ec || !fs::is_regular_file(host, ec)) {
            result.skipped.push_back(path + ": cannot read " + host.string());
            continue;
        }
        if (size > 0xFFFFFFFFu) {
            result.skipped.push_back(path + ": exceeds the 4 GiB DVD file limit");
            continue;
        }
        OverlayNode* dir = &root;
        bool conflict = false;
        bool createdDir = false;
        for (std::size_t i = 0; i + 1 < parts.size() && !conflict; ++i) {
            OverlayNode* next = nullptr;
            for (OverlayNode& child : dir->children) {
                if (sameName(child.name, parts[i])) {
                    next = &child;
                    break;
                }
            }
            if (next == nullptr) {
                dir->children.push_back({parts[i], true, 0, 0, {}, {}, {}});
                next = &dir->children.back();
                createdDir = true;
            } else if (!next->isDir) {
                conflict = true;
            }
            dir = next;
        }
        (void)createdDir;
        if (conflict) {
            result.skipped.push_back(path + ": a directory of this path is a file on the disc");
            continue;
        }
        OverlayNode* file = nullptr;
        for (OverlayNode& child : dir->children) {
            if (sameName(child.name, parts.back())) {
                file = &child;
                break;
            }
        }
        if (file != nullptr && file->isDir) {
            result.skipped.push_back(path + ": is a directory on the disc");
            continue;
        }
        const bool fits = file != nullptr && size <= file->length;
        if (file == nullptr) {
            dir->children.push_back({parts.back(), false, 0, 0, {}, {}, {}});
            file = &dir->children.back();
            ++result.added;
        } else {
            ++result.replaced;
        }
        if (!fits) {
            file->position = static_cast<std::uint32_t>(cursor >> 2);
            cursor += (size + kSyntheticAlignment - 1) / kSyntheticAlignment * kSyntheticAlignment;
            if (cursor > kMaxDiscBytes) {
                setError(error, "mod files exceed the addressable disc size");
                return false;
            }
        }
        file->length = static_cast<std::uint32_t>(size);
        file->host = host;
        file->mod = overlayFile.mod;
    }

    // Rebuild the tables.
    std::vector<FstEntry> entries;
    std::vector<fs::path> hosts;
    std::vector<std::string> mods;
    std::string strings;
    auto addName = [&](const std::string& n) {
        const std::size_t offset = strings.size();
        strings += n;
        strings += '\0';
        return static_cast<std::uint32_t>(offset);
    };
    entries.push_back({true, 0, 0, 0});
    hosts.emplace_back();
    mods.emplace_back();
    auto emit = [&](auto&& self, const OverlayNode& dir, std::uint32_t dirIndex) -> void {
        for (const OverlayNode& child : dir.children) {
            const std::uint32_t index = static_cast<std::uint32_t>(entries.size());
            if (child.isDir) {
                entries.push_back({true, addName(child.name), dirIndex, 0});
                hosts.emplace_back();
                mods.emplace_back();
                self(self, child, index);
                entries[index].nextOrLength = static_cast<std::uint32_t>(entries.size());
            } else {
                entries.push_back({false, addName(child.name), child.position, child.length});
                hosts.push_back(child.host);
                mods.push_back(child.mod);
            }
        }
    };
    emit(emit, root, 0);
    entries[0].nameOffset = addName("");
    entries[0].nextOrLength = static_cast<std::uint32_t>(entries.size());
    if (strings.size() > kMaxStringOffset) {
        setError(error, "file names exceed the 16 MiB FST string table limit");
        return false;
    }
    Fst rebuilt;
    rebuilt.mEntries = std::move(entries);
    rebuilt.mHostPaths = std::move(hosts);
    rebuilt.mOverlayMods = std::move(mods);
    rebuilt.mStrings.assign(strings.begin(), strings.end());
    if (!rebuilt.finish(error)) {
        return false;
    }
    *this = std::move(rebuilt);
    return true;
}

const std::string& Fst::overlayMod(std::uint32_t index) const {
    static const std::string none;
    return index < mOverlayMods.size() ? mOverlayMods[index] : none;
}

bool Fst::finish(std::string* error) {
    mExtents.clear();
    mFileCount = 0;
    for (std::uint32_t i = 1; i < mEntries.size(); ++i) {
        const FstEntry& e = mEntries[i];
        if (e.isDir) {
            continue;
        }
        ++mFileCount;
        if (e.nextOrLength != 0) {
            mExtents.push_back({std::uint64_t(e.parentOrPosition) << 2, e.nextOrLength, i});
        }
    }
    std::sort(mExtents.begin(), mExtents.end(), [](const Extent& a, const Extent& b) { return a.start < b.start; });
    for (std::size_t i = 1; i < mExtents.size(); ++i) {
        const Extent& prev = mExtents[i - 1];
        if (prev.start + prev.length > mExtents[i].start) {
            setError(error, std::string("FST files '") + name(prev.entry) + "' and '" + name(mExtents[i].entry) + "' overlap on disc");
            return false;
        }
    }
    const std::uint64_t lastEnd = mExtents.empty() ? 0 : mExtents.back().start + mExtents.back().length;
    mDiscEnd = (lastEnd + 32 + kSyntheticAlignment - 1) / kSyntheticAlignment * kSyntheticAlignment;
    return true;
}

std::int32_t Fst::convertPathToEntrynum(const char* pathPtr, std::uint32_t currentDirectory) const {
    std::uint32_t dirLookAt = currentDirectory;

    while (true) {
        if (*pathPtr == '\0') {
            return static_cast<std::int32_t>(dirLookAt);
        } else if (*pathPtr == '/') {
            dirLookAt = 0;
            pathPtr++;
            continue;
        } else if (*pathPtr == '.') {
            if (pathPtr[1] == '.') {
                if (pathPtr[2] == '/') {
                    dirLookAt = mEntries[dirLookAt].parentOrPosition;
                    pathPtr += 3;
                    continue;
                } else if (pathPtr[2] == '\0') {
                    return static_cast<std::int32_t>(mEntries[dirLookAt].parentOrPosition);
                }
            } else if (pathPtr[1] == '/') {
                pathPtr += 2;
                continue;
            } else if (pathPtr[1] == '\0') {
                return static_cast<std::int32_t>(dirLookAt);
            }
        }

        const char* ptr = pathPtr;
        while (*ptr != '\0' && *ptr != '/') {
            ptr++;
        }
        const bool isDir = *ptr != '\0';
        const std::size_t length = static_cast<std::size_t>(ptr - pathPtr);

        std::uint32_t i = dirLookAt + 1;
        bool found = false;
        while (i < mEntries[dirLookAt].nextOrLength) {
            const FstEntry& e = mEntries[i];
            if ((e.isDir || !isDir) && isSame(pathPtr, name(i))) {
                found = true;
                break;
            }
            i = e.isDir ? e.nextOrLength : i + 1;
        }
        if (!found) {
            return -1;
        }
        if (!isDir) {
            return static_cast<std::int32_t>(i);
        }
        dirLookAt = i;
        pathPtr += length + 1;
    }
}

std::uint32_t Fst::entryToPath(std::uint32_t entry, char* path, std::uint32_t maxlen) const {
    if (entry == 0) {
        return 0;
    }
    std::uint32_t parent = mEntries[entry].parentOrPosition;
    if (!mEntries[entry].isDir) {
        // Files do not record their parent: find the innermost directory containing them.
        parent = 0;
        for (std::uint32_t i = 1; i < entry;) {
            if (mEntries[i].isDir && entry < mEntries[i].nextOrLength) {
                parent = i;
                i++;
            } else {
                i = mEntries[i].isDir ? mEntries[i].nextOrLength : i + 1;
            }
        }
    }
    std::uint32_t loc = entryToPath(parent, path, maxlen);
    if (loc == maxlen) {
        return loc;
    }
    path[loc++] = '/';
    for (const char* src = name(entry); loc < maxlen && *src != '\0';) {
        path[loc++] = *src++;
    }
    return loc;
}

bool Fst::convertEntrynumToPath(std::uint32_t entryNum, char* path, std::uint32_t maxlen) const {
    if (maxlen == 0) {
        return false;
    }
    std::uint32_t loc = entryToPath(entryNum, path, maxlen);
    if (loc == maxlen) {
        path[maxlen - 1] = '\0';
        return false;
    }
    if (mEntries[entryNum].isDir) {
        if (loc == maxlen - 1) {
            path[loc] = '\0';
            return false;
        }
        path[loc++] = '/';
    }
    path[loc] = '\0';
    return true;
}

}  // namespace PetariNative::Platform::DVD
