// Turning the layer on and off for a game, reading what it logged, and the warnings shown.
//
// Turning on puts d3d11.dll (from this program's resources) and dx11on10.ini next to the
// game's .exe; another program's d3d11.dll is renamed to d3d11.dll.dx11on10-backup first.
// Turning off removes our files and puts the backup back. The steps run as a list that is
// undone if one fails, and run again in an elevated copy of this program when the folder
// needs administrator rights.
#include "app.h"

#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "resource.h"

namespace app {
namespace {

const wchar_t kDll[] = L"d3d11.dll";
const wchar_t kIni[] = L"dx11on10.ini";
const wchar_t kLog[] = L"dx11on10.log";
const wchar_t kBackup[] = L"d3d11.dll.dx11on10-backup";

struct Bytes {
    const uint8_t* data = nullptr;
    size_t size = 0;
};

Bytes Resource(int id) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!r) return {};
    HGLOBAL g = LoadResource(nullptr, r);
    return {static_cast<const uint8_t*>(LockResource(g)), SizeofResource(nullptr, r)};
}

Bytes Proxy(unsigned machine) { return Resource(machine == IMAGE_FILE_MACHINE_AMD64 ? IDR_PROXY_X64 : IDR_PROXY_X86); }

bool FindNoCase(const std::vector<uint8_t>& bytes, const char* text) {
    size_t n = strlen(text);
    auto it = std::search(bytes.begin(), bytes.end(), text, text + n, [](uint8_t a, char b) {
        return (a >= 'A' && a <= 'Z' ? a | 0x20 : a) == uint8_t(b);
    });
    return it != bytes.end();
}

// Builds from before the version resource are recognised by the name of their log file.
bool Ours(const std::wstring& dll) {
    if (VersionString(dll, L"ProductName") == L"DX11on10") return true;
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(dll, bytes, 16u << 20)) return false;
    static const uint8_t marker[] = {'\\', 0, 'd', 0, 'x', 0, '1', 0, '1', 0, 'o', 0, 'n', 0, '1', 0, '0', 0, '.', 0, 'l', 0, 'o', 0, 'g', 0};
    return std::search(bytes.begin(), bytes.end(), std::begin(marker), std::end(marker)) != bytes.end();
}

// Who another d3d11.dll or dxgi.dll belongs to, as far as we can tell.
std::wstring ForeignName(const std::wstring& dll) {
    std::vector<uint8_t> bytes;
    ReadFileBytes(dll, bytes, 64u << 20);
    struct Known {
        const char* marker;
        const wchar_t* name;
    };
    static const Known kKnown[] = {{"reshade", L"ReShade"},     {"enbseries", L"ENBSeries"},
                                   {"dxvk", L"DXVK"},           {"special k", L"Special K"},
                                   {"specialk", L"Special K"},  {"dgvoodoo", L"dgVoodoo"},
                                   {"3dmigoto", L"3DMigoto"},   {"lossless scaling", L"Lossless Scaling"},
                                   {"optiscaler", L"OptiScaler"}};
    for (const Known& k : kKnown)
        if (FindNoCase(bytes, k.marker)) return k.name;
    std::wstring product = VersionString(dll, L"ProductName");
    if (product.find(L"Microsoft") != std::wstring::npos) return L"a copy of Windows' own file";
    return product.empty() ? L"an unknown program" : product;
}

bool CanWrite(const std::wstring& dir) {
    std::wstring probe = Join(dir, L"dx11on10-write-test.tmp");
    HANDLE f = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (f == INVALID_HANDLE_VALUE) return GetLastError() != ERROR_ACCESS_DENIED;
    CloseHandle(f);
    return true;
}

