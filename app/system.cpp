// Strings, files, settings and the graphics card check.
#include "app.h"

#include <d3d11.h>
#include <dxgi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdarg>
#include <cwchar>


namespace app {

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), DWORD(s.size()));
    return s;
}

std::wstring Format(const wchar_t* format, ...) {
    wchar_t buf[2048];
    va_list args;
    va_start(args, format);
    vswprintf(buf, 2048, format, args);
    va_end(args);
    return buf;
}

std::wstring DirOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : path.substr(0, slash);
}

std::wstring FileName(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring Stem(const std::wstring& path) {
    std::wstring name = FileName(path);
    size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? name : name.substr(0, dot);
}

std::wstring Join(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    wchar_t last = dir.back();
    return last == L'\\' || last == L'/' ? dir + name : dir + L"\\" + name;
}

bool FileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

uint64_t FileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out, size_t limit) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    GetFileSizeEx(f, &size);
    size_t n = size_t(std::min<uint64_t>(uint64_t(size.QuadPart), limit));
    out.resize(n);
    DWORD got = 0;
    bool ok = n == 0 || (ReadFile(f, out.data(), DWORD(n), &got, nullptr) && got == n);
    CloseHandle(f);
    return ok;
}

std::wstring ErrorText(DWORD error) {
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   error, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring s = text ? text : Format(L"Error %lu.", error);
    LocalFree(text);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
    return s;
}

std::wstring SelfPath() {
    wchar_t path[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    return path;
}

std::wstring VersionString(const std::wstring& path, const wchar_t* name) {
    DWORD ignored = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size) return {};
    std::vector<uint8_t> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return {};
    struct Translation {
        WORD language, codepage;
    }* translations = nullptr;
    UINT bytes = 0;
    std::vector<std::wstring> blocks;
    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &bytes))
        for (UINT i = 0; i < bytes / sizeof(Translation); i++)
            blocks.push_back(Format(L"%04x%04x", translations[i].language, translations[i].codepage));
    blocks.push_back(L"040904b0");
    blocks.push_back(L"040904e4");
    for (const std::wstring& block : blocks) {
        wchar_t* value = nullptr;
        UINT length = 0;
        std::wstring query = L"\\StringFileInfo\\" + block + L"\\" + name;
        if (VerQueryValueW(data.data(), query.c_str(), reinterpret_cast<void**>(&value), &length) && value && length > 1)
            return value;
    }
    return {};
}

// ---- settings ----

std::wstring SettingsDir() {
    wchar_t base[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, base))) return DirOf(SelfPath());
    std::wstring dir = Join(base, L"DX11on10");
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

namespace {

const std::wstring& SettingsFile() {
    static std::wstring path = [] {
        std::wstring p = Join(SettingsDir(), L"settings.ini");
        // Starting the file with a UTF-16 byte order mark makes the profile functions keep
        // non-English paths intact.
        HANDLE f = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            const uint8_t bom[2] = {0xFF, 0xFE};
            DWORD written;
            WriteFile(f, bom, 2, &written, nullptr);
            CloseHandle(f);
        }
        return p;
    }();
    return path;
}

}  // namespace

std::wstring GetSetting(const std::wstring& section, const wchar_t* key, const wchar_t* fallback) {
    std::vector<wchar_t> buf(4096);
    GetPrivateProfileStringW(section.c_str(), key, fallback, buf.data(), DWORD(buf.size()), SettingsFile().c_str());
    return buf.data();
}

void SetSetting(const std::wstring& section, const wchar_t* key, const std::wstring* value) {
    WritePrivateProfileStringW(section.c_str(), key, value ? value->c_str() : nullptr, SettingsFile().c_str());
}

std::vector<std::pair<std::wstring, std::wstring>> SettingsSection(const std::wstring& section) {
    std::vector<wchar_t> buf(65536);
    DWORD n = GetPrivateProfileSectionW(section.c_str(), buf.data(), DWORD(buf.size()), SettingsFile().c_str());
    std::vector<std::pair<std::wstring, std::wstring>> out;
    for (const wchar_t* p = buf.data(); p < buf.data() + n && *p; p += wcslen(p) + 1) {
        std::wstring line = p;
        size_t eq = line.find(L'=');
        if (eq != std::wstring::npos) out.emplace_back(line.substr(0, eq), line.substr(eq + 1));
    }
    return out;
}

// ---- graphics card ----

std::wstring LevelName(unsigned level) {
    switch (level) {
        case D3D_FEATURE_LEVEL_9_1:
        case D3D_FEATURE_LEVEL_9_2:
        case D3D_FEATURE_LEVEL_9_3: return L"DirectX 9";
        case D3D_FEATURE_LEVEL_10_0: return L"DirectX 10";
        case D3D_FEATURE_LEVEL_10_1: return L"DirectX 10.1";
        case D3D_FEATURE_LEVEL_11_0: return L"DirectX 11";
        case D3D_FEATURE_LEVEL_11_1: return L"DirectX 11.1";
        case 0: return L"unknown";
        default: return L"DirectX 12";
    }
}

GpuInfo DetectGpu() {
    GpuInfo info;
    // From System32 by full path: never a d3d11.dll that happens to sit next to this program.
    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    HMODULE d3d11 = LoadLibraryW(Join(system, L"d3d11.dll").c_str());
    HMODULE dxgi = LoadLibraryW(Join(system, L"dxgi.dll").c_str());
    auto create = d3d11 ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(d3d11, "D3D11CreateDevice")) : nullptr;
    using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
    auto factory1 = dxgi ? reinterpret_cast<CreateFactory>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    if (!create || !factory1) {
        info.runtime = false;
        return info;
    }
    IDXGIFactory1* factory = nullptr;
    if (FAILED(factory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return info;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        bool software = (desc.Flags & 2 /* DXGI_ADAPTER_FLAG_SOFTWARE */) || (desc.VendorId == 0x1414 && desc.DeviceId == 0x8c);
        if (!software) {
            if (info.name.empty()) {
                info.name = desc.Description;
                const D3D_FEATURE_LEVEL all[] = {D3D_FEATURE_LEVEL(0xc100), D3D_FEATURE_LEVEL(0xc000),
                                                 D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                                 D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
                                                 D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_2,
                                                 D3D_FEATURE_LEVEL_9_1};
                D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL(0);
                // Older runtimes reject levels they don't know, so retry without the newest ones.
                for (int skip = 0; skip <= 3 && !got; skip++)
                    if (SUCCEEDED(create(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, all + skip, UINT(9 - skip),
                                         D3D11_SDK_VERSION, nullptr, &got, nullptr)))
                        break;
                info.level = got;
            } else {
                info.others++;
            }
        }
        adapter->Release();
    }
    factory->Release();
    return info;
}

}  // namespace app
