// Config file, log file and one-time initialisation.
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <set>

#include "../common/version.h"
#include "proxy.h"

Config g_config;
Stats g_stats;

extern HMODULE g_self;

namespace {

HANDLE g_log = INVALID_HANDLE_VALUE;
SRWLOCK g_logLock = SRWLOCK_INIT;
std::set<std::string>* g_seen;
LARGE_INTEGER g_start, g_freq;
std::wstring g_dir;

std::wstring ModuleDir(HMODULE module) {
    wchar_t path[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(module, path, MAX_PATH * 2);
    std::wstring s(path, n);
    size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : s.substr(0, slash);
}

std::wstring IniString(const wchar_t* ini, const wchar_t* key, const wchar_t* fallback) {
    wchar_t buf[512];
    GetPrivateProfileStringW(L"dx11on10", key, fallback, buf, 512, ini);
    std::wstring s = buf;
    size_t semi = s.find(L';');
    if (semi != std::wstring::npos) s = s.substr(0, semi);
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) s.pop_back();
    return s;
}

void LoadConfig() {
    std::wstring ini = g_dir + L"\\dx11on10.ini";
    std::wstring level = IniString(ini.c_str(), L"MaxFeatureLevel", L"10_1");
    if (level == L"native") g_config.native = true;
    else if (level == L"10_0") g_config.maxLevel = D3D_FEATURE_LEVEL_10_0;
    else g_config.maxLevel = D3D_FEATURE_LEVEL_10_1;
    std::wstring fake = IniString(ini.c_str(), L"FakeFeatureLevel", L"auto");
    g_config.fake = fake == L"always" ? Config::FakeAlways : fake == L"off" ? Config::FakeOff : Config::FakeAuto;
    g_config.translate = IniString(ini.c_str(), L"Translate", L"1") != L"0";
    g_config.warp = IniString(ini.c_str(), L"Driver", L"hardware") == L"warp";
    g_config.debugLayer = IniString(ini.c_str(), L"DebugLayer", L"0") == L"1";
    g_config.dumpShaders = _wtoi(IniString(ini.c_str(), L"DumpShaders", L"0").c_str());
    g_config.exitAfterFrame = wcstoul(IniString(ini.c_str(), L"ExitAfterFrame", L"0").c_str(), nullptr, 10);
    g_config.traceFrame = wcstoul(IniString(ini.c_str(), L"TraceFrame", L"0").c_str(), nullptr, 10);
    g_config.emulate = IniString(ini.c_str(), L"Emulate", L"0") == L"1";
    g_config.keepCompute = IniString(ini.c_str(), L"KeepCompute", L"0") == L"1";
    std::wstring keep = IniString(ini.c_str(), L"KeepComputeIds", L"");
    for (const wchar_t* p = keep.c_str(); *p;) {
        wchar_t* end;
        unsigned long id = wcstoul(p, &end, 10);
        if (end == p) {
            p++;
            continue;
        }
        g_config.keepComputeIds.push_back(id);
        p = end;
    }
    std::wstring shots = IniString(ini.c_str(), L"Screenshots", L"");
    for (const wchar_t* p = shots.c_str(); *p;) {
        wchar_t* end;
        unsigned long frame = wcstoul(p, &end, 10);
        if (end == p) {
            p++;
            continue;
        }
        g_config.screenshots.push_back(frame);
        p = end;
    }
}

void OpenLog() {
    std::wstring path = g_dir + L"\\dx11on10.log";
    g_log = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (g_log == INVALID_HANDLE_VALUE) {
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        g_dir = temp;
        if (!g_dir.empty() && g_dir.back() == L'\\') g_dir.pop_back();
        path = g_dir + L"\\dx11on10.log";
        g_log = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    }
}

BOOL CALLBACK InitOnce(PINIT_ONCE, PVOID, PVOID*) {
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_start);
    g_seen = new std::set<std::string>;
    g_dir = ModuleDir(g_self);
    LoadConfig();
    OpenLog();
    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    Log("dx11on10 %s, built %s (%s)", DX11ON10_VERSION, __DATE__, sizeof(void*) == 8 ? "64-bit" : "32-bit");
    Log("process: %ls", exe);
    InstallCrashLogger();
    Log("config: MaxFeatureLevel=%s FakeFeatureLevel=%s Translate=%d Driver=%s DebugLayer=%d DumpShaders=%d "
        "ExitAfterFrame=%u",
        g_config.native ? "native" : LevelName(g_config.maxLevel).c_str(),
        g_config.fake == Config::FakeAlways ? "always" : g_config.fake == Config::FakeOff ? "off" : "auto",
        g_config.translate, g_config.warp ? "warp" : "hardware", g_config.debugLayer, g_config.dumpShaders,
        g_config.exitAfterFrame);
    return TRUE;
}

}  // namespace

void EnsureInit() {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, InitOnce, nullptr, nullptr);
}

std::wstring OutputPath(const std::wstring& name) { return g_dir + L"\\" + name; }

void Log(const char* fmt, ...) {
    if (g_log == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double t = double(now.QuadPart - g_start.QuadPart) / double(g_freq.QuadPart);
    char buf[2048];
    int n = snprintf(buf, sizeof(buf), "[%9.3f] [%5lu] ", t, GetCurrentThreadId());
    va_list args;
    va_start(args, fmt);
    int m = vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, args);
    va_end(args);
    n += m < 0 ? 0 : (m < int(sizeof(buf)) - n - 2 ? m : int(sizeof(buf)) - n - 3);
    buf[n++] = '\r';
    buf[n++] = '\n';
    AcquireSRWLockExclusive(&g_logLock);
    DWORD written;
    WriteFile(g_log, buf, n, &written, nullptr);
    ReleaseSRWLockExclusive(&g_logLock);
}

bool FirstTime(const std::string& key) {
    AcquireSRWLockExclusive(&g_logLock);
    bool inserted = g_seen && g_seen->insert(key).second;
    ReleaseSRWLockExclusive(&g_logLock);
    return inserted;
}

void LogFlush() {
    if (g_log != INVALID_HANDLE_VALUE) FlushFileBuffers(g_log);
}

std::string LevelName(D3D_FEATURE_LEVEL level) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d_%d", level >> 12, (level >> 8) & 0xf);
    return buf;
}