RunReport ReadReport(const std::wstring& path) {
    RunReport r;
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(path, bytes, 32u << 20)) return r;
    r.present = true;
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) r.written = data.ftLastWriteTime;
    std::string text(bytes.begin(), bytes.end());
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        size_t at;
        char real[16] = {}, seen[16] = {};
        if ((at = line.find("created at feature level ")) != std::string::npos &&
            sscanf(line.c_str() + at, "created at feature level %15[0-9_], game sees %15[0-9_]", real, seen) == 2) {
            r.realLevel = real;
            r.gameLevel = seen;
        } else if (line.find("first frame presented") != std::string::npos) {
            r.firstFrame = true;
        } else if ((at = line.find("] frame ")) != std::string::npos) {
            unsigned frame, draws, unchanged;
            sscanf(line.c_str() + at,
                   "] frame %u: %lf fps; draws %u; shaders converted %u, unchanged %u, placeholders %u; draws skipped %u, "
                   "dispatches skipped %u, emulated %u",
                   &frame, &r.fps, &draws, &r.converted, &unchanged, &r.placeholders, &r.drawsSkipped,
                   &r.dispatchesSkipped, &r.emulated);
        } else if ((at = line.find("unloading: shaders converted ")) != std::string::npos) {
            unsigned unchanged;
            sscanf(line.c_str() + at,
                   "unloading: shaders converted %u, unchanged %u, placeholders %u; draws skipped %u, dispatches skipped %u",
                   &r.converted, &unchanged, &r.placeholders, &r.drawsSkipped, &r.dispatchesSkipped);
        } else if (line.find("can't be replayed") != std::string::npos) {
            r.computeUnsupported++;
        } else if (line.find("EXCEPTION 0x") != std::string::npos) {
            r.exceptions++;
        } else if (line.find("no requested level is available") != std::string::npos) {
            r.refused = true;
        } else if (line.find("real D3D11CreateDevice failed") != std::string::npos) {
            r.createFailed = true;
        }
    }
    return r;
}

// ---- file operations ----

struct Op {
    wchar_t kind;  // 'x' extract d3d11.dll (a: "64" or "32") to b, 'c' copy a to b, 'm' move a to b, 'd' delete a
    std::wstring a, b;
};

DWORD Extract(const std::wstring& bits, const std::wstring& to) {
    Bytes dll = Proxy(bits == L"64" ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386);
    if (!dll.data) return ERROR_RESOURCE_DATA_NOT_FOUND;
    std::wstring temp = to + L".dx11on10-new";
    HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD written = 0;
    BOOL ok = WriteFile(f, dll.data, DWORD(dll.size), &written, nullptr) && written == dll.size;
    DWORD error = ok ? 0 : GetLastError();
    CloseHandle(f);
    if (!error && !MoveFileExW(temp.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        error = GetLastError();
    if (error) DeleteFileW(temp.c_str());
    return error;
}

DWORD Apply(const Op& op, std::vector<Op>& undo) {
    switch (op.kind) {
        case 'x':
        case 'c': {
            bool existed = FileExists(op.b);
            DWORD e = op.kind == 'x' ? Extract(op.a, op.b) : (CopyFileW(op.a.c_str(), op.b.c_str(), FALSE) ? 0 : GetLastError());
            if (!e && !existed) undo.push_back({'d', op.b, L""});
            return e;
        }
        case 'm':
            if (!MoveFileExW(op.a.c_str(), op.b.c_str(), MOVEFILE_WRITE_THROUGH)) return GetLastError();
            undo.push_back({'m', op.b, op.a});
            return 0;
        case 'd':
            if (!DeleteFileW(op.a.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) return GetLastError();
            return 0;
    }
    return ERROR_INVALID_PARAMETER;
}

DWORD RunOps(const std::vector<Op>& ops) {
    std::vector<Op> undo;
    for (const Op& op : ops) {
        DWORD e = Apply(op, undo);
        if (e) {
            std::vector<Op> ignored;
            for (auto it = undo.rbegin(); it != undo.rend(); ++it) Apply(*it, ignored);
            return e;
        }
    }
    return 0;
}

std::wstring TempPath(const wchar_t* extension) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    return Join(dir, Format(L"dx11on10-%lu-%lu%ls", GetCurrentProcessId(), GetTickCount(), extension));
}

bool WriteBytes(const std::wstring& path, const void* data, size_t size) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(f, data, DWORD(size), &written, nullptr) && written == size;
    CloseHandle(f);
    return ok;
}

Result Failed(DWORD error, const std::wstring& what) {
    Result r;
    r.ok = false;
    r.message = what + L"\n\nWindows said: " + ErrorText(error);
    if (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION)
        r.message += L"\n\nIf the game, its launcher or an antivirus scan is using these files, close it and try again.";
    return r;
}

// Runs the steps here, or in an elevated copy of this program if the folder needs it.
Result Execute(const std::vector<Op>& ops, bool admin, const std::wstring& what) {
    if (!admin) {
        DWORD e = RunOps(ops);
        if (!e) return {};
        if (e != ERROR_ACCESS_DENIED) return Failed(e, what);
    }
    std::wstring list = L"\xFEFF";
    for (const Op& op : ops) list += std::wstring(1, op.kind) + L"\t" + op.a + L"\t" + op.b + L"\n";
    std::wstring file = TempPath(L".ops");
    if (!WriteBytes(file, list.data(), list.size() * sizeof(wchar_t))) return Failed(GetLastError(), what);
    std::wstring self = SelfPath(), args = L"--ops \"" + file + L"\"";
    SHELLEXECUTEINFOW info = {sizeof(info)};
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.hwnd = GetActiveWindow();
    info.lpVerb = L"runas";
    info.lpFile = self.c_str();
    info.lpParameters = args.c_str();
    info.nShow = SW_HIDE;
    Result r;
    if (!ShellExecuteExW(&info)) {
        DWORD e = GetLastError();
        DeleteFileW(file.c_str());
        if (e == ERROR_CANCELLED) {
            r.ok = false;
            r.cancelled = true;
            r.message = L"Nothing was changed: the administrator permission was not given.";
            return r;
        }
        return Failed(e, what);
    }
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    DeleteFileW(file.c_str());
    return code ? Failed(code, what) : r;
}

std::string IniText(const GameSettings& s) {
    std::string text = "; Written by the DX11on10 app " DX11ON10_VERSION ". Change these settings in the app.\r\n"
                       "[dx11on10]\r\n";
    text += std::string("MaxFeatureLevel=") + (s.level100 ? "10_0" : "10_1") + "\r\n";
    text += std::string("FakeFeatureLevel=") + (s.fakeAlways ? "always" : "auto") + "\r\n";
    text += "Translate=1\r\n";
    text += std::string("Driver=") + (s.warp ? "warp" : "hardware") + "\r\n";
    return text;
}

}  // namespace

