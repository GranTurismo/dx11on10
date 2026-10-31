// The window, its messages, and what each button does.
//
// Command line (for the elevated helper and for testing):
//   --ops <file>                  run the file steps listed in <file> (started elevated by the app)
//   --report <out.txt>            write what the scan found
//   --screenshot <out.png> [--size WxH] [--scale S] [--theme light|dark] [--select text] [--all]
//                [--gpu 10_1|11_0|9_3] [--toast text] [--scroll N]
//   --toggle <text> <out.txt>     turn the first matching game on or off, answering every question "yes"
//   --add <exe>                   add a game to the list
//   --hits <file>                 (with the window) write where each control is after every frame, for
//                                 tools/input.ps1's click steps
#include "app.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstdio>

#include "resource.h"
#include "ui.h"

namespace app {
namespace {

constexpr UINT kScanned = WM_APP + 1;
constexpr UINT kFirstRun = WM_APP + 2;
constexpr UINT_PTR kAnimTimer = 1;
constexpr UINT_PTR kRunningTimer = 2;  // notices games starting and stopping

HWND g_window = nullptr;
AppState g_state;
View g_view;
Com<ID2D1HwndRenderTarget> g_rt;
float g_dpi = 96;
bool g_timer = false;
bool g_tracking = false;
bool g_headless = false;
std::wstring g_headlessLog;
std::wstring g_hitsPath;
struct ScanResult {
    std::vector<Game> games;
    GpuInfo gpu;
};

// Actions keep references into g_state.games across dialogs, which run their own message
// loops; a scan that finishes meanwhile waits until the action is done.
int g_busy = 0;
ScanResult* g_pendingScan = nullptr;
void OnScanned(ScanResult* result);

struct Busy {
    Busy() { g_busy++; }
    ~Busy() {
        if (--g_busy == 0 && g_pendingScan) {
            ScanResult* r = g_pendingScan;
            g_pendingScan = nullptr;
            OnScanned(r);
        }
    }
};

// ---- helpers ----

float DipX(LPARAM l) { return float(short(LOWORD(l))) * 96.f / g_dpi; }
float DipY(LPARAM l) { return float(short(HIWORD(l))) * 96.f / g_dpi; }

void Redraw() {
    if (g_window) InvalidateRect(g_window, nullptr, FALSE);
}

UINT WindowDpi(HWND window) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static auto get = reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (get && window) return get(window);
    HDC dc = GetDC(nullptr);
    UINT dpi = UINT(GetDeviceCaps(dc, LOGPIXELSX));
    ReleaseDC(nullptr, dc);
    return dpi;
}

bool SystemDark() {
    DWORD value = 1, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return false;
    return value == 0;
}

void ApplyTheme() {
    g_view.SetDark(SystemDark());
    if (!g_window) return;
    BOOL dark = g_view.Dark();
    if (FAILED(DwmSetWindowAttribute(g_window, 20, &dark, sizeof(dark))))  // DWMWA_USE_IMMERSIVE_DARK_MODE
        DwmSetWindowAttribute(g_window, 19, &dark, sizeof(dark));          // the same before Windows 10 20H1
}

void Toast(const std::wstring& text, Level level = Level::Success) {
    if (g_headless) g_headlessLog += L"toast: " + text + L"\n";
    g_state.toast = text;
    g_state.toastLevel = level;
    g_state.toastAt = GetTickCount();
    Redraw();
}

struct DialogButton {
    int id;
    std::wstring text;
};

// Keeps button `data` disabled while the dialog's check box is clear.
HRESULT CALLBACK GateButton(HWND dialog, UINT message, WPARAM wParam, LPARAM, LONG_PTR data) {
    if (data && message == TDN_CREATED) SendMessageW(dialog, TDM_ENABLE_BUTTON, WPARAM(data), FALSE);
    if (data && message == TDN_VERIFICATION_CLICKED) SendMessageW(dialog, TDM_ENABLE_BUTTON, WPARAM(data), wParam);
    return S_OK;
}

// A Windows task dialog. With `gate`, the button with that id stays disabled until the check
// box is ticked. Returns the id of the button pressed (IDCANCEL when closed).
int Ask(PCWSTR icon, const std::wstring& title, const std::wstring& content, const std::vector<DialogButton>& buttons,
        int defaultId, const wchar_t* check = nullptr, bool* checked = nullptr, int gate = 0) {
    if (g_headless) {
        g_headlessLog += L"dialog: " + title + L"\n";
        if (checked) *checked = true;
        return buttons.empty() ? IDOK : buttons.front().id;
    }
    std::vector<TASKDIALOG_BUTTON> list;
    for (const DialogButton& b : buttons) list.push_back({b.id, b.text.c_str()});
    TASKDIALOGCONFIG c = {};
    c.cbSize = sizeof(c);
    c.hwndParent = g_window;
    c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    if (buttons.empty()) c.dwCommonButtons = TDCBF_OK_BUTTON;
    c.pszWindowTitle = L"DX11on10";
    c.pszMainIcon = icon;
    c.pszMainInstruction = title.c_str();
    c.pszContent = content.c_str();
    c.cButtons = UINT(list.size());
    c.pButtons = list.empty() ? nullptr : list.data();
    c.nDefaultButton = defaultId;
    c.pszVerificationText = check;
    c.lpCallbackData = gate;
    c.pfCallback = GateButton;
    int pressed = IDCANCEL;
    BOOL ticked = FALSE;
    if (FAILED(TaskDialogIndirect(&c, &pressed, nullptr, &ticked))) {
        // Without the version 6 common controls (shouldn't happen: the manifest asks for them).
        UINT type = MB_ICONWARNING | (buttons.size() > 1 ? MB_OKCANCEL : MB_OK);
        pressed = MessageBoxW(g_window, (title + L"\n\n" + content).c_str(), L"DX11on10", type) == IDOK
                      ? (buttons.empty() ? IDOK : buttons.front().id)
                      : IDCANCEL;
        ticked = TRUE;
    }
    if (checked) *checked = ticked;
    return pressed;
}

void Error(const std::wstring& title, const std::wstring& content) {
    Ask(TD_ERROR_ICON, title, content, {}, 0);
}

Game* Selected() { return g_state.selected >= 0 ? &g_state.games[g_state.selected] : nullptr; }

bool IsOn(const Game& g) { return g.install == Install::On || g.install == Install::Outdated; }

int Rank(const Game& g) {
    if (IsOn(g)) return 0;
    if ((g.apis & ApiD3D11) || g.store == Store::Custom) return 1;
    return 2;
}

void SortGames() {
    std::wstring keep = Selected() ? Selected()->id : L"";
    std::stable_sort(g_state.games.begin(), g_state.games.end(), [](const Game& a, const Game& b) {
        if (Rank(a) != Rank(b)) return Rank(a) < Rank(b);
        return lstrcmpiW(a.name.c_str(), b.name.c_str()) < 0;
    });
    g_state.selected = -1;
    for (int i = 0; i < int(g_state.games.size()); i++)
        if (g_state.games[i].id == keep) g_state.selected = i;
}

void EnsureSelection() {
    std::vector<int> visible = VisibleGames(g_state);
    if (std::find(visible.begin(), visible.end(), g_state.selected) == visible.end())
        g_state.selected = visible.empty() ? -1 : visible.front();
}

void RefreshAll() {
    std::set<std::wstring> running = RunningFolders();
    for (Game& g : g_state.games) RefreshState(g, running);
}

void RefreshOne(Game& g) { RefreshState(g, RunningFolders()); }

// Cheap: only which games run. A game that just stopped also gets its log read again.
void RefreshRunning() {
    std::set<std::wstring> running = RunningFolders();
    bool changed = false;
    for (Game& g : g_state.games) {
        bool now = !g.exe.empty() && running.count(Lower(DirOf(g.exe))) > 0;
        if (now == g.running) continue;
        changed = true;
        if (now) g.running = true;
        else RefreshState(g, running);
    }
    if (changed) Redraw();
}

// ---- scanning ----

DWORD WINAPI ScanThread(void*) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto* result = new ScanResult;
    result->gpu = DetectGpu();
    result->games = FindGames();
    std::set<std::wstring> running = RunningFolders();
    for (Game& g : result->games) RefreshState(g, running);
    if (!PostMessageW(g_window, kScanned, 0, reinterpret_cast<LPARAM>(result))) delete result;
    CoUninitialize();
    return 0;
}

