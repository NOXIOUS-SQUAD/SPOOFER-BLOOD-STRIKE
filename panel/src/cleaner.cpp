#include "cleaner.h"

#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <vector>
#include <string>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace cleaner {
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
    return out;
}

std::string env_var(const char* name) {
    DWORD n = GetEnvironmentVariableA(name, nullptr, 0);
    if (n <= 1) return "";
    std::string v(n - 1, 0);
    GetEnvironmentVariableA(name, &v[0], n);
    return v;
}

bool path_exists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void measure(const std::wstring& dir, uint64_t& files, uint64_t& bytes) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> subdirs;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::wstring full = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            subdirs.push_back(full);
        } else {
            LARGE_INTEGER sz{};
            sz.LowPart = fd.nFileSizeLow;
            sz.HighPart = (LONG)fd.nFileSizeHigh;
            files++;
            bytes += (uint64_t)sz.QuadPart;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (auto& sd : subdirs)
        measure(sd, files, bytes);
}

bool zero_fill(const std::wstring& path, uint64_t& out_size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0) { CloseHandle(h); out_size = 0; return true; }
    out_size = (uint64_t)size.QuadPart;
    const DWORD CHUNK = 1 << 20;
    std::vector<BYTE> zeros(CHUNK, 0);
    LARGE_INTEGER pos{};
    bool ok = true;
    while (pos.QuadPart < size.QuadPart) {
        ULONGLONG remaining = (ULONGLONG)(size.QuadPart - pos.QuadPart);
        DWORD to_write = (DWORD)(remaining < CHUNK ? remaining : CHUNK);
        DWORD written = 0;
        if (!WriteFile(h, zeros.data(), to_write, &written, nullptr) || written != to_write) { ok = false; break; }
        pos.QuadPart += written;
    }
    SetFilePointerEx(h, size, nullptr, FILE_BEGIN);
    SetEndOfFile(h);
    FlushFileBuffers(h);
    CloseHandle(h);
    return ok;
}

void wipe_tree(const std::wstring& dir, WipeReport& rep) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> subdirs;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::wstring full = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            subdirs.push_back(full);
        } else {
            uint64_t sz = 0;
            bool filled = zero_fill(full, sz);
            if (filled && DeleteFileW(full.c_str())) { rep.files_wiped++; rep.bytes_freed += sz; }
            else rep.files_failed++;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (auto& sd : subdirs)
        wipe_tree(sd, rep);
    RemoveDirectoryW(dir.c_str());
}

}

static CleanTarget make_target(const std::string& label, const std::string& path, bool needs_admin) {
    CleanTarget t;
    t.label = label;
    t.path = path;
    t.needs_admin = needs_admin;
    std::wstring w = widen(path);
    if (path_exists(w)) {
        t.exists = true;
        DWORD attr = GetFileAttributesW(w.c_str());
        if (attr & FILE_ATTRIBUTE_DIRECTORY)
            measure(w, t.files, t.bytes);
        else {
            LARGE_INTEGER sz{};
            HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                if (GetFileSizeEx(h, &sz)) { t.bytes = (uint64_t)sz.QuadPart; t.files = 1; }
                CloseHandle(h);
            }
        }
    }
    return t;
}

std::vector<CleanTarget> scan_temp() {
    std::vector<CleanTarget> out;
    out.push_back(make_target("Temporales de usuario (%TEMP%)", env_var("TEMP"), false));
    out.push_back(make_target("Temporales del sistema (%WINDIR%\\Temp)", env_var("SystemRoot") + "\\Temp", true));
    out.push_back(make_target("INetCache (explorador)", env_var("LOCALAPPDATA") + "\\Microsoft\\Windows\\INetCache", false));
    out.push_back(make_target("CrashDumps", env_var("LOCALAPPDATA") + "\\CrashDumps", false));
    out.push_back(make_target("Miniaturas del explorador", env_var("LOCALAPPDATA") + "\\Microsoft\\Windows\\Explorer", false));
    out.push_back(make_target("Windows Error Reporting", env_var("LOCALAPPDATA") + "\\Microsoft\\Windows\\WER", false));
    return out;
}