// ---- settings ----

void LoadGameSettings(Game& game) {
    std::wstring s = L"game:" + game.id;
    game.settings.fakeAlways = GetSetting(s, L"fake", L"always") != L"auto";
    game.settings.level100 = GetSetting(s, L"level", L"auto") == L"10_0";
    game.settings.warp = GetSetting(s, L"driver", L"hardware") == L"warp";
}

void SaveGameSettings(const Game& game) {
    std::wstring s = L"game:" + game.id;
    std::wstring fake = game.settings.fakeAlways ? L"always" : L"auto";
    std::wstring level = game.settings.level100 ? L"10_0" : L"auto";
    std::wstring driver = game.settings.warp ? L"warp" : L"hardware";
    SetSetting(s, L"fake", &fake);
    SetSetting(s, L"level", &level);
    SetSetting(s, L"driver", &driver);
}

// ---- state ----

std::set<std::wstring> RunningFolders() {
    std::set<std::wstring> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe = {sizeof(pe)};
    for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!p) continue;
        wchar_t path[MAX_PATH * 2];
        DWORD size = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(p, 0, path, &size)) out.insert(Lower(DirOf(path)));
        CloseHandle(p);
    }
    CloseHandle(snap);
    return out;
}

bool ProxyIdentical(const std::wstring& path, unsigned machine) {
    Bytes dll = Proxy(machine);
    std::vector<uint8_t> bytes;
    return dll.data && FileSize(path) == dll.size && ReadFileBytes(path, bytes) &&
           memcmp(bytes.data(), dll.data, dll.size) == 0;
}

void RefreshState(Game& game, const std::set<std::wstring>& running) {
    game.install = Install::Off;
    game.installedVersion.clear();
    game.foreignName.clear();
    game.dxgiMod.clear();
    game.backup = game.running = game.needsAdmin = false;
    game.report = RunReport{};
    if (game.exe.empty()) return;
    std::wstring dir = DirOf(game.exe), dll = Join(dir, kDll);
    if (FileExists(dll)) {
        if (Ours(dll)) {
            game.installedVersion = VersionString(dll, L"ProductVersion");
            game.install = ProxyIdentical(dll, game.machine) ? Install::On : Install::Outdated;
        } else {
            game.install = Install::Foreign;
            game.foreignName = ForeignName(dll);
        }
    }
    game.backup = FileExists(Join(dir, kBackup));
    if (FileExists(Join(dir, L"dxgi.dll"))) game.dxgiMod = ForeignName(Join(dir, L"dxgi.dll"));
    game.running = running.count(Lower(dir)) > 0;
    game.needsAdmin = DirExists(dir) && !CanWrite(dir);
    if (game.install == Install::On || game.install == Install::Outdated) game.report = ReadReport(Join(dir, kLog));
}

// ---- turning on and off ----