void StartScan() {
    if (g_state.scanning) return;
    g_state.scanning = true;
    HANDLE thread = CreateThread(nullptr, 0, ScanThread, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    else g_state.scanning = false;
    Redraw();
}

void OnScanned(ScanResult* result) {
    if (g_busy) {
        delete g_pendingScan;
        g_pendingScan = result;
        return;
    }
    std::wstring keep = Selected() ? Selected()->id : L"";
    g_state.games = std::move(result->games);
    g_state.gpu = result->gpu;
    g_state.gpuKnown = true;
    g_state.scanning = false;
    delete result;
    g_state.selected = -1;
    for (int i = 0; i < int(g_state.games.size()); i++)
        if (g_state.games[i].id == keep) g_state.selected = i;
    SortGames();
    EnsureSelection();
    Redraw();
}

// ---- turning on and off ----

bool CheckNotRunning(Game& g) {
    RefreshOne(g);
    if (!g.running) return true;
    Error(L"Close " + g.name + L" first",
          L"Windows doesn't let DX11on10 change the files of a game that is running. Close the game and its "
          L"launcher, then try again.");
    return false;
}

void ShowResult(const Result& r, Game& g, const std::wstring& done, const wchar_t* failTitle) {
    if (r.ok) Toast(done);
    else if (r.cancelled) Toast(r.message, Level::Info);
    else Error(failTitle, r.message);
    (void)g;
}

void TurnOn(Game& g, bool update) {
    if (!update) {
        if (g.machine != IMAGE_FILE_MACHINE_AMD64 && g.machine != IMAGE_FILE_MACHINE_I386) {
            Error(L"DX11on10 can't be used with this game",
                  g.machine ? L"Its .exe is for a different kind of processor (" + MachineName(g.machine) +
                                  L"). DX11on10 works with 32-bit and 64-bit games for Intel and AMD processors."
                            : L"The game's .exe couldn't be read. If the wrong file was picked, use Change .exe.");
            return;
        }
        const GpuInfo& gpu = g_state.gpu;
        if (g_state.gpuKnown && !gpu.runtime) {
            Error(L"DirectX 11 is missing from Windows",
                  L"DX11on10 and DirectX 11 games need the DirectX 11 part of Windows.\n\nOn Windows 7, install "
                  L"Service Pack 1 and the Platform Update (KB2670838) from Windows Update.");
            return;
        }
        if (g_state.gpuKnown && gpu.level >= 0xb000 && !g.settings.warp &&
            GetSetting(L"app", L"dx11Confirmed") != L"1") {
            bool dontAsk = false;
            int answer = Ask(TD_INFORMATION_ICON, L"Your graphics card already supports DirectX 11",
                             gpu.name + L" runs DirectX 11 games by itself. DX11on10 is made for DirectX 10 cards: here "
                                        L"it would limit " + g.name +
                                 L" to DirectX 10 features, which can look worse or run slower.\n\nTurn it on only to "
                                 L"test how a game behaves on a DirectX 10 card.",
                             {{100, L"Turn on for testing"}, {IDCANCEL, L"Cancel"}}, IDCANCEL,
                             L"Don't ask again on this PC", &dontAsk);
            if (answer != 100) return;
            if (dontAsk) {
                std::wstring one = L"1";
                SetSetting(L"app", L"dx11Confirmed", &one);
            }
        }
        if (g_state.gpuKnown && gpu.level && gpu.level < 0xa000 && !g.settings.warp) {
            int answer = Ask(TD_WARNING_ICON, L"Your graphics card only supports DirectX 9",
                             L"DX11on10 needs a DirectX 10 graphics card. With " + gpu.name + L", " + g.name +
                                 L" won't start unless you turn on CPU rendering in its settings, which is very slow.",
                             {{100, L"Turn on anyway"}, {IDCANCEL, L"Cancel"}}, IDCANCEL);
            if (answer != 100) return;
        }
        if (!g.antiCheat.empty()) {
            std::wstring names = g.antiCheat[0];
            for (size_t i = 1; i < g.antiCheat.size(); i++) names += L" and " + g.antiCheat[i];
            int answer = Ask(TD_ERROR_ICON, L"This game uses anti-cheat",
                             g.name + L" comes with " + names +
                                 L". Anti-cheat programs check the game's files, and DX11on10's d3d11.dll can look like a "
                                 L"cheat to them.\n\nThis can get you kicked from online play or banned for good. Nobody "
                                 L"can undo a ban for you. Only use DX11on10 in the game's offline modes.",
                             {{100, L"Turn on anyway"}, {IDCANCEL, L"Cancel"}}, IDCANCEL,
                             L"I'll only play offline, and I accept the risk of a ban", nullptr, 100);
            if (answer != 100) return;
        }
        if (!(g.apis & ApiD3D11)) {
            std::wstring why;
            if (g.apis & (ApiD3D12 | ApiVulkan))
                why = L"It uses " + ApiNames(g.apis & (ApiD3D12 | ApiVulkan)) +
                      L". DX11on10 only changes how DirectX 11 runs, so it will most likely make no difference.";
            else if (g.apis)
                why = L"It uses " + ApiNames(g.apis) +
                      L", which DirectX 10 graphics cards already run. DX11on10 will most likely make no difference.";
            else
                why = L"DX11on10 couldn't find DirectX 11 in the game's files. The game might load it in a way DX11on10 "
                      L"can't see, or a launcher .exe might have been picked instead of the game.";
            int answer = Ask(TD_WARNING_ICON, g.name + L" doesn't seem to use DirectX 11", why,
                             {{100, L"Turn on anyway"}, {IDCANCEL, L"Cancel"}}, IDCANCEL);
            if (answer != 100) return;
        }
        if (g.install == Install::Foreign) {
            int answer = Ask(TD_WARNING_ICON, L"Another d3d11.dll is in the way",
                             g.name + L"'s folder already has a d3d11.dll from " + g.foreignName +
                                 L".\n\nDX11on10 will rename it to d3d11.dll.dx11on10-backup and put it back when you "
                                 L"turn DX11on10 off. " + g.foreignName + L" won't work while DX11on10 is on.",
                             {{100, L"Rename it and turn on"}, {IDCANCEL, L"Cancel"}}, IDCANCEL);
            if (answer != 100) return;
        }
    }
    if (!CheckNotRunning(g)) return;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    Result r = Enable(g);
    ShowResult(r, g,
               update ? L"DX11on10 for " + g.name + L" is up to date."
                      : L"DX11on10 is on for " + g.name + L". Start the game as usual.",
               update ? L"Couldn't update DX11on10" : L"Couldn't turn on DX11on10");
}

void TurnOff(Game& g) {
    if (!CheckNotRunning(g)) return;
    bool hadBackup = g.backup;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    Result r = Disable(g);
    ShowResult(r, g,
               L"DX11on10 is off for " + g.name + L"." +
                   (hadBackup && r.ok ? L" Its earlier d3d11.dll is back." : L""),
               L"Couldn't turn off DX11on10");
}

void Toggle(int index) {
    Game& g = g_state.games[index];
    if (IsOn(g)) TurnOff(g);
    else TurnOn(g, false);
    SortGames();
    Redraw();
}

void ChangeSetting(Game& g, GameSettings next) {
    if (next.fakeAlways == g.settings.fakeAlways && next.level100 == g.settings.level100 && next.warp == g.settings.warp)
        return;
    if (next.warp && !g.settings.warp) {
        int answer = Ask(TD_WARNING_ICON, L"Use CPU rendering?",
                         L"The processor draws every frame instead of the graphics card. Most games will run at only a "
                         L"few frames per second. This is meant for PCs whose graphics card can't run DirectX 10 at all.",
                         {{100, L"Use CPU rendering"}, {IDCANCEL, L"Cancel"}}, IDCANCEL);
        if (answer != 100) return;
    }
    GameSettings before = g.settings;
    g.settings = next;
    if (IsOn(g)) {
        Result r = WriteGameIni(g);
        if (!r.ok) {
            g.settings = before;
            if (r.cancelled) Toast(r.message, Level::Info);
            else Error(L"Couldn't save the setting", r.message);
            Redraw();
            return;
        }
        Toast(L"Saved. It applies the next time " + g.name + L" starts.");
    }
    SaveGameSettings(g);
    Redraw();
}

// ---- other buttons ----

void Play(Game& g) {
    std::wstring target, dir;
    if (g.store == Store::Steam) target = L"steam://rungameid/" + g.storeId;
    else if (g.store == Store::Epic) target = L"com.epicgames.launcher://apps/" + g.storeId + L"?action=launch&silent=true";
    else target = g.exe, dir = DirOf(g.exe);
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(g_window, L"open", target.c_str(), nullptr,
                                                dir.empty() ? nullptr : dir.c_str(), SW_SHOWNORMAL)) <= 32)
        Error(L"Couldn't start " + g.name, ErrorText(GetLastError()));
    else
        Toast(L"Starting " + g.name + L"…", Level::Info);
}

