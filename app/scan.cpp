// Finding installed games (Steam, GOG, Epic and ones the user added) and looking inside
// their files: which .exe is the game, which graphics APIs it uses, whether it has anti-cheat.
#include "app.h"

#include <shlobj.h>

#include <algorithm>
#include <cstring>
#include <map>


namespace app {
namespace {

// ---- reading .exe and .dll files ----

// A read-only view of a file, or of as much of it as fits in the address space.
class Mapped {
public:
    explicit Mapped(const std::wstring& path, uint64_t limit = 768ull << 20) {
        file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, 0, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER size;
        if (!GetFileSizeEx(file_, &size) || size.QuadPart == 0) return;
        mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping_) return;
        for (uint64_t want = std::min<uint64_t>(uint64_t(size.QuadPart), limit); want >= (1u << 20) || !data;
             want /= 2) {
            data = static_cast<const uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, SIZE_T(want)));
            if (data) {
                this->size = size_t(want);
                break;
            }
            if (want < (1u << 20)) break;
        }
    }
    ~Mapped() {
        if (data) UnmapViewOfFile(data);
        if (mapping_) CloseHandle(mapping_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    }
    Mapped(const Mapped&) = delete;
    Mapped& operator=(const Mapped&) = delete;

    const uint8_t* data = nullptr;
    size_t size = 0;

private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
};

struct PeInfo {
    unsigned machine = 0;
    std::vector<std::string> imports;  // lower case, including delay-loaded ones
};

template <typename T>
const T* At(const uint8_t* d, size_t n, size_t offset) {
    return offset <= n && n - offset >= sizeof(T) ? reinterpret_cast<const T*>(d + offset) : nullptr;
}