Result Enable(Game& game) {
    Result r;
    if (game.machine != IMAGE_FILE_MACHINE_AMD64 && game.machine != IMAGE_FILE_MACHINE_I386) {
        r.ok = false;
        r.message = L"DX11on10 only works with 32-bit and 64-bit Windows games for Intel and AMD processors.";
        return r;
    }
    std::wstring dir = DirOf(game.exe), dll = Join(dir, kDll), backup = Join(dir, kBackup);
    std::vector<Op> ops;
    if (game.install == Install::Foreign) {
        if (FileExists(backup)) {
            r.ok = false;
            r.message = L"The game folder already has a file called " + std::wstring(kBackup) +
                        L" from an earlier time, so the current d3d11.dll can't be set aside.\n\nMove one of the two files "
                        L"out of the folder by hand, then try again.\n\n" + dir;
            return r;
        }
        ops.push_back({'m', dll, backup});
    }
    std::string ini = IniText(game.settings);
    std::wstring temp = TempPath(L".ini");
    if (!WriteBytes(temp, ini.data(), ini.size())) return Failed(GetLastError(), L"Couldn't prepare the settings file.");
    ops.push_back({'c', temp, Join(dir, kIni)});
    ops.push_back({'x', game.machine == IMAGE_FILE_MACHINE_AMD64 ? L"64" : L"32", dll});
    r = Execute(ops, game.needsAdmin, L"Couldn't put DX11on10's files into the game folder:\n" + dir);
    DeleteFileW(temp.c_str());
    RefreshState(game, RunningFolders());
    return r;
}

Result Disable(Game& game) {
    std::wstring dir = DirOf(game.exe), dll = Join(dir, kDll), backup = Join(dir, kBackup);
    std::vector<Op> ops;
    if (game.install == Install::On || game.install == Install::Outdated) ops.push_back({'d', dll, L""});
    ops.push_back({'d', Join(dir, kIni), L""});
    ops.push_back({'d', Join(dir, kLog), L""});
    if (game.backup && game.install != Install::Foreign) ops.push_back({'m', backup, dll});
    Result r = Execute(ops, game.needsAdmin, L"Couldn't remove DX11on10's files from the game folder:\n" + dir);
    RefreshState(game, RunningFolders());
    return r;
}

Result WriteGameIni(Game& game) {
    std::string ini = IniText(game.settings);
    std::wstring temp = TempPath(L".ini");
    if (!WriteBytes(temp, ini.data(), ini.size())) return Failed(GetLastError(), L"Couldn't prepare the settings file.");
    Result r = Execute({{'c', temp, Join(DirOf(game.exe), kIni)}}, game.needsAdmin, L"Couldn't save the settings.");
    DeleteFileW(temp.c_str());
    return r;
}

int RunOpsFile(const std::wstring& path) {
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(path, bytes) || bytes.size() < 2) return ERROR_FILE_NOT_FOUND;
    std::wstring text(reinterpret_cast<const wchar_t*>(bytes.data()), bytes.size() / sizeof(wchar_t));
    if (!text.empty() && text[0] == 0xFEFF) text.erase(0, 1);
    std::vector<Op> ops;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(start, end - start);
        start = end + 1;
        size_t t1 = line.find(L'\t'), t2 = t1 == std::wstring::npos ? t1 : line.find(L'\t', t1 + 1);
        if (line.empty() || t2 == std::wstring::npos) continue;
        ops.push_back({line[0], line.substr(t1 + 1, t2 - t1 - 1), line.substr(t2 + 1)});
    }
    return int(RunOps(ops));
}

// ---- what the user should know about a game ----

