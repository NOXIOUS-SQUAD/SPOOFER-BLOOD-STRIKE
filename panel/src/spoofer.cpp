#include "spoofer.h"
#include "tinyjson.h"

#include <windows.h>
#include <winioctl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <cstdio>
#include <fstream>
#include <sstream>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace spoofer {
namespace {

std::string widen_to_narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
    return out;
}
std::wstring narrow_to_widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
}

std::string env_var(const char* name) {
    DWORD n = GetEnvironmentVariableA(name, nullptr, 0);
    if (n <= 1) return "";
    std::string v(n - 1, 0);
    GetEnvironmentVariableA(name, &v[0], n);
    return v;
}

bool path_is_dir(const std::string& p) {
    DWORD a = GetFileAttributesW(narrow_to_widen(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool path_is_file(const std::string& p) {
    DWORD a = GetFileAttributesW(narrow_to_widen(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string lower_copy(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string exe_in_root(const std::string& root) {
    static const char* REL[] = {
        "\\Engine\\Binaries\\Win64\\BloodStrike.exe",
        "\\Binaries\\Win64\\BloodStrike.exe",
        "\\BloodStrike.exe",
        "\\launcher.exe",
    };
    for (const char* r : REL) {
        std::string p = root + r;
        if (path_is_file(p)) return p;
    }
    return "";
}

bool skip_scan_dir(const std::string& name) {
    static const char* BAD[] = {
        "windows", "$recycle.bin", "system volume information", "winsxs", "node_modules",
        ".git", "appdata", "$windows.~bt", "$windows.~ws", "recovery", "msocache", "perflogs",
        "config.msi", "onedrivetemp", "packages", "driverstore",
    };
    std::string n = lower_copy(name);
    for (const char* b : BAD) if (n == b) return true;
    return false;
}

std::string scan_for_game(const std::string& root, int depth, int& budget) {
    if (depth <= 0 || budget <= 0) return "";
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((root + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return "";
    std::string found;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        if (skip_scan_dir(name)) continue;
        if (--budget <= 0) break;
        std::string sub = root + "\\" + name;
        if (lower_copy(name).find("bloodstrike") != std::string::npos) {
            found = exe_in_root(sub);
            if (!found.empty()) break;
        }
        found = scan_for_game(sub, depth - 1, budget);
        if (!found.empty()) break;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

std::string lvl_tag(int lvl) {
    switch (lvl) {
        case 1: return "[OK]   ";
        case 2: return "[WARN] ";
        case 3: return "[ERROR]";
        default: return "[..]   ";
    }
}

bool reg_write_str(HKEY root, const std::wstring& sub, const std::wstring& name, const std::string& value) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(root, sub.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    std::wstring w = narrow_to_widen(value);
    LONG r = RegSetValueExW(k, name.c_str(), 0, REG_SZ, (const BYTE*)w.c_str(), (DWORD)((w.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

bool reg_write_dword(HKEY root, const std::wstring& sub, const std::wstring& name, DWORD value) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(root, sub.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    LONG r = RegSetValueExW(k, name.c_str(), 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

bool reg_delete_tree(HKEY root, const std::wstring& sub) {
    LONG r = RegDeleteTreeW(root, sub.c_str());

    return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
}

int run_hidden(const std::wstring& cmdline) {
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = cmdline;
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

bool zero_fill_file(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0) { CloseHandle(h); return true; }
    const DWORD CHUNK = 1 << 20;
    std::vector<BYTE> zeros(CHUNK, 0);
    LARGE_INTEGER pos{};
    while (pos.QuadPart < size.QuadPart) {
        ULONGLONG remaining = (ULONGLONG)(size.QuadPart - pos.QuadPart);
        DWORD to_write = (DWORD)(remaining < (ULONGLONG)CHUNK ? remaining : (ULONGLONG)CHUNK);
        DWORD written = 0;
        if (!WriteFile(h, zeros.data(), to_write, &written, nullptr) || written != to_write) {
            CloseHandle(h);
            return false;
        }
        pos.QuadPart += written;
    }
    SetFilePointerEx(h, size, nullptr, FILE_BEGIN);
    SetEndOfFile(h);
    FlushFileBuffers(h);
    CloseHandle(h);
    return true;
}

void wipe_tree(const std::wstring& dir, LogFn log, int& files_wiped, int& files_failed) {
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
            zero_fill_file(full);
            if (DeleteFileW(full.c_str())) files_wiped++;
            else files_failed++;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (auto& sd : subdirs)
        wipe_tree(sd, log, files_wiped, files_failed);
    RemoveDirectoryW(dir.c_str());
    (void)log;
}

}

bool service_ne_protect_disable(LogFn log) {
    static const wchar_t* KEY = L"SYSTEM\\CurrentControlSet\\Services\\NEProtect";
    HKEY k = nullptr;
    LONG r = RegOpenKeyExW(HKEY_LOCAL_MACHINE, KEY, 0, KEY_READ | KEY_WOW64_64KEY, &k);
    if (r == ERROR_FILE_NOT_FOUND) {
        if (log) log(2, "Servicio NEProtect no está instalado — nada que bloquear.");
        return true;
    }
    if (r != ERROR_SUCCESS) {
        if (log) log(3, "No se pudo abrir HKLM\\...\\Services\\NEProtect (¿sin admin?).");
        return false;
    }
    DWORD start = 0, sz = sizeof(start), type = 0;
    RegQueryValueExW(k, L"Start", nullptr, &type, (LPBYTE)&start, &sz);
    RegCloseKey(k);
    if (!reg_write_dword(HKEY_LOCAL_MACHINE, KEY, L"Start", 4)) {
        if (log) log(3, "Fallo al escribir Start=4 en NEProtect (requiere admin).");
        return false;
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "NEProtect.sys deshabilitado (Start: %lu -> 4).", (unsigned long)start);
    if (log) log(1, msg);
    return true;
}

bool service_ne_protect_restore(LogFn log) {
    static const wchar_t* KEY = L"SYSTEM\\CurrentControlSet\\Services\\NEProtect";
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, KEY, 0, KEY_READ | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS) {
        RegCloseKey(k);
        if (!reg_write_dword(HKEY_LOCAL_MACHINE, KEY, L"Start", 2)) {
            if (log) log(3, "Fallo al restaurar NEProtect (requiere admin).");
            return false;
        }
        if (log) log(1, "NEProtect restaurado a Start=2 (bajo demanda).");
    }
    return true;
}

bool layer_registry_apply(const identity::Profile& p, LogFn log) {
    bool ok = true;

    if (!p.machine_guid.empty()) {
        if (reg_write_str(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography",
                          L"MachineGuid", p.machine_guid)) {
            if (log) log(1, "MachineGuid rotado -> " + p.machine_guid);
        } else { if (log) log(3, "Fallo MachineGuid (requiere admin)."); ok = false; }
    }

    if (!p.install_date.empty()) {
        DWORD epoch = (DWORD)strtoul(p.install_date.c_str(), nullptr, 10);
        if (reg_write_dword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                            L"InstallDate", epoch)) {
            if (log) log(1, "InstallDate rotado -> epoch " + p.install_date);
        } else { if (log) log(3, "Fallo InstallDate (requiere admin)."); ok = false; }
    }

    if (!p.product_id.empty()) {
        if (reg_write_str(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                          L"ProductId", p.product_id)) {
            if (log) log(1, "ProductId rotado -> " + p.product_id);
        } else { if (log) log(3, "Fallo ProductId (requiere admin)."); ok = false; }
    }

    if (!p.hw_profile_guid.empty()) {
        const wchar_t* sub = L"SYSTEM\\CurrentControlSet\\Control\\IDConfigDB\\Hardware Profiles\\0001";
        if (reg_write_str(HKEY_LOCAL_MACHINE, sub, L"HwProfileGuid", p.hw_profile_guid)) {
            if (log) log(1, "HwProfileGuid rotado -> " + p.hw_profile_guid);
        } else {
            if (log) log(2, "HwProfileGuid: no se pudo escribir (puede no existir ese perfil).");
        }
    }
    return ok;
}

bool layer_volume_apply(const std::string& serial_hex, LogFn log) {
    if (serial_hex.size() < 8) {
        if (log) log(3, "Volume serial inválido (se esperan 8 hex).");
        return false;
    }

    BYTE new_serial[8]{};
    for (int i = 0; i < 8; i++) {
        char c = serial_hex[i];
        int v = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 0;
        new_serial[i] = (BYTE)v;
    }

    HANDLE h = CreateFileW(L"\\\\.\\C:", GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (log) log(3, "No se pudo abrir \\\\.\\C: (requiere admin y tener C: desocupado).");
        return false;
    }
    BYTE sector[512]{};
    DWORD read = 0;
    bool ok = false;
    if (ReadFile(h, sector, sizeof(sector), &read, nullptr) && read == sizeof(sector)) {
        bool is_ntfs = memcmp(sector + 3, "NTFS    ", 8) == 0;
        bool is_fat  = memcmp(sector + 0x36, "FAT", 3) == 0 || memcmp(sector + 0x52, "FAT", 3) == 0;
        DWORD offset = 0; DWORD len = 0;
        if (is_ntfs)      { offset = 0x48; len = 8; }
        else if (is_fat)  { offset = 0x43; len = 4; }
        if (len) {
            memcpy(sector + offset, new_serial, len);
            if (SetFilePointer(h, 0, nullptr, FILE_BEGIN) == 0) {
                DWORD written = 0;
                ok = WriteFile(h, sector, sizeof(sector), &written, nullptr) && written == sizeof(sector);
            }
        } else {
            if (log) log(3, "C: no parece NTFS ni FAT — abortado por seguridad.");
        }
    }
    CloseHandle(h);
    if (ok && log) log(1, std::string("Volume serial C: rotado -> ") + serial_hex);
    return ok;
}

bool layer_mac_apply(const identity::Profile& p, LogFn log) {
    if (p.network_address.empty() || p.adapter_index.empty()) {
        if (log) log(2, "MAC: perfil sin adaptador/mac objetivo.");
        return true;
    }

    std::string mac_clean;
    for (char c : p.network_address)
        if (c != ':' && c != '-') mac_clean += (char)toupper((unsigned char)c);
    if (mac_clean.size() != 12) {
        if (log) log(3, "MAC inválida en el perfil: " + p.network_address);
        return false;
    }

    auto adapters = identity::read_adapters();
    std::string class_path;
    std::string friendly;
    uint32_t want = (uint32_t)strtoul(p.adapter_index.c_str(), nullptr, 10);
    for (auto& a : adapters) {
        if (a.index == want) { class_path = a.registry_path; friendly = a.friendly_name; break; }
    }
    if (class_path.empty()) {

        for (auto& a : adapters)
            if (a.is_primary) { class_path = a.registry_path; friendly = a.friendly_name; break; }
    }
    if (class_path.empty()) {
        if (log) log(3, "MAC: no se encontró la ruta Control\\Class del adaptador " + p.network_address);
        return false;
    }

    std::wstring sub = narrow_to_widen(class_path);
    if (!reg_write_str(HKEY_LOCAL_MACHINE, sub, L"NetworkAddress", mac_clean)) {
        if (log) log(3, "Fallo al escribir NetworkAddress (requiere admin).");
        return false;
    }
    if (log) log(1, "NetworkAddress -> " + mac_clean + " (" + friendly + ")");

    if (!friendly.empty()) {
        std::wstring base = L"netsh interface set interface name=\"" + narrow_to_widen(friendly) + L"\" admin=";
        int r1 = run_hidden(base + L"disabled");
        int r2 = run_hidden(base + L"enabled");
        if (r1 == 0 && r2 == 0) {
            if (log) log(1, "Adaptador reiniciado — MAC activa.");
        } else {
            if (log) log(2, "Adaptador no se pudo reiniciar por netsh; la MAC aplica al reiniciar Windows.");
        }
    }
    return true;
}

std::vector<std::string> cache_paths() {
    std::vector<std::string> paths;
    std::string local = env_var("LOCALAPPDATA");
    std::string roaming = env_var("APPDATA");
    std::string user = env_var("USERPROFILE");
    std::string temp = env_var("TEMP");
    if (!local.empty()) {
        paths.push_back(local + "\\NetEase");
        paths.push_back(local + "\\BloodStrike");
    }
    if (!roaming.empty()) paths.push_back(roaming + "\\Netease");
    if (!user.empty())    paths.push_back(user + "\\AppData\\LocalLow\\NetEase");
    if (!temp.empty())    paths.push_back(temp + "\\NetEase");
    return paths;
}

bool layer_purge_caches(LogFn log) {
    int wiped = 0, failed = 0;
    auto paths = cache_paths();
    for (auto& p : paths) {
        std::wstring w = narrow_to_widen(p);
        DWORD attr = GetFileAttributesW(w.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) continue;
        if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            zero_fill_file(w);
            if (DeleteFileW(w.c_str())) wiped++; else failed++;
            continue;
        }
        char msg[256];
        snprintf(msg, sizeof(msg), "Purgando cache (zero-fill + delete): %s", p.c_str());
        if (log) log(0, msg);
        wipe_tree(w, log, wiped, failed);
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "Caches purgados: %d archivos borrados.", wiped);
    if (log) log(1, msg);
    if (failed > 0) {
        snprintf(msg, sizeof(msg), "%d archivos no se pudieron borrar (¿juego en ejecución?).", failed);
        if (log) log(2, msg);
    }
    return true;
}

bool sinkhole_add(LogFn log) {
    static const char* DOMAINS[] = {
        "applog.matrix.easebar.com",
        "mgbsdknaeast.matrix.easebar.com",
        "drpf-g83naxx1ena.proxima.nie.easebar.com",
        "nie.easebar.com",
        "gph.easebar.com",
    };
    std::wstring hosts = L"C:\\Windows\\System32\\drivers\\etc\\hosts";
    DWORD attr = GetFileAttributesW(hosts.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        if (log) log(3, "No se pudo abrir el archivo hosts.");
        return false;
    }
    SetFileAttributesW(hosts.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);

    std::ifstream in(hosts, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    if (content.find("# >>> BS-SPOOFER TELEMETRY") != std::string::npos) {
        if (log) log(2, "El sinkhole ya estaba aplicado.");
        SetFileAttributesW(hosts.c_str(), attr);
        return true;
    }
    std::string block = "\r\n# >>> BS-SPOOFER TELEMETRY (spoofer BS) >>>\r\n";
    for (auto* d : DOMAINS) {
        block += "0.0.0.0 ";
        block += d;
        block += "\r\n";
    }
    block += "# <<< BS-SPOOFER TELEMETRY <<<\r\n";
    content += block;

    std::ofstream out(hosts, std::ios::binary | std::ios::trunc);
    if (!out) {
        SetFileAttributesW(hosts.c_str(), attr);
        if (log) log(3, "No se pudo escribir hosts (requiere admin).");
        return false;
    }
    out.write(content.data(), (std::streamsize)content.size());
    out.close();
    SetFileAttributesW(hosts.c_str(), attr);
    if (log) log(1, "Sinkhole de telemetría aplicado (5 dominios easebar -> 0.0.0.0).");
    return true;
}

bool sinkhole_remove(LogFn log) {
    std::wstring hosts = L"C:\\Windows\\System32\\drivers\\etc\\hosts";
    DWORD attr = GetFileAttributesW(hosts.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return false;
    SetFileAttributesW(hosts.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);

    std::ifstream in(hosts, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    const std::string a = "# >>> BS-SPOOFER TELEMETRY";
    const std::string b = "# <<< BS-SPOOFER TELEMETRY <<<";
    size_t pa = content.find(a), pb = content.find(b);
    if (pa == std::string::npos || pb == std::string::npos) {
        SetFileAttributesW(hosts.c_str(), attr);
        return true;
    }
    size_t end = pb + b.size();

    while (end < content.size() && (content[end] == '\r' || content[end] == '\n')) end++;
    content.erase(pa, end - pa);

    std::ofstream out(hosts, std::ios::binary | std::ios::trunc);
    if (!out) { SetFileAttributesW(hosts.c_str(), attr); return false; }
    out.write(content.data(), (std::streamsize)content.size());
    out.close();
    SetFileAttributesW(hosts.c_str(), attr);
    if (log) log(1, "Sinkhole de telemetría eliminado (hosts restaurado).");
    return true;
}

bool clean_registry_traces(LogFn log) {
    bool ok = true;

    if (reg_delete_tree(HKEY_CURRENT_USER, L"Software\\Netease"))
        if (log) log(1, "Registro: HKCU\\Software\\Netease eliminado.");
    else if (log) log(2, "Registro: HKCU\\Software\\Netease no se pudo eliminar.");

    if (reg_delete_tree(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Tracing\\BloodStrike_RASAPI32"))
        if (log) log(1, "Registro: Tracing BloodStrike_RASAPI32 eliminado.");
    if (reg_delete_tree(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Tracing\\BloodStrike_RASMANCS"))
        if (log) log(1, "Registro: Tracing BloodStrike_RASMANCS eliminado.");

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
            std::string s = widen_to_narrow(name);
            std::string low; for (char c : s) low += (char)tolower((unsigned char)c);
            if (low.find("bloodstrike") != std::string::npos ||
                low.find("netease") != std::string::npos)
                hits.push_back(name);
        }
        for (auto& h : hits)
            RegDeleteValueW(k, h.c_str());
        char msg[128];
        snprintf(msg, sizeof(msg), "MuiCache: %d entradas NetEase/BloodStrike eliminadas.", (int)hits.size());
        if (log) log(1, msg);
        RegCloseKey(k);
    }
    return ok;
}

std::vector<PreflightItem> preflight(const Options& opt, const identity::Profile& profile) {
    std::vector<PreflightItem> items;
    PreflightItem admin;
    admin.label = "Estado de privilegios";
    admin.current = identity::is_elevated() ? "Administrador" : "Usuario estándar";
    admin.will_be = "";
    admin.note = identity::is_elevated() ? "" : "Sin admin: volumen/servicio/hosts fallarán";
    items.push_back(admin);

    if (opt.layer_registry) {
        auto add = [&](const char* label, const std::string& cur, const std::string& nw) {
            PreflightItem i;
            i.label = label;
            i.current = cur.empty() ? "(vacío)" : cur;
            i.will_be = nw.empty() ? "(sin cambio)" : nw;
            i.note = "requiere admin";
            items.push_back(i);
        };
        add("MachineGuid", identity::read_machine_guid(), profile.machine_guid);
        add("InstallDate", identity::read_install_date(), profile.install_date);
        add("ProductId", identity::read_product_id(), profile.product_id);
        add("HwProfileGuid", identity::read_hw_profile_guid(), profile.hw_profile_guid);
    }
    if (opt.layer_mac) {
        auto adapters = identity::read_adapters();
        std::string cur;
        for (auto& a : adapters) if (a.is_primary) cur = a.mac;
        PreflightItem i;
        i.label = "MAC (adaptador primario)";
        i.current = cur.empty() ? "(sin adaptador)" : cur;
        i.will_be = profile.network_address;
        i.note = "reinicia red / aplica tras reinicio";
        items.push_back(i);
    }
    if (opt.layer_volume) {
        PreflightItem i;
        i.label = "Volume serial C:";
        i.current = identity::read_volume_serial('C');
        i.will_be = profile.volume_serial;
        i.note = "escritura raw al boot sector";
        items.push_back(i);
    }
    if (opt.layer_driver_block) {
        static const wchar_t* KEY = L"SYSTEM\\CurrentControlSet\\Services\\NEProtect";
        HKEY k = nullptr;
        bool exists = RegOpenKeyExW(HKEY_LOCAL_MACHINE, KEY, 0, KEY_READ | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS;
        if (k) RegCloseKey(k);
        PreflightItem i;
        i.label = "NEProtect.sys";
        i.current = exists ? "instalado" : "no instalado";
        i.will_be = "Start=4 (disabled)";
        i.note = "el juego se lanzará además con __COMPAT_LAYER=RUNASINVOKER";
        items.push_back(i);
    }
    if (opt.layer_purge_cache) {
        int count = 0;
        for (auto& p : cache_paths())
            if (GetFileAttributesW(narrow_to_widen(p).c_str()) != INVALID_FILE_ATTRIBUTES) count++;
        PreflightItem i;
        i.label = "Caches NetEase";
        char c[64]; snprintf(c, sizeof(c), "%d rutas presentes", count);
        i.current = c;
        i.will_be = "zero-fill + delete";
        i.note = "cierra el juego antes";
        items.push_back(i);
    }
    if (opt.sinkhole_telemetry) {
        PreflightItem i;
        i.label = "Telemetría easebar";
        i.current = "5 dominios";
        i.will_be = "0.0.0.0 en hosts";
        i.note = "requiere admin";
        items.push_back(i);
    }
    if (opt.clean_traces) {
        PreflightItem i;
        i.label = "Rastros registro";
        i.current = "HKCU\\Software\\Netease, Tracing, MuiCache";
        i.will_be = "eliminados";
        items.push_back(i);
    }
    if (opt.make_backup) {
        PreflightItem i;
        i.label = "Backup";
        i.current = "perfil con valores originales";
        i.will_be = "se guarda en profiles\\";
        items.push_back(i);
    }
    return items;
}

bool apply(const Options& opt, const identity::Profile& profile, const std::string& profile_dir, LogFn log) {
    bool ok = true;

    if (opt.make_backup) {
        identity::Profile backup = identity::capture_current();
        if (!backup.empty()) {
            std::string fname = "backup_" + identity::utc_now();
            for (char& c : fname) if (c == ' ' || c == ':') c = '-';
            fname += ".json";
            std::string path = profile_dir;
            if (!path.empty() && path.back() != '\\') path += '\\';
            path += fname;
            if (identity::save_profile(backup, path))
                if (log) log(0, "Backup guardado: " + fname);
            else if (log) log(2, "No se pudo guardar el backup.");
        }
    }

    if (log) log(0, "== L1: bloqueo de driver NEProtect ==");
    if (opt.layer_driver_block)
        ok = service_ne_protect_disable(log) && ok;

    if (log) log(0, "== L3: rotación de registro ==");
    if (opt.layer_registry)
        ok = layer_registry_apply(profile, log) && ok;

    if (log) log(0, "== L4: volume serial ==");
    if (opt.layer_volume)
        ok = layer_volume_apply(profile.volume_serial, log) && ok;

    if (log) log(0, "== L5: MAC address ==");
    if (opt.layer_mac)
        ok = layer_mac_apply(profile, log) && ok;

    if (log) log(0, "== L2: purga de caches ==");
    if (opt.layer_purge_cache)
        layer_purge_caches(log);

    if (log) log(0, "== Extras: rastros y telemetría ==");
    if (opt.clean_traces)
        clean_registry_traces(log);
    if (opt.sinkhole_telemetry)
        sinkhole_add(log);

    if (log) log(1, ok ? "Proceso completado sin errores bloqueantes."
                       : "Proceso completado CON errores — revisa el log.");
    return ok;
}

bool restore(const identity::Profile& b, LogFn log) {
    bool ok = true;
    identity::Profile p = b;
    if (p.orig_machine_guid.empty())   p.orig_machine_guid   = p.machine_guid;
    if (p.orig_install_date.empty())   p.orig_install_date   = p.install_date;
    if (p.orig_product_id.empty())     p.orig_product_id     = p.product_id;
    if (p.orig_hw_profile_guid.empty())p.orig_hw_profile_guid = p.hw_profile_guid;
    if (p.orig_network_address.empty())p.orig_network_address = p.network_address;

    if (log) log(0, "Restaurando identidad original...");

    if (!p.orig_machine_guid.empty())
        ok = reg_write_str(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid", p.orig_machine_guid) && ok;
    if (!p.orig_install_date.empty())
        ok = reg_write_dword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"InstallDate",
                             (DWORD)strtoul(p.orig_install_date.c_str(), nullptr, 10)) && ok;
    if (!p.orig_product_id.empty())
        ok = reg_write_str(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"ProductId", p.orig_product_id) && ok;
    if (!p.orig_hw_profile_guid.empty()) {
        const wchar_t* sub = L"SYSTEM\\CurrentControlSet\\Control\\IDConfigDB\\Hardware Profiles\\0001";
        ok = reg_write_str(HKEY_LOCAL_MACHINE, sub, L"HwProfileGuid", p.orig_hw_profile_guid) && ok;
    }

    if (!p.adapter_index.empty()) {
        auto adapters = identity::read_adapters();
        uint32_t want = (uint32_t)strtoul(p.adapter_index.c_str(), nullptr, 10);
        for (auto& a : adapters) {
            if (a.index == want && !a.registry_path.empty()) {
                std::string mac_clean;
                for (char c : p.orig_network_address)
                    if (c != ':' && c != '-') mac_clean += (char)toupper((unsigned char)c);
                if (mac_clean.empty()) {

                    HKEY k = nullptr;
                    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, narrow_to_widen(a.registry_path).c_str(), 0,
                                      KEY_ALL_ACCESS | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS) {
                        RegDeleteValueW(k, L"NetworkAddress");
                        RegCloseKey(k);
                    }
                } else {
                    reg_write_str(HKEY_LOCAL_MACHINE, narrow_to_widen(a.registry_path), L"NetworkAddress", mac_clean);
                }
                if (log) log(1, "MAC restaurada -> " + p.orig_network_address);
                break;
            }
        }
    }

    if (!p.volume_serial.empty())
        layer_volume_apply(p.volume_serial, nullptr);

    service_ne_protect_restore(log);
    sinkhole_remove(log);

    if (log) log(1, "Identidad restaurada.");
    return ok;
}

bool game_running() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"BloodStrike.exe") == 0) { found = true; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

std::string find_game_exe() {
    std::vector<std::string> scan_roots;

    const char* envs[] = { "ProgramFiles", "ProgramFiles(x86)", "ProgramData", "LOCALAPPDATA" };
    static const char* SUBDIRS[] = {
        "\\BloodStrike", "\\NetEase\\BloodStrike", "\\NetEase\\BloodStrike\\BloodStrike",
    };
    for (const char* e : envs) {
        std::string base = env_var(e);
        if (base.empty()) continue;
        for (const char* s : SUBDIRS) {
            std::string p = base + s;
            if (!path_is_dir(p)) continue;
            std::string x = exe_in_root(p);
            if (!x.empty()) return x;
        }
    }

    DWORD drives = GetLogicalDrives();
    for (int i = 2; i < 26; i++) {
        if (!(drives & (1u << i))) continue;
        std::string root;
        root += (char)('A' + i);
        root += ":";
        if (GetDriveTypeA((root + "\\").c_str()) != DRIVE_FIXED) continue;

        static const char* DRIVE_SUBS[] = {
            "\\bloodstrike", "\\BloodStrike", "\\Games\\BloodStrike",
            "\\SteamLibrary\\steamapps\\common\\BLOODSTRIKE",
            "\\Program Files\\BloodStrike",
            "\\Program Files (x86)\\BloodStrike",
            "\\NetEase\\BloodStrike",
        };
        for (const char* s : DRIVE_SUBS) {
            std::string p = root + s;
            if (!path_is_dir(p)) continue;
            std::string x = exe_in_root(p);
            if (!x.empty()) return x;
        }
        scan_roots.push_back(root);
    }

    for (auto& r : scan_roots) {
        int budget = 4000;
        std::string x = scan_for_game(r, 4, budget);
        if (!x.empty()) return x;
    }
    return "";
}

bool launch_game(const std::string& exe_path, LogFn log) {
    if (exe_path.empty()) { if (log) log(3, "Ruta del ejecutable vacía."); return false; }
    DWORD attr = GetFileAttributesW(narrow_to_widen(exe_path).c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        if (log) log(3, "Ejecutable no encontrado: " + exe_path);
        return false;
    }

    std::wstring env;
    LPWCH cur = GetEnvironmentStringsW();
    if (cur) {
        for (LPWCH p = cur; *p; p += wcslen(p) + 1) {
            if (_wcsnicmp(p, L"__COMPAT_LAYER=", 15) != 0) {
                env += p;
                env += L'\0';
            }
        }
        FreeEnvironmentStringsW(cur);
    }
    env += L"__COMPAT_LAYER=RUNASINVOKER";
    env += L'\0';
    env += L'\0';

    std::wstring wdir = narrow_to_widen(exe_path);
    size_t slash = wdir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) wdir = wdir.substr(0, slash);

    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + narrow_to_widen(exe_path) + L"\"";
    BOOL good = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE,
                               CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP,
                               (LPVOID)env.data(), wdir.empty() ? nullptr : wdir.c_str(), &si, &pi);
    if (!good) {
        if (log) log(3, "CreateProcess falló (error " + std::to_string(GetLastError()) + ").");
        return false;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (log) log(1, "Juego lanzado en Medium IL (NEProtect.sys bloqueado). PID " + std::to_string(pi.dwProcessId));
    return true;
}

}