void OpenFolder(const Game& g) {
    if (FileExists(g.exe)) {
        std::wstring args = L"/select,\"" + g.exe + L"\"";
        ShellExecuteW(g_window, nullptr, L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    } else {
        ShellExecuteW(g_window, L"open", g.root.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void OpenLog(const Game& g) {
    std::wstring log = Join(DirOf(g.exe), L"dx11on10.log");
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(g_window, L"open", log.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        ShellExecuteW(g_window, nullptr, L"notepad.exe", (L"\"" + log + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}

std::wstring PickExe(const std::wstring& folder) {
    Com<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                                reinterpret_cast<void**>(dialog.Put()))))
        return {};
    COMDLG_FILTERSPEC filter[] = {{L"Programs (*.exe)", L"*.exe"}};
    dialog->SetFileTypes(1, filter);
    dialog->SetTitle(L"Choose the game's .exe");
    dialog->SetOptions(FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR);
    if (!folder.empty()) {
        Com<IShellItem> item;
        if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_IShellItem,
                                                  reinterpret_cast<void**>(item.Put()))))
            dialog->SetFolder(item.get());
    }
    if (dialog->Show(g_window) != S_OK) return {};
    Com<IShellItem> result;
    wchar_t* path = nullptr;
    std::wstring out;
    if (SUCCEEDED(dialog->GetResult(result.Put())) && SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        out = path;
        CoTaskMemFree(path);
    }
    return out;
}

void AddGame(const std::wstring& exe) {
    for (int i = 0; i < int(g_state.games.size()); i++)
        if (Lower(g_state.games[i].exe) == Lower(exe)) {
            g_state.selected = i;
            Toast(g_state.games[i].name + L" is already in the list.", Level::Info);
            return;
        }
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    Game g = CustomGame(exe);
    Analyze(g);
    LoadGameSettings(g);
    RefreshOne(g);
    SetSetting(L"custom", g.id.c_str(), &exe);
    g_state.games.push_back(g);
    g_state.selected = int(g_state.games.size()) - 1;
    SortGames();
    Toast(g.name + L" was added.");
}

void ChangeExe(Game& g) {
    if (IsOn(g)) {
        Ask(TD_INFORMATION_ICON, L"Turn DX11on10 off first",
            L"DX11on10's files are next to the current .exe. Turn DX11on10 off for " + g.name +
                L", change the .exe, then turn it on again.",
            {}, 0);
        return;
    }
    std::wstring exe = PickExe(g.exe.empty() ? g.root : DirOf(g.exe));
    if (exe.empty()) return;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    if (g.store == Store::Custom) {
        SetSetting(L"custom", g.id.c_str(), nullptr);
        SetSetting(L"game:" + g.id, nullptr, nullptr);
        Game replacement = CustomGame(exe);
        replacement.name = g.name;
        SetSetting(L"custom", replacement.id.c_str(), &exe);
        SetSetting(L"game:" + replacement.id, L"name", &replacement.name);
        replacement.settings = g.settings;
        SaveGameSettings(replacement);
        g = replacement;
    } else {
        g.exe = exe;
        g.exeFromUser = true;
        SetSetting(L"game:" + g.id, L"exe", &exe);
    }
    Analyze(g);
    RefreshOne(g);
    SortGames();
    Toast(L"The game's .exe is now " + FileName(exe) + L".");
}

void RemoveGame(int index) {
    Game& g = g_state.games[index];
    std::wstring what = IsOn(g) ? L"DX11on10 is turned off for it first, so its files are removed from the game folder."
                                : L"The game itself isn't changed.";
    if (Ask(TD_INFORMATION_ICON, L"Remove " + g.name + L" from the list?", what,
            {{100, L"Remove"}, {IDCANCEL, L"Cancel"}}, 100) != 100)
        return;
    if (IsOn(g)) {
        TurnOff(g);
        if (IsOn(g)) return;
    }
    SetSetting(L"custom", g.id.c_str(), nullptr);
    SetSetting(L"game:" + g.id, nullptr, nullptr);
    std::wstring name = g.name;
    g_state.games.erase(g_state.games.begin() + index);
    g_state.selected = -1;
    EnsureSelection();
    Toast(name + L" was removed from the list.", Level::Info);
}

void About() {
    Ask(TD_INFORMATION_ICON, L"DX11on10 " DX11ON10_VERSION_W,
        L"Runs DirectX 11 games on DirectX 10 graphics cards by translating what they ask for into DirectX 10.\n\n"
        L"For each game you turn it on for, the app puts d3d11.dll and dx11on10.ini next to the game's .exe. The game "
        L"writes dx11on10.log there when it runs. Turning DX11on10 off removes these files and puts back anything that "
        L"was set aside.\n\nApp settings: " + SettingsDir() +
            L"\n\nIncludes code from Microsoft's DirectX shader format headers (University of Illinois/NCSA licence).",
        {}, 0);
}

void FirstRun() {
    if (GetSetting(L"app", L"accepted") == L"1") return;
    int answer = Ask(TD_INFORMATION_ICON, L"Before you start",
                     L"DX11on10 lets DirectX 11 games run on DirectX 10 graphics cards. It puts a small file "
                     L"(d3d11.dll) next to each game you turn it on for.\n\n"
                     L"•  It's experimental: some games won't start, and graphics can look different or run slower.\n"
                     L"•  Never use it with online games that have anti-cheat (Easy Anti-Cheat, BattlEye and others). "
                     L"The anti-cheat can treat it as a cheat and ban your account.\n"
                     L"•  Turning DX11on10 off for a game removes its files and puts back anything it moved.",
                     {{100, L"Continue"}, {IDCANCEL, L"Quit"}}, 100, L"I understand", nullptr, 100);
    if (answer != 100) {
        DestroyWindow(g_window);
        return;
    }
    std::wstring one = L"1";
    SetSetting(L"app", L"accepted", &one);
}

void OnClick(uint32_t id) {
    Busy busy;
    Game* g = Selected();
    uint32_t index = IndexOf(id);
    switch (KindOf(id)) {
        case HitRow: g_state.selected = int(index); break;
        case HitRowSwitch:
            g_state.selected = int(index);
            Redraw();
            Toggle(int(index));
            break;
        case HitFilter:
            g_state.showAll = index == 1;
            EnsureSelection();
            break;
        case HitShowAll:
            g_state.showAll = true;
            EnsureSelection();
            break;
        case HitAdd: {
            std::wstring exe = PickExe(L"");
            if (!exe.empty()) AddGame(exe);
            break;
        }
        case HitRefresh: StartScan(); break;
        case HitAbout: About(); break;
        case HitSwitch:
            if (g) Toggle(g_state.selected);
            break;
        case HitUpdate:
            if (g) {
                TurnOn(*g, true);
                SortGames();
            }
            break;
        case HitPlay:
            if (g) Play(*g);
            break;
        case HitFolder:
            if (g) OpenFolder(*g);
            break;
        case HitLog:
            if (g) OpenLog(*g);
            break;
        case HitChangeExe:
            if (g) ChangeExe(*g);
            break;
        case HitRemove:
            if (g) RemoveGame(g_state.selected);
            break;
        case HitFake:
            if (g) {
                GameSettings s = g->settings;
                s.fakeAlways = index == 0;
                ChangeSetting(*g, s);
            }
            break;
        case HitLevel:
            if (g) {
                GameSettings s = g->settings;
                s.level100 = index == 1;
                ChangeSetting(*g, s);
            }
            break;
        case HitWarp:
            if (g) {
                GameSettings s = g->settings;
                s.warp = !s.warp;
                ChangeSetting(*g, s);
            }
            break;
        default: break;
    }
    Redraw();
}

void MoveSelection(int step) {
    std::vector<int> visible = VisibleGames(g_state);
    if (visible.empty()) return;
    auto it = std::find(visible.begin(), visible.end(), g_state.selected);
    int pos = it == visible.end() ? 0 : int(it - visible.begin()) + step;
    pos = std::max(0, std::min(pos, int(visible.size()) - 1));
    g_state.selected = visible[pos];
    g_view.ScrollToRow(pos);
    Redraw();
}

// ---- window ----

void DumpHits();

void Paint() {
    RECT rc;
    GetClientRect(g_window, &rc);
    if (!g_rt) {
        D2D1_RENDER_TARGET_PROPERTIES props = {D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                               {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED},
                                               g_dpi, g_dpi, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT};
        D2D1_HWND_RENDER_TARGET_PROPERTIES hwnd = {g_window, {UINT32(rc.right), UINT32(rc.bottom)},
                                                   D2D1_PRESENT_OPTIONS_NONE};
        if (FAILED(g_view.Factory()->CreateHwndRenderTarget(&props, &hwnd, g_rt.Put()))) return;
    }
    g_rt->BeginDraw();
    g_view.Render(g_rt.get(), g_state, float(rc.right) * 96.f / g_dpi, float(rc.bottom) * 96.f / g_dpi, g_dpi);
    if (g_rt->EndDraw(nullptr, nullptr) == D2DERR_RECREATE_TARGET) {
        g_view.DiscardDeviceResources();
        g_rt.Reset();
        Redraw();
    }
    if (!g_hitsPath.empty()) DumpHits();
    bool animate = g_view.Animating();
    if (animate && !g_timer) g_timer = SetTimer(g_window, kAnimTimer, 15, nullptr) != 0;
    if (!animate && g_timer) {
        KillTimer(g_window, kAnimTimer);
        g_timer = false;
    }
}

void WriteText(const std::wstring& path, const std::wstring& text);

void DumpHits() {
    static const wchar_t* const kNames[] = {L"none",   L"row",    L"rowswitch", L"filter",    L"add",
                                            L"refresh", L"about", L"switch",    L"play",      L"update",
                                            L"folder", L"log",    L"changeexe", L"remove",    L"fake",
                                            L"level",  L"warp",   L"showall",   L"listbar",   L"detailbar"};
    std::wstring text = Format(L"dpi\t%.0f\n", g_dpi);
    for (const View::HitRect& h : g_view.Hits()) {
        Hit kind = KindOf(h.id);
        uint32_t index = IndexOf(h.id);
        std::wstring label;
        if ((kind == HitRow || kind == HitRowSwitch) && index < g_state.games.size()) label = g_state.games[index].name;
        text += Format(L"%ls\t%u\t%.1f\t%.1f\t%.1f\t%.1f\t", kind < 20 ? kNames[kind] : L"?", index, h.rect.left,
                       h.rect.top, h.rect.right, h.rect.bottom) +
                label + L"\n";
    }
    std::wstring temp = g_hitsPath + L".tmp";
    WriteText(temp, text);
    MoveFileExW(temp.c_str(), g_hitsPath.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void SavePlacement() {
    WINDOWPLACEMENT wp = {sizeof(wp)};
    if (!GetWindowPlacement(g_window, &wp)) return;
    RECT& r = wp.rcNormalPosition;
    std::wstring value = Format(L"%ld,%ld,%ld,%ld,%d", r.left, r.top, r.right, r.bottom, wp.showCmd == SW_SHOWMAXIMIZED);
    SetSetting(L"app", L"window", &value);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(window, &ps);
            Paint();
            EndPaint(window, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_SIZE:
            if (g_rt) g_rt->Resize(D2D1_SIZE_U{LOWORD(lParam), HIWORD(lParam)});
            Redraw();
            return 0;
        case WM_TIMER:
            if (wParam == kRunningTimer) {
                if (!g_busy && !g_state.scanning) RefreshRunning();
            } else {
                Redraw();
            }
            return 0;
        case WM_DPICHANGED: {
            g_dpi = float(HIWORD(wParam));
            if (g_rt) g_rt->SetDpi(g_dpi, g_dpi);
            g_view.DiscardDeviceResources();
            g_rt.Reset();
            const RECT* r = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize = {LONG(960 * g_dpi / 96), LONG(640 * g_dpi / 96)};
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (lParam && lstrcmpW(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0) {
                ApplyTheme();
                Redraw();
            }
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wParam) != WA_INACTIVE && !g_state.scanning && !g_busy && !g_state.games.empty()) {
                RefreshAll();
                Redraw();
            }
            return 0;
        case WM_MOUSEMOVE: {
            if (!g_tracking) {
                TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, window, 0};
                g_tracking = TrackMouseEvent(&t);
            }
            if (g_view.Dragging()) {
                g_view.Drag(DipY(lParam));
                Redraw();
                return 0;
            }
            uint32_t hit = g_view.HitTest(DipX(lParam), DipY(lParam));
            if (hit != g_view.hover) {
                g_view.hover = hit;
                Redraw();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_tracking = false;
            if (g_view.hover && !g_view.Dragging()) {
                g_view.hover = 0;
                Redraw();
            }
            return 0;
        case WM_LBUTTONDOWN: {
            SetCapture(window);
            uint32_t hit = g_view.HitTest(DipX(lParam), DipY(lParam));
            g_view.pressed = hit;
            if (KindOf(hit) == HitListBar || KindOf(hit) == HitDetailBar) g_view.BeginDrag(hit, DipY(lParam));
            Redraw();
            return 0;
        }
        case WM_LBUTTONUP: {
            ReleaseCapture();
            uint32_t pressed = g_view.pressed;
            g_view.pressed = 0;
            if (g_view.Dragging()) {
                g_view.EndDrag();
            } else {
                uint32_t hit = g_view.HitTest(DipX(lParam), DipY(lParam));
                if (hit && hit == pressed) OnClick(hit);
            }
            Redraw();
            return 0;
        }
        case WM_MOUSEWHEEL: {
            POINT p = {short(LOWORD(lParam)), short(HIWORD(lParam))};
            ScreenToClient(window, &p);
            g_view.Wheel(float(p.x) * 96.f / g_dpi, float(p.y) * 96.f / g_dpi, float(GET_WHEEL_DELTA_WPARAM(wParam)));
            g_view.hover = g_view.HitTest(float(p.x) * 96.f / g_dpi, float(p.y) * 96.f / g_dpi);
            Redraw();
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                bool hand = g_view.hover && KindOf(g_view.hover) != HitListBar && KindOf(g_view.hover) != HitDetailBar;
                SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_KEYDOWN:
            switch (wParam) {
                case VK_UP: MoveSelection(-1); return 0;
                case VK_DOWN: MoveSelection(1); return 0;
                case VK_HOME: MoveSelection(-100000); return 0;
                case VK_END: MoveSelection(100000); return 0;
                case VK_SPACE:
                    if (Selected()) {
                        Busy busy;
                        Toggle(g_state.selected);
                    }
                    return 0;
                case VK_F5: StartScan(); return 0;
                case 'O':
                    if (GetKeyState(VK_CONTROL) < 0) OnClick(MakeHit(HitAdd));
                    return 0;
            }
            break;
        case WM_DROPFILES: {
            Busy busy;
            HDROP drop = reinterpret_cast<HDROP>(wParam);
            UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; i++) {
                wchar_t path[MAX_PATH * 2];
                DragQueryFileW(drop, i, path, MAX_PATH * 2);
                std::wstring p = path;
                if (Lower(p).size() > 4 && Lower(p).substr(p.size() - 4) == L".exe") AddGame(p);
                else Toast(FileName(p) + L" isn't a program (.exe).", Level::Warning);
            }
            DragFinish(drop);
            Redraw();
            return 0;
        }
        case kScanned: OnScanned(reinterpret_cast<ScanResult*>(lParam)); return 0;
        case kFirstRun: FirstRun(); return 0;
        case WM_CLOSE: SavePlacement(); break;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

int RunWindow(HINSTANCE instance, int show) {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                             GetSystemMetrics(SM_CYICON), 0));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DX11on10Window";
    RegisterClassExW(&wc);

    g_dpi = float(WindowDpi(nullptr));
    int w = int(1180 * g_dpi / 96), h = int(780 * g_dpi / 96);
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    w = std::min(w, int(work.right - work.left));
    h = std::min(h, int(work.bottom - work.top));
    int x = work.left + (work.right - work.left - w) / 2, y = work.top + (work.bottom - work.top - h) / 2;
    bool maximized = false;
    RECT saved;
    int max = 0;
    if (swscanf(GetSetting(L"app", L"window").c_str(), L"%ld,%ld,%ld,%ld,%d", &saved.left, &saved.top, &saved.right,
                &saved.bottom, &max) == 5 &&
        MonitorFromRect(&saved, MONITOR_DEFAULTTONULL)) {
        x = saved.left, y = saved.top, w = saved.right - saved.left, h = saved.bottom - saved.top;
        maximized = max != 0;
    }
    g_window = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"DX11on10", WS_OVERLAPPEDWINDOW, x, y, w, h,
                               nullptr, nullptr, instance, nullptr);
    if (!g_window) return 1;
    g_dpi = float(WindowDpi(g_window));
    ApplyTheme();
    ShowWindow(g_window, maximized ? SW_SHOWMAXIMIZED : show);
    UpdateWindow(g_window);
    StartScan();
    SetTimer(g_window, kRunningTimer, 2000, nullptr);
    PostMessageW(g_window, kFirstRun, 0, 0);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

// ---- command-line modes for testing ----

void WriteText(const std::wstring& path, const std::wstring& text) {
    std::string utf8 = Narrow(text);
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    fwrite(utf8.data(), 1, utf8.size(), f);
    fclose(f);
}

void ScanNow() {
    g_state.gpu = DetectGpu();
    g_state.gpuKnown = true;
    g_state.games = FindGames();
    RefreshAll();
    SortGames();
    EnsureSelection();
}

int Report(const std::wstring& out) {
    ScanNow();
    std::wstring text = L"gpu: " + g_state.gpu.name + L" (" + LevelName(g_state.gpu.level) + L")" +
                        (g_state.gpu.runtime ? L"" : L" runtime missing") + L"\n";
    const wchar_t* states[] = {L"off", L"on", L"outdated", L"foreign"};
    for (const Game& g : g_state.games) {
        text += L"\n" + g.name + L"  [" + g.id + L"]\n  root: " + g.root + L"\n  exe: " + g.exe +
                (g.exeFromUser ? L" (chosen)" : L"") + L"\n  " + MachineName(g.machine) + L", " + ApiNames(g.apis) +
                L", state " + states[int(g.install)] + (g.running ? L", running" : L"") +
                (g.needsAdmin ? L", needs admin" : L"") + L"\n";
        for (const std::wstring& a : g.antiCheat) text += L"  anti-cheat: " + a + L"\n";
        if (g.report.present)
            text += Format(L"  last run: %hs -> %hs, %.1f fps, %u converted, %u exceptions\n", g.report.realLevel.c_str(),
                           g.report.gameLevel.c_str(), g.report.fps, g.report.converted, g.report.exceptions);
        for (const Notice& n : GameNotices(g, g_state.gpu)) text += L"  notice: " + n.title + L"\n";
    }
    WriteText(out, text);
    return 0;
}

std::wstring Arg(int argc, wchar_t** argv, const wchar_t* name, const wchar_t* fallback = L"") {
    for (int i = 1; i + 1 < argc; i++)
        if (lstrcmpW(argv[i], name) == 0) return argv[i + 1];
    return fallback;
}

bool Flag(int argc, wchar_t** argv, const wchar_t* name) {
    for (int i = 1; i < argc; i++)
        if (lstrcmpW(argv[i], name) == 0) return true;
    return false;
}

int Select(const std::wstring& text) {
    for (int i = 0; i < int(g_state.games.size()); i++)
        if (Lower(g_state.games[i].name).find(Lower(text)) != std::wstring::npos) return i;
    return -1;
}

// Renders the window into a PNG without showing it (design checks over SSH).
int Screenshot(int argc, wchar_t** argv, const std::wstring& out) {
    ScanNow();
    int w = 1180, h = 780;
    swscanf(Arg(argc, argv, L"--size", L"1180x780").c_str(), L"%dx%d", &w, &h);
    float scale = float(_wtof(Arg(argc, argv, L"--scale", L"1.5").c_str()));
    std::wstring theme = Arg(argc, argv, L"--theme", L"light");
    g_view.SetDark(theme == L"dark");
    g_state.showAll = Flag(argc, argv, L"--all");
    std::wstring gpu = Arg(argc, argv, L"--gpu");
    if (gpu == L"10_1") g_state.gpu.level = 0xa100;
    else if (gpu == L"10_0") g_state.gpu.level = 0xa000;
    else if (gpu == L"11_0") g_state.gpu.level = 0xb000;
    else if (gpu == L"9_3") g_state.gpu.level = 0x9300;
    if (!gpu.empty() && g_state.gpu.name.empty()) g_state.gpu.name = L"Test graphics card";
    std::wstring select = Arg(argc, argv, L"--select");
    if (!select.empty() && Select(select) >= 0) g_state.selected = Select(select);
    else EnsureSelection();
    std::wstring toast = Arg(argc, argv, L"--toast");
    if (!toast.empty()) Toast(toast);
    if (Flag(argc, argv, L"--scanning")) g_state.scanning = true;

    UINT pw = UINT(float(w) * scale), ph = UINT(float(h) * scale);
    Com<IWICBitmap> bitmap;
    if (FAILED(g_view.Wic()->CreateBitmap(pw, ph, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, bitmap.Put())))
        return 2;
    D2D1_RENDER_TARGET_PROPERTIES props = {D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                           {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED},
                                           96 * scale, 96 * scale, D2D1_RENDER_TARGET_USAGE_NONE,
                                           D2D1_FEATURE_LEVEL_DEFAULT};
    Com<ID2D1RenderTarget> rt;
    if (FAILED(g_view.Factory()->CreateWicBitmapRenderTarget(bitmap.get(), &props, rt.Put()))) return 3;
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    float scroll = float(_wtof(Arg(argc, argv, L"--scroll", L"0").c_str()));
    for (int pass = 0; pass < 2; pass++) {  // the second pass knows the content heights
        if (pass == 1 && scroll > 0) g_view.Wheel(float(w) - 50, 300, -scroll * 120.f / 64.f);
        g_state.toastAt = GetTickCount() - 1000;
        rt->BeginDraw();
        g_view.Render(rt.get(), g_state, float(w), float(h), 96 * scale);
        rt->EndDraw(nullptr, nullptr);
    }
    Com<IWICStream> stream;
    Com<IWICBitmapEncoder> encoder;
    Com<IWICBitmapFrameEncode> frame;
    if (FAILED(g_view.Wic()->CreateStream(stream.Put())) ||
        FAILED(stream->InitializeFromFilename(out.c_str(), GENERIC_WRITE)) ||
        FAILED(g_view.Wic()->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.Put())) ||
        FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(frame.Put(), nullptr)) || FAILED(frame->Initialize(nullptr)))
        return 4;
    frame->SetSize(pw, ph);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
    frame->SetPixelFormat(&format);
    frame->WriteSource(bitmap.get(), nullptr);
    frame->Commit();
    encoder->Commit();
    return 0;
}

int ToggleCommand(const std::wstring& text, const std::wstring& out) {
    g_headless = true;
    ScanNow();
    int i = Select(text);
    if (i < 0) {
        WriteText(out, L"no game matches " + text + L"\n");
        return 1;
    }
    g_state.selected = i;
    std::wstring before = IsOn(g_state.games[i]) ? L"on" : L"off";
    std::wstring id = g_state.games[i].id;
    Toggle(i);
    for (const Game& g : g_state.games)
        if (g.id == id)
            g_headlessLog += g.name + L": " + before + L" -> " + (IsOn(g) ? L"on" : L"off") + L"\n";
    WriteText(out, g_headlessLog);
    return 0;
}

}  // namespace
}  // namespace app

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    using namespace app;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 3 && lstrcmpW(argv[1], L"--ops") == 0) return RunOpsFile(argv[2]);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    if (!g_view.Init()) {
        MessageBoxW(nullptr, L"DX11on10 needs Direct2D, which this version of Windows doesn't have.", L"DX11on10",
                    MB_ICONERROR);
        return 1;
    }
    if (argc >= 3 && lstrcmpW(argv[1], L"--report") == 0) return Report(argv[2]);
    if (argc >= 3 && lstrcmpW(argv[1], L"--screenshot") == 0) return Screenshot(argc, argv, argv[2]);
    if (argc >= 4 && lstrcmpW(argv[1], L"--toggle") == 0) return ToggleCommand(argv[2], argv[3]);
    if (argc >= 3 && lstrcmpW(argv[1], L"--add") == 0) {
        std::wstring exe = argv[2];
        SetSetting(L"custom", CustomId(exe).c_str(), &exe);
        return 0;
    }
    g_hitsPath = Arg(argc, argv, L"--hits");
    int code = RunWindow(instance, show);
    CoUninitialize();
    return code;
}