bool ReadPe(const uint8_t* d, size_t n, PeInfo& out) {
    if (n < 0x40 || d[0] != 'M' || d[1] != 'Z') return false;
    uint32_t pe = *reinterpret_cast<const uint32_t*>(d + 0x3c);
    const uint32_t* signature = At<uint32_t>(d, n, pe);
    const IMAGE_FILE_HEADER* file = At<IMAGE_FILE_HEADER>(d, n, size_t(pe) + 4);
    if (!signature || *signature != 0x00004550 || !file) return false;
    out.machine = file->Machine;
    size_t optional = size_t(pe) + 24;
    const uint16_t* magic = At<uint16_t>(d, n, optional);
    if (!magic) return true;
    const IMAGE_DATA_DIRECTORY* dirs = nullptr;
    uint32_t count = 0;
    uint64_t imageBase = 0;
    if (*magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        auto* o = At<IMAGE_OPTIONAL_HEADER32>(d, n, optional);
        if (!o) return true;
        dirs = o->DataDirectory, count = o->NumberOfRvaAndSizes, imageBase = o->ImageBase;
    } else if (*magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        auto* o = At<IMAGE_OPTIONAL_HEADER64>(d, n, optional);
        if (!o) return true;
        dirs = o->DataDirectory, count = o->NumberOfRvaAndSizes, imageBase = o->ImageBase;
    } else {
        return true;
    }
    size_t sectionsAt = optional + file->SizeOfOptionalHeader;
    std::vector<IMAGE_SECTION_HEADER> sections;
    for (unsigned i = 0; i < file->NumberOfSections; i++) {
        auto* s = At<IMAGE_SECTION_HEADER>(d, n, sectionsAt + i * sizeof(IMAGE_SECTION_HEADER));
        if (!s) break;
        sections.push_back(*s);
    }
    auto offsetOf = [&](uint64_t rva) -> size_t {
        for (const IMAGE_SECTION_HEADER& s : sections) {
            uint32_t span = std::max(s.Misc.VirtualSize, s.SizeOfRawData);
            if (rva >= s.VirtualAddress && rva < uint64_t(s.VirtualAddress) + span)
                return size_t(rva - s.VirtualAddress + s.PointerToRawData);
        }
        return SIZE_MAX;
    };
    auto name = [&](uint64_t rva) {
        size_t at = offsetOf(rva);
        std::string s;
        for (size_t i = at; i < n && i - at < 260 && d[i]; i++) s += char(d[i] | (d[i] >= 'A' && d[i] <= 'Z' ? 0x20 : 0));
        return s;
    };
    if (count > IMAGE_DIRECTORY_ENTRY_IMPORT && dirs[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress) {
        size_t at = offsetOf(dirs[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
        for (int i = 0; i < 1024; i++) {
            auto* desc = At<IMAGE_IMPORT_DESCRIPTOR>(d, n, at + i * sizeof(IMAGE_IMPORT_DESCRIPTOR));
            if (!desc || !desc->Name) break;
            out.imports.push_back(name(desc->Name));
        }
    }
    if (count > IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT && dirs[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress) {
        struct DelayDescriptor {
            uint32_t attributes, name, module, iat, nameTable, boundIat, unloadIat, timestamp;
        };
        size_t at = offsetOf(dirs[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress);
        for (int i = 0; i < 1024; i++) {
            auto* desc = At<DelayDescriptor>(d, n, at + i * sizeof(DelayDescriptor));
            if (!desc || !desc->name) break;
            out.imports.push_back(name(desc->attributes & 1 ? desc->name : desc->name - imageBase));
        }
    }
    return true;
}

unsigned ApisFromImports(const std::vector<std::string>& imports) {
    unsigned apis = 0;
    for (const std::string& i : imports) {
        if (i == "d3d9.dll") apis |= ApiD3D9;
        else if (i == "d3d10.dll" || i == "d3d10_1.dll") apis |= ApiD3D10;
        else if (i == "d3d11.dll") apis |= ApiD3D11;
        else if (i == "d3d12.dll") apis |= ApiD3D12;
        else if (i == "vulkan-1.dll") apis |= ApiVulkan;
        else if (i == "opengl32.dll") apis |= ApiOpenGL;
    }
    return apis;
}

// Games that load their graphics DLL at run time still name it somewhere in their code
// ("d3d11.dll", D3D11CreateDevice, "vulkan-1.dll"), in ASCII or UTF-16.
unsigned ApisFromStrings(const uint8_t* d, size_t n) {
    unsigned apis = 0;
    auto lower = [](uint8_t c) { return uint8_t(c >= 'A' && c <= 'Z' ? c | 0x20 : c); };
    auto version = [](uint8_t c) {
        return c == '0' ? ApiD3D10 : c == '1' ? ApiD3D11 : c == '2' ? ApiD3D12 : 0u;
    };
    for (size_t i = 0; i + 16 < n; i++) {
        uint8_t c = lower(d[i]);
        if (c == 'd') {
            if (d[i + 1] == '3' && lower(d[i + 2]) == 'd') {
                if (d[i + 3] == '1') apis |= version(d[i + 4]);
                else if (d[i + 3] == '9' && (d[i + 4] == '.' || d[i + 4] == 0)) apis |= ApiD3D9;
            } else if (d[i + 1] == 0 && d[i + 2] == '3' && d[i + 3] == 0 && lower(d[i + 4]) == 'd' && d[i + 5] == 0 &&
                       d[i + 6] == '1' && d[i + 7] == 0 && d[i + 9] == 0) {
                apis |= version(d[i + 8]);
            }
        } else if (c == 'v' && lower(d[i + 1]) == 'u' && lower(d[i + 2]) == 'l' && lower(d[i + 3]) == 'k' &&
                   lower(d[i + 4]) == 'a' && lower(d[i + 5]) == 'n' && d[i + 6] == '-' && d[i + 7] == '1') {
            apis |= ApiVulkan;
        } else if (c == 'v' && d[i + 1] == 'k' && memcmp(d + i + 2, "CreateInstance", 14) == 0) {
            apis |= ApiVulkan;
        }
    }
    return apis;
}

// ---- one game folder ----

std::wstring LetterKey(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : Lower(s))
        if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9')) out += c;
    return out;
}

bool Contains(const std::wstring& s, const wchar_t* part) { return s.find(part) != std::wstring::npos; }

// Not the game: installers, crash reporters, helpers.
bool HelperExe(const std::wstring& stem) {
    static const wchar_t* const kParts[] = {
        L"unins", L"setup", L"redist", L"dxsetup", L"dotnet", L"crash", L"reporter", L"bugreport", L"uploader",
        L"updater", L"patcher", L"cefprocess", L"subprocess", L"webhelper", L"easyanticheat", L"protected_game",
        L"beservice", L"battleye", L"cleanup", L"installer", L"activation", L"physx", L"oalinst", L"directx",
        L"notification", L"overlay", L"dxwebsetup", L"pnkbstr", L"pbsvc", L"renderdoc", L"vcredist", L"vc_redist",
        L"touchup", L"prereq", L"unrealcefsubprocess", L"epicwebhelper", L"quicksfv", L"register"};
    for (const wchar_t* p : kParts)
        if (Contains(stem, p)) return true;
    return false;
}

bool SkippedDir(const std::wstring& name, int depth) {
    static const wchar_t* const kNames[] = {
        L"_commonredist", L"commonredist", L"redist", L"_redist", L"redistributables", L"directx", L"dotnet",
        L"dotnetfx", L"vcredist", L"vc_redist", L"support", L"installer", L"installers", L"__installer",
        L"easyanticheat", L"battleye", L"prerequisites", L"prereqs", L"physx", L"crashreporter", L"crashpad",
        L"documentation", L"manual", L"tools", L"sdk", L"editor", L"__macosx", L"$recycle.bin"};
    for (const wchar_t* n : kNames)
        if (name == n) return true;
    return depth == 0 && name == L"engine";  // Unreal: the game's .exe is in <Project>\Binaries
}

struct AntiCheatMarker {
    const wchar_t* part;
    const wchar_t* name;
};
const AntiCheatMarker kAntiCheat[] = {
    {L"easyanticheat", L"Easy Anti-Cheat"}, {L"start_protected_game", L"Easy Anti-Cheat"},
    {L"eac_launcher", L"Easy Anti-Cheat"},  {L"battleye", L"BattlEye"},
    {L"beservice", L"BattlEye"},            {L"beclient", L"BattlEye"},
    {L"xigncode", L"XIGNCODE3"},            {L"xhunter1", L"XIGNCODE3"},
    {L"gameguard", L"nProtect GameGuard"},  {L"gamemon.des", L"nProtect GameGuard"},
    {L"nprotect", L"nProtect"},             {L"pnkbstr", L"PunkBuster"},
    {L"pbsvc", L"PunkBuster"},              {L"equ8", L"EQU8"},
    {L"mhyprot", L"mhyprot"},               {L"anticheatexpert", L"Anti-Cheat Expert"},
    {L"ace-base", L"Anti-Cheat Expert"},    {L"tenprotect", L"TenProtect"},
    {L"hackshield", L"HackShield"},         {L"ehsvc.dll", L"HackShield"},
    {L"faceit", L"FACEIT"},
};

void NoteAntiCheat(const std::wstring& lowerName, std::vector<std::wstring>& found) {
    for (const AntiCheatMarker& m : kAntiCheat)
        if (Contains(lowerName, m.part) && std::find(found.begin(), found.end(), m.name) == found.end())
            found.push_back(m.name);
}

struct Candidate {
    std::wstring path;
    int depth = 0;
    uint64_t size = 0;
    int score = 0;
    unsigned apis = 0;
};

struct Walk {
    std::vector<Candidate> exes;
    std::vector<std::wstring> antiCheat;
    int visited = 0;
};

void WalkDir(const std::wstring& dir, int depth, Walk& walk) {
    if (depth > 4 || walk.visited > 40000) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(Join(dir, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> subdirs;
    do {
        walk.visited++;
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::wstring lower = Lower(name);
        NoteAntiCheat(lower, walk.antiCheat);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && !SkippedDir(lower, depth)) subdirs.push_back(name);
        } else if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, L".exe") == 0) {
            Candidate c;
            c.path = Join(dir, name);
            c.depth = depth;
            c.size = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            walk.exes.push_back(c);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (const std::wstring& s : subdirs) WalkDir(Join(dir, s), depth + 1, walk);
}

// DLLs next to an .exe that do the rendering for it (UnityPlayer.dll, engine DLLs).
// Wrappers and overlays are left out: they import graphics DLLs whatever the game uses.
class DirApis {
public:
    unsigned Imports(const std::wstring& dir) { return Get(dir).imports; }
    unsigned Strings(const std::wstring& dir) {
        Entry& e = Get(dir);
        if (!e.stringsDone) {
            e.stringsDone = true;
            uint64_t budget = 384ull << 20;
            for (const std::wstring& dll : e.dlls) {
                uint64_t size = FileSize(dll);
                if (size > budget) continue;
                budget -= size;
                Mapped m(dll);
                if (m.data) e.strings |= ApisFromStrings(m.data, m.size);
            }
        }
        return e.strings;
    }

private:
    struct Entry {
        std::vector<std::wstring> dlls;
        unsigned imports = 0, strings = 0;
        bool stringsDone = false;
    };
    Entry& Get(const std::wstring& dir) {
        std::wstring key = Lower(dir);
        auto it = cache_.find(key);
        if (it != cache_.end()) return it->second;
        Entry& e = cache_[key];
        static const wchar_t* const kSkip[] = {L"d3d11.dll", L"dxgi.dll", L"d3d9.dll", L"d3d10.dll", L"d3d10_1.dll",
                                               L"d3d12.dll", L"opengl32.dll", L"dinput8.dll", L"version.dll",
                                               L"winmm.dll", L"vulkan-1.dll"};
        static const wchar_t* const kSkipPrefix[] = {L"nvngx", L"libcef", L"eosovh", L"gameoverlay", L"amd_", L"ffx_",
                                                     L"sl.", L"libxess", L"d3dcompiler", L"nvapi", L"dxcompiler",
                                                     L"dxil", L"libegl", L"libglesv2", L"vk_swiftshader", L"reshade",
                                                     L"discord", L"galaxy", L"steam"};
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(Join(dir, L"*.dll").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) return e;
        do {
            std::wstring lower = Lower(fd.cFileName);
            bool skip = false;
            for (const wchar_t* s : kSkip) skip |= lower == s;
            for (const wchar_t* p : kSkipPrefix) skip |= lower.rfind(p, 0) == 0;
            if (!skip && e.dlls.size() < 64) e.dlls.push_back(Join(dir, fd.cFileName));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        for (const std::wstring& dll : e.dlls) {
            Mapped m(dll, 64ull << 20);
            PeInfo pe;
            if (m.data && ReadPe(m.data, m.size, pe)) e.imports |= ApisFromImports(pe.imports);
        }
        return e;
    }
    std::map<std::wstring, Entry> cache_;
};

unsigned ExeImports(const std::wstring& exe, unsigned* machine = nullptr) {
    Mapped m(exe, 64ull << 20);
    PeInfo pe;
    if (!m.data || !ReadPe(m.data, m.size, pe)) return 0;
    if (machine) *machine = pe.machine;
    return ApisFromImports(pe.imports);
}

constexpr unsigned kGraphics = ApiD3D9 | ApiD3D10 | ApiD3D11 | ApiD3D12 | ApiVulkan | ApiOpenGL;

std::wstring ChooseExe(Walk& walk, const std::wstring& storeExe, const std::wstring& gameName, DirApis& dirs) {
    std::wstring nameKey = LetterKey(gameName);
    std::wstring storeKey = Lower(storeExe);
    std::vector<Candidate> list;
    for (Candidate c : walk.exes) {
        std::wstring stem = Lower(Stem(c.path));
        bool fromStore = !storeKey.empty() && Lower(c.path) == storeKey;
        if (HelperExe(stem) && !fromStore) continue;
        c.score = fromStore ? 400 : 0;
        c.score -= c.depth * 20;
        c.score += int(std::min<uint64_t>(c.size >> 20, 150));
        std::wstring stemKey = LetterKey(stem);
        if (stemKey.size() >= 3 && !nameKey.empty() &&
            (nameKey.find(stemKey) != std::wstring::npos || stemKey.find(nameKey) != std::wstring::npos ||
             nameKey.compare(0, 4, stemKey, 0, 4) == 0))
            c.score += 150;
        if (Contains(stem, L"shipping")) c.score += 120;
        if (Contains(stem, L"launcher")) c.score -= 300;
        if (Contains(stem, L"config") || Contains(stem, L"settings")) c.score -= 200;
        if (Contains(stem, L"server") || Contains(stem, L"editor")) c.score -= 250;
        if (Contains(stem, L"benchmark") || Contains(stem, L"tool")) c.score -= 120;
        list.push_back(c);
    }
    if (list.empty()) return storeExe;
    std::sort(list.begin(), list.end(), [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    if (list.size() > 12) list.resize(12);
    bool any = false;
    for (Candidate& c : list) {
        c.apis = ExeImports(c.path) | dirs.Imports(DirOf(c.path));
        if (c.apis & kGraphics) any = true;
    }
    if (!any)
        for (size_t i = 0; i < list.size() && i < 3; i++) {
            Mapped m(list[i].path);
            if (m.data) list[i].apis = ApisFromStrings(m.data, m.size) | dirs.Strings(DirOf(list[i].path));
        }
    for (Candidate& c : list) {
        if (c.apis & ApiD3D11) c.score += 800;
        else if (c.apis & kGraphics) c.score += 600;
    }
    return std::max_element(list.begin(), list.end(),
                            [](const Candidate& a, const Candidate& b) { return a.score < b.score; })
        ->path;
}

// ---- stores ----

std::wstring RegString(HKEY root, const wchar_t* path, const wchar_t* name, REGSAM view = 0) {
    HKEY key;
    if (RegOpenKeyExW(root, path, 0, KEY_READ | view, &key) != ERROR_SUCCESS) return {};
    wchar_t buf[2048];
    DWORD size = sizeof(buf) - sizeof(wchar_t), type = 0;
    std::wstring out;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        buf[size / sizeof(wchar_t)] = 0;
        out = buf;
    }
    RegCloseKey(key);
    return out;
}

std::string ReadText(const std::wstring& path) {
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(path, bytes, 8u << 20)) return {};
    return std::string(bytes.begin(), bytes.end());
}

// The quoted string that follows position `at` (Valve's VDF and JSON use the same escapes).
std::string QuotedAfter(const std::string& text, size_t at, size_t* end = nullptr) {
    size_t open = text.find('"', at);
    if (open == std::string::npos) return {};
    std::string out;
    size_t i = open + 1;
    for (; i < text.size() && text[i] != '"'; i++) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            char e = text[++i];
            out += e == 'n' ? '\n' : e == 't' ? '\t' : e;
        } else {
            out += text[i];
        }
    }
    if (end) *end = i + 1;
    return out;
}

// Value of "key" in a VDF/ACF file or a JSON object (first match, any nesting).
std::string KeyValue(const std::string& text, const char* key, size_t from = 0, size_t* next = nullptr) {
    std::string quoted = std::string("\"") + key + "\"";
    for (size_t at = from; (at = text.find(quoted, at)) != std::string::npos; at += quoted.size()) {
        size_t i = at + quoted.size();
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == ':')) i++;
        if (i < text.size() && text[i] == '"') return QuotedAfter(text, i, next);
    }
    if (next) *next = std::string::npos;
    return {};
}

std::wstring Backslashes(std::wstring s) {
    std::replace(s.begin(), s.end(), L'/', L'\\');
    return s;
}

void SteamGames(std::vector<Game>& games) {
    // InstallPath keeps the folder names' case; SteamPath (per user) is all lower case.
    std::wstring steam = RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", KEY_WOW64_32KEY);
    if (steam.empty() || !DirExists(steam))
        steam = Backslashes(RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"));
    if (steam.empty() || !DirExists(steam)) return;
    std::vector<std::wstring> libraries = {steam};
    std::string folders = ReadText(Join(steam, L"steamapps\\libraryfolders.vdf"));
    for (size_t at = 0; at < folders.size();) {
        size_t next = 0;
        std::string path = KeyValue(folders, "path", at, &next);
        if (next == std::string::npos) break;
        at = next;
        std::wstring lib = Backslashes(Widen(path));
        bool known = false;
        for (const std::wstring& l : libraries) known |= Lower(l) == Lower(lib);
        if (!known && DirExists(lib)) libraries.push_back(lib);
    }
    for (const std::wstring& lib : libraries) {
        std::wstring apps = Join(lib, L"steamapps");
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(Join(apps, L"appmanifest_*.acf").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::string acf = ReadText(Join(apps, fd.cFileName));
            std::wstring appid = Widen(KeyValue(acf, "appid"));
            std::wstring name = Widen(KeyValue(acf, "name"));
            std::wstring dir = Widen(KeyValue(acf, "installdir"));
            unsigned flags = unsigned(atoi(KeyValue(acf, "StateFlags").c_str()));
            if (appid.empty() || dir.empty() || !(flags & 4)) continue;  // 4: fully installed
            if (appid == L"228980" || name.rfind(L"Proton", 0) == 0 || name.rfind(L"Steam Linux Runtime", 0) == 0 ||
                name.rfind(L"Steamworks", 0) == 0 || name.rfind(L"SteamVR", 0) == 0)
                continue;
            Game g;
            g.store = Store::Steam;
            g.storeId = appid;
            g.id = L"steam:" + appid;
            g.name = name.empty() ? dir : name;
            g.root = Join(Join(apps, L"common"), dir);
            if (!DirExists(g.root)) continue;
            std::wstring cache = Join(steam, L"appcache\\librarycache");
            for (std::wstring art : {Join(Join(cache, appid), L"header.jpg"), Join(cache, appid + L"_header.jpg")})
                if (g.art.empty() && FileExists(art)) g.art = art;
            if (g.art.empty()) {  // newer Steam versions keep some images one folder deeper
                WIN32_FIND_DATAW sub;
                HANDLE s = FindFirstFileW(Join(Join(cache, appid), L"*").c_str(), &sub);
                if (s != INVALID_HANDLE_VALUE) {
                    do {
                        std::wstring art = Join(Join(Join(cache, appid), sub.cFileName), L"header.jpg");
                        if ((sub.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && sub.cFileName[0] != L'.' &&
                            FileExists(art))
                            g.art = art;
                    } while (g.art.empty() && FindNextFileW(s, &sub));
                    FindClose(s);
                }
            }
            games.push_back(g);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

void GogGames(std::vector<Game>& games) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\GOG.com\\Games", 0, KEY_READ | KEY_WOW64_32KEY, &key) != ERROR_SUCCESS)
        return;
    wchar_t sub[256];
    for (DWORD i = 0; RegEnumKeyW(key, i, sub, 256) == ERROR_SUCCESS; i++) {
        std::wstring path = std::wstring(L"SOFTWARE\\GOG.com\\Games\\") + sub;
        Game g;
        g.store = Store::GOG;
        g.storeId = sub;
        g.id = std::wstring(L"gog:") + sub;
        g.name = RegString(HKEY_LOCAL_MACHINE, path.c_str(), L"gameName", KEY_WOW64_32KEY);
        g.root = RegString(HKEY_LOCAL_MACHINE, path.c_str(), L"path", KEY_WOW64_32KEY);
        g.storeExe = RegString(HKEY_LOCAL_MACHINE, path.c_str(), L"exe", KEY_WOW64_32KEY);
        if (g.storeExe.empty()) {
            std::wstring file = RegString(HKEY_LOCAL_MACHINE, path.c_str(), L"exeFile", KEY_WOW64_32KEY);
            if (!file.empty()) g.storeExe = Join(g.root, file);
        }
        if (!g.root.empty() && DirExists(g.root)) games.push_back(g);
    }
    RegCloseKey(key);
}

void EpicGames(std::vector<Game>& games) {
    wchar_t data[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, data))) return;
    std::wstring dir = Join(data, L"Epic\\EpicGamesLauncher\\Data\\Manifests");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Join(dir, L"*.item").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string json = ReadText(Join(dir, fd.cFileName));
        if (json.find("\"bIsIncompleteInstall\": true") != std::string::npos) continue;
        Game g;
        g.store = Store::Epic;
        g.storeId = Widen(KeyValue(json, "AppName"));
        g.id = L"epic:" + g.storeId;
        g.name = Widen(KeyValue(json, "DisplayName"));
        g.root = Backslashes(Widen(KeyValue(json, "InstallLocation")));
        std::wstring launch = Backslashes(Widen(KeyValue(json, "LaunchExecutable")));
        if (!launch.empty()) g.storeExe = Join(g.root, launch);
        if (!g.storeId.empty() && !g.root.empty() && DirExists(g.root)) games.push_back(g);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void AnalyzeWith(Game& game, DirApis& dirs) {
    Walk walk;
    WalkDir(game.root, 0, walk);
    if (!game.exeFromUser) game.exe = ChooseExe(walk, game.storeExe, game.name, dirs);
    game.antiCheat = walk.antiCheat;
    if (game.exe.empty()) return;
    // Anti-cheat folders can sit above the .exe (Unreal: <root>\EasyAntiCheat, <root>\Game\Binaries\Win64).
    std::wstring dir = DirOf(game.exe);
    for (int up = 0; up < 4 && !dir.empty(); up++, dir = DirOf(dir)) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(Join(dir, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) continue;
        int n = 0;
        do NoteAntiCheat(Lower(fd.cFileName), game.antiCheat);
        while (++n < 2000 && FindNextFileW(h, &fd));
        FindClose(h);
    }
    unsigned machine = 0;
    std::wstring exeDir = DirOf(game.exe);
    game.apis = ExeImports(game.exe, &machine) | dirs.Imports(exeDir);
    game.machine = machine;
    if (!(game.apis & ApiD3D11)) {
        Mapped m(game.exe);
        if (m.data) game.apis |= ApisFromStrings(m.data, m.size);
        game.apis |= dirs.Strings(exeDir);
    }
}

}  // namespace

std::wstring StoreName(Store store) {
    switch (store) {
        case Store::Steam: return L"Steam";
        case Store::GOG: return L"GOG";
        case Store::Epic: return L"Epic Games";
        default: return L"Added by you";
    }
}

std::wstring MachineName(unsigned machine) {
    switch (machine) {
        case IMAGE_FILE_MACHINE_AMD64: return L"64-bit";
        case IMAGE_FILE_MACHINE_I386: return L"32-bit";
        case 0xAA64: return L"ARM64";
        case 0: return L"";
        default: return L"other CPU";
    }
}

std::wstring ApiNames(unsigned apis) {
    std::vector<std::wstring> dx;
    if (apis & ApiD3D9) dx.push_back(L"9");
    if (apis & ApiD3D10) dx.push_back(L"10");
    if (apis & ApiD3D11) dx.push_back(L"11");
    if (apis & ApiD3D12) dx.push_back(L"12");
    std::wstring out;
    if (!dx.empty()) {
        out = L"DirectX " + dx[0];
        for (size_t i = 1; i < dx.size(); i++) out += (i + 1 == dx.size() ? L" + " : L", ") + dx[i];
    }
    if (apis & ApiVulkan) out += out.empty() ? L"Vulkan" : L" + Vulkan";
    if (apis & ApiOpenGL) out += out.empty() ? L"OpenGL" : L" + OpenGL";
    return out;
}

std::wstring CustomId(const std::wstring& exe) {
    uint64_t hash = 1469598103934665603ull;
    for (wchar_t c : Lower(exe)) hash = (hash ^ uint64_t(c)) * 1099511628211ull;
    return Format(L"exe:%016llx", static_cast<unsigned long long>(hash));
}

Game CustomGame(const std::wstring& exe) {
    Game g;
    g.store = Store::Custom;
    g.id = CustomId(exe);
    g.exe = exe;
    g.root = DirOf(exe);
    g.exeFromUser = true;
    for (const wchar_t* field : {L"ProductName", L"FileDescription"}) {
        std::wstring v = VersionString(exe, field);
        if (g.name.empty() && !v.empty() && !Contains(Lower(v), L"bootstrap") && v.size() < 80) g.name = v;
    }
    if (g.name.empty()) g.name = Stem(exe);
    return g;
}

void Analyze(Game& game) {
    DirApis dirs;
    AnalyzeWith(game, dirs);
}

std::vector<Game> FindGames() {
    std::vector<Game> games;
    SteamGames(games);
    GogGames(games);
    EpicGames(games);
    for (Game& g : games) {
        std::wstring chosen = GetSetting(L"game:" + g.id, L"exe");
        if (!chosen.empty() && FileExists(chosen)) {
            g.exe = chosen;
            g.exeFromUser = true;
        }
    }
    for (const auto& [id, exe] : SettingsSection(L"custom")) {
        Game g = CustomGame(exe);
        std::wstring name = GetSetting(L"game:" + g.id, L"name");
        if (!name.empty()) g.name = name;
        games.push_back(g);
    }
    DirApis dirs;
    for (Game& g : games) {
        AnalyzeWith(g, dirs);
        LoadGameSettings(g);
    }
    // A game added by hand that a store also lists: keep the store's entry.
    std::set<std::wstring> storeExes;
    for (const Game& g : games)
        if (g.store != Store::Custom) storeExes.insert(Lower(g.exe));
    games.erase(std::remove_if(games.begin(), games.end(),
                               [&](const Game& g) { return g.store == Store::Custom && storeExes.count(Lower(g.exe)); }),
                games.end());
    return games;
}

}  // namespace app
