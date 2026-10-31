// DX11on10 app: finds installed games and turns the DX11on10 layer on or off for each one
// by putting d3d11.dll and dx11on10.ini next to the game's .exe (and removing them again).
#pragma once
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601  // runs on Windows 7 SP1; newer APIs are loaded at run time
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "../src/common/version.h"

namespace app {

// ---- strings and files (system.cpp) ----
std::wstring Widen(const std::string& s);  // UTF-8
std::string Narrow(const std::wstring& s);
std::wstring Lower(std::wstring s);
std::wstring Format(const wchar_t* format, ...);
std::wstring DirOf(const std::wstring& path);
std::wstring FileName(const std::wstring& path);
std::wstring Stem(const std::wstring& path);
std::wstring Join(const std::wstring& dir, const std::wstring& name);
bool FileExists(const std::wstring& path);
bool DirExists(const std::wstring& path);
bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out, size_t limit = SIZE_MAX);
uint64_t FileSize(const std::wstring& path);
std::wstring ErrorText(DWORD error);
std::wstring SelfPath();

// Version resource strings of a .exe/.dll ("ProductName", "FileDescription", ...).
std::wstring VersionString(const std::wstring& path, const wchar_t* name);

// ---- settings (%APPDATA%\DX11on10\settings.ini) ----
std::wstring SettingsDir();
std::wstring GetSetting(const std::wstring& section, const wchar_t* key, const wchar_t* fallback = L"");
// A null value removes the key; a null key removes the whole section.
void SetSetting(const std::wstring& section, const wchar_t* key, const std::wstring* value);
std::vector<std::pair<std::wstring, std::wstring>> SettingsSection(const std::wstring& section);

// ---- graphics card ----
struct GpuInfo {
    bool runtime = true;        // d3d11.dll could be loaded
    std::wstring name;          // first hardware adapter
    unsigned level = 0;         // its highest D3D_FEATURE_LEVEL, 0 if none
    unsigned others = 0;        // further hardware adapters
};
GpuInfo DetectGpu();
std::wstring LevelName(unsigned level);  // "DirectX 10.1"

// ---- games (scan.cpp) ----
enum class Store { Steam, GOG, Epic, Custom };
enum Api : unsigned { ApiD3D9 = 1, ApiD3D10 = 2, ApiD3D11 = 4, ApiD3D12 = 8, ApiVulkan = 16, ApiOpenGL = 32 };

struct GameSettings {
    bool fakeAlways = true;  // FakeFeatureLevel=always, else auto
    bool level100 = false;   // MaxFeatureLevel=10_0
    bool warp = false;       // Driver=warp (CPU rendering)
};

enum class Install { Off, On, Outdated, Foreign };  // Foreign: another program's d3d11.dll is there

// What the game's dx11on10.log says about its last start.
struct RunReport {
    bool present = false;
    FILETIME written{};
    std::string realLevel, gameLevel;  // "10_1", "11_0"
    bool firstFrame = false;
    double fps = 0;
    unsigned converted = 0, placeholders = 0, drawsSkipped = 0, dispatchesSkipped = 0, emulated = 0;
    unsigned computeUnsupported = 0;
    unsigned exceptions = 0;
    bool refused = false;       // the game only accepted levels the card doesn't have
    bool createFailed = false;  // the driver couldn't create a device
};

struct Game {
    std::wstring id;  // "steam:489830", "gog:...", "epic:...", "exe:<hash>"
    std::wstring name;
    Store store = Store::Custom;
    std::wstring storeId;  // Steam app id, GOG id, Epic app name
    std::wstring root;     // install folder
    std::wstring exe;      // the .exe that gets d3d11.dll next to it
    std::wstring storeExe; // what the store says the .exe is, if it says
    std::wstring art;      // header image, may be empty
    bool exeFromUser = false;

    // from the files (slow: scan thread)
    unsigned machine = 0;  // IMAGE_FILE_MACHINE_*, 0 if the .exe couldn't be read
    unsigned apis = 0;
    std::vector<std::wstring> antiCheat;

    // from the game folder (fast)
    Install install = Install::Off;
    std::wstring installedVersion;
    std::wstring foreignName;  // who the other d3d11.dll belongs to
    std::wstring dxgiMod;      // a dxgi.dll mod, if any
    bool backup = false;       // d3d11.dll.dx11on10-backup exists
    bool running = false;
    bool needsAdmin = false;
    RunReport report;
    GameSettings settings;
};

std::vector<Game> FindGames();          // all stores + games the user added; analysed
void Analyze(Game& game);               // pick the .exe (unless the user did), APIs, anti-cheat
Game CustomGame(const std::wstring& exe);
std::wstring CustomId(const std::wstring& exe);
std::wstring StoreName(Store store);
std::wstring MachineName(unsigned machine);  // "64-bit", "32-bit", "ARM64"
std::wstring ApiNames(unsigned apis);        // "DirectX 11 + 12"

// ---- installing (install.cpp) ----
std::set<std::wstring> RunningFolders();  // lower-case folders of running programs
void RefreshState(Game& game, const std::set<std::wstring>& running);
void LoadGameSettings(Game& game);
void SaveGameSettings(const Game& game);

struct Result {
    bool ok = true;
    bool cancelled = false;  // the user said no to the administrator prompt
    std::wstring message;
};
Result Enable(Game& game);    // install or update
Result Disable(Game& game);
Result WriteGameIni(Game& game);  // after a settings change while on
int RunOpsFile(const std::wstring& path);  // the elevated helper (--ops)
bool ProxyIdentical(const std::wstring& path, unsigned machine);

// ---- warnings shown for a game ----
enum class Level { Info, Success, Warning, Danger };
struct Notice {
    Level level;
    std::wstring title, body;
};
std::vector<Notice> GameNotices(const Game& game, const GpuInfo& gpu);

}  // namespace app