std::vector<CleanTarget> scan_own_traces() {
    std::vector<CleanTarget> out;
    std::string exe = current_exe_name();
    std::string base = exe;
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);

    out.push_back(make_target("Prefetch propio", env_var("SystemRoot") + "\\Prefetch\\" + base + "-*.pf", false));
    out.push_back(make_target("Recent propio", env_var("APPDATA") + "\\Microsoft\\Windows\\Recent\\" + base + ".lnk", false));
    out.push_back(make_target("JumpLists propias", env_var("APPDATA") + "\\Microsoft\\Windows\\Recent\\AutomaticDestinations", false));
    out.push_back(make_target("MuiCache propio",
        "HKCU\\Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\Shell\\MuiCache", false));
    return out;
}

std::vector<CleanTarget> scan_game_traces() {
    std::vector<CleanTarget> out;
    for (auto& p : spoofer::cache_paths())
        out.push_back(make_target("Cache NetEase/BloodStrike", p, false));

    std::string roaming = env_var("APPDATA");
    out.push_back(make_target("Mpay (SQLite + logs)", roaming + "\\Netease\\Mpay", false));
    return out;
}

WipeReport wipe(const std::vector<CleanTarget>& targets, spoofer::LogFn log) {
    WipeReport rep;
    for (auto& t : targets) {
        if (!t.exists) continue;
        std::wstring w = widen(t.path);
        if (log) {
            char msg[300];
            snprintf(msg, sizeof(msg), "Borrando (zero-fill): %s", t.path.c_str());
            log(0, msg);
        }
        DWORD attr = GetFileAttributesW(w.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {

            uint64_t sz = 0;
            if (zero_fill(w, sz) && DeleteFileW(w.c_str())) { rep.files_wiped++; rep.bytes_freed += sz; }
            else rep.files_failed++;
            continue;
        }
        wipe_tree(w, rep);
    }
    if (log) log(1, "Limpieza completada.");
    return rep;
}

std::string current_exe_name() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring w(buf);
    size_t slash = w.find_last_of(L"\\/");
    if (slash != std::wstring::npos) w = w.substr(slash + 1);
    return narrow(w);
}

void wipe_self_traces(spoofer::LogFn log) {
    std::string exe = current_exe_name();
    std::string base = exe;
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    std::string lower_base = base;
    for (char& c : lower_base) c = (char)tolower((unsigned char)c);

    std::string pf_dir = env_var("SystemRoot") + "\\Prefetch\\";
    std::wstring pattern = widen(pf_dir + base + "-*.pf");
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            uint64_t sz = 0;
            std::wstring f = widen(pf_dir) + fd.cFileName;
            if (zero_fill(f, sz) && DeleteFileW(f.c_str()))
                if (log) log(1, "Rastro propio borrado: Prefetch\\" + narrow(fd.cFileName));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    std::string lnk = env_var("APPDATA") + "\\Microsoft\\Windows\\Recent\\" + base + ".lnk";
    uint64_t dummy = 0;
    if (path_exists(widen(lnk))) {
        if (zero_fill(widen(lnk), dummy) && DeleteFileW(widen(lnk).c_str()))
            if (log) log(1, "Rastro propio borrado: Recent\\" + base + ".lnk");
    }

    static const wchar_t* MUI = L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\Shell\\MuiCache";
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, MUI, 0, KEY_ALL_ACCESS, &k) == ERROR_SUCCESS) {
        std::vector<std::wstring> hits;
        wchar_t name[1024]; DWORD nlen;
        for (DWORD i = 0; ; i++) {
            nlen = 1024;
            LONG r = RegEnumValueW(k, i, name, &nlen, nullptr, nullptr, nullptr, nullptr);
            if (r == ERROR_NO_MORE_ITEMS) break;
            if (r != ERROR_SUCCESS) continue;
            std::string s = narrow(name);
            std::string low; for (char c : s) low += (char)tolower((unsigned char)c);
            if (low.find(lower_base) != std::string::npos)
                hits.push_back(name);
        }
        for (auto& hh : hits) RegDeleteValueW(k, hh.c_str());
        if (!hits.empty()) {
            char msg[96]; snprintf(msg, sizeof(msg), "MuiCache: %d entradas propias eliminadas.", (int)hits.size());
            if (log) log(1, msg);
        }
        RegCloseKey(k);
    }

    if (log) log(0, "Rastros propios revisados (prefetch, Recent, MuiCache).");
}

}