std::vector<Notice> GameNotices(const Game& g, const GpuInfo& gpu) {
    std::vector<Notice> out;
    bool on = g.install == Install::On || g.install == Install::Outdated;
    if (!g.antiCheat.empty()) {
        std::wstring names = g.antiCheat[0];
        for (size_t i = 1; i < g.antiCheat.size(); i++) names += L", " + g.antiCheat[i];
        out.push_back({Level::Danger, L"Uses anti-cheat: " + names,
                       L"Anti-cheat can treat DX11on10's d3d11.dll as a cheat. You could be kicked from online play or "
                       L"banned. Only use DX11on10 in offline modes, at your own risk."});
    }
    if (g.exe.empty() || !FileExists(g.exe)) {
        out.push_back({Level::Danger, L"Game .exe not found",
                       L"Use Change .exe to pick the file that starts the game."});
    } else if (g.machine != IMAGE_FILE_MACHINE_AMD64 && g.machine != IMAGE_FILE_MACHINE_I386) {
        out.push_back({Level::Danger, L"Unsupported game",
                       g.machine ? L"This .exe is for a different kind of processor (" + MachineName(g.machine) +
                                       L"). DX11on10 works with 32-bit and 64-bit x86 games."
                                 : L"The game's .exe couldn't be read. Use Change .exe if the wrong file was picked."});
    }
    if (g.machine && !(g.apis & ApiD3D11)) {
        if (g.apis & (ApiD3D12 | ApiVulkan))
            out.push_back({Level::Warning, L"This game uses " + ApiNames(g.apis & (ApiD3D12 | ApiVulkan)),
                           L"DX11on10 only helps games that use DirectX 11. Look in the game's settings or launch "
                           L"options for a DirectX 11 mode."});
        else if (g.apis & (ApiD3D9 | ApiD3D10 | ApiOpenGL))
            out.push_back({Level::Info, L"Doesn't use DirectX 11",
                           L"It uses " + ApiNames(g.apis) + L", which DirectX 10 graphics cards can already run. "
                           L"It doesn't need DX11on10."});
        else
            out.push_back({Level::Info, L"DirectX 11 not detected",
                           L"DX11on10 couldn't find DirectX 11 in this game's files. You can still try it. If the "
                           L"wrong file was picked, use Change .exe."});
    } else if ((g.apis & ApiD3D11) && (g.apis & ApiD3D12) && !on) {
        out.push_back({Level::Info, L"Also has DirectX 12",
                       L"If the game has a graphics API option, choose DirectX 11. DX11on10 can't help in DirectX 12 "
                       L"mode."});
    }
    if (g.install == Install::Outdated)
        out.push_back({Level::Warning, L"Update available",
                       L"This game has " + (g.installedVersion.empty() ? std::wstring(L"another version") : L"version " + g.installedVersion) +
                           L" of DX11on10. Click Update to install version " DX11ON10_VERSION_W L"."});
    if (g.install == Install::Foreign)
        out.push_back({Level::Warning, L"Another d3d11.dll is in the game folder",
                       L"It belongs to " + g.foreignName + L". Turning DX11on10 on renames it to " + kBackup +
                           L" and puts it back when you turn DX11on10 off. " + g.foreignName +
                           L" won't work while DX11on10 is on."});
    else if (on && g.backup)
        out.push_back({Level::Info, L"A d3d11.dll was set aside",
                       L"The game folder's earlier d3d11.dll is saved as " + std::wstring(kBackup) +
                           L". It comes back when you turn DX11on10 off."});
    if (!g.dxgiMod.empty())
        out.push_back({Level::Warning, L"dxgi.dll mod found (" + g.dxgiMod + L")",
                       L"Mods that replace dxgi.dll can stop DX11on10 from working. If the game doesn't start, "
                       L"remove that mod while you use DX11on10."});
    if (g.running)
        out.push_back({Level::Info, L"The game is running",
                       L"Close it before you turn DX11on10 on or off. Settings apply the next time it starts."});
    if (g.needsAdmin && g.install != Install::On)
        out.push_back({Level::Info, L"Protected folder",
                       L"This game is installed in a folder that needs administrator rights. Windows will ask for "
                       L"permission when you turn DX11on10 on or off."});
    if (on && g.report.present && !g.running) {
        const RunReport& r = g.report;
        if (r.exceptions)
            out.push_back({Level::Danger, L"The game crashed last time",
                           L"DX11on10 logged an error. Try Graphics level 10.0, or turn DX11on10 off. Sending "
                           L"dx11on10.log to the developers helps them fix it."});
        else if (r.createFailed)
            out.push_back({Level::Danger, L"The graphics driver refused to start",
                           L"Update your graphics driver. CPU rendering also works, but very slowly."});
        else if (r.refused)
            out.push_back({Level::Warning, L"The game asked for DirectX 11 only",
                           L"Set \"Tell the game it has DirectX 11\" to Always, then start the game again."});
        else if (!r.firstFrame && !r.realLevel.empty())
            out.push_back({Level::Warning, L"No picture last time",
                           L"The game closed before it drew anything. Look at dx11on10.log, or try the other "
                           L"settings below."});
        if (r.drawsSkipped || r.dispatchesSkipped)
            out.push_back({Level::Info, L"Some effects are skipped",
                           L"A few effects need features DirectX 10 cards don't have (like tessellation or advanced "
                           L"compute shaders). Expect missing details such as shadows, grass or post-processing."});
    }
    if (on && gpu.level >= 0xb000 && !g.settings.warp)
        out.push_back({Level::Info, L"Testing mode",
                       L"Your graphics card supports DirectX 11, so this game is limited to DirectX 10 features "
                       L"while DX11on10 is on."});
    return out;
}

}  // namespace app
