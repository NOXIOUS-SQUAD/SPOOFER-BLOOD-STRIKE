#include "identity.h"
#include "tinyjson.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <winioctl.h>
#include <intrin.h>
#include <ctime>
#include <iterator>
#include <random>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "advapi32.lib")

namespace identity {

namespace {

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}

std::mt19937& rng() {
    static std::mt19937 r([] {
        std::random_device rd;
        auto seed = ((uint64_t)rd() << 32) ^ rd() ^ (uint64_t)GetTickCount64();
        return std::mt19937((uint32_t)(seed ^ (seed >> 32)));
    }());
    return r;
}

std::string rand_hex(int chars, bool upper) {
    static const char* lo = "0123456789abcdef";
    static const char* up = "0123456789ABCDEF";
    const char* t = upper ? up : lo;
    std::string s;
    for (int i = 0; i < chars; i++) s += t[rng()() % 16];
    return s;
}

std::string reg_read_str(HKEY root, const wchar_t* subkey, const wchar_t* value, bool wow64 = true) {
    HKEY k = nullptr;
    REGSAM access = KEY_READ | (wow64 ? KEY_WOW64_64KEY : 0);
    if (RegOpenKeyExW(root, subkey, 0, access, &k) != ERROR_SUCCESS) return "";
    wchar_t buf[1024]{};
    DWORD size = sizeof(buf);
    DWORD type = 0;
    std::string out;
    if (RegQueryValueExW(k, value, nullptr, &type, (LPBYTE)buf, &size) == ERROR_SUCCESS) {
        if (type == REG_SZ || type == REG_EXPAND_SZ)
            out = narrow(std::wstring(buf, size / sizeof(wchar_t) - (size >= sizeof(wchar_t) ? 1 : 0)));
    }
    RegCloseKey(k);

    while (!out.empty() && out.back() == '\0') out.pop_back();
    return trim(out);
}

DWORD reg_read_dword(HKEY root, const wchar_t* subkey, const wchar_t* value, DWORD def = 0, bool wow64 = true) {
    HKEY k = nullptr;
    REGSAM access = KEY_READ | (wow64 ? KEY_WOW64_64KEY : 0);
    if (RegOpenKeyExW(root, subkey, 0, access, &k) != ERROR_SUCCESS) return def;
    DWORD v = def, size = sizeof(v), type = 0;
    if (RegQueryValueExW(k, value, nullptr, &type, (LPBYTE)&v, &size) != ERROR_SUCCESS || type != REG_DWORD)
        v = def;
    RegCloseKey(k);
    return v;
}

}

namespace {

struct MD5 {
    uint32_t s[4] = { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u };
    uint64_t bitlen = 0;
    uint8_t buf[64]{};
    size_t buflen = 0;

    static uint32_t rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

    void block(const uint8_t* m) {
        static const uint32_t K[64] = {
            0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
            0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
            0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
            0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
            0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
            0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
            0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
            0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
        };
        static const int S[64] = {
            7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
            5, 9,14,20,5, 9,14,20,5, 9,14,20,5, 9,14,20,
            4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
            6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
        };
        uint32_t M[16];
        memcpy(M, m, 64);
        uint32_t A = s[0], B = s[1], C = s[2], D = s[3];
        for (int i = 0; i < 64; i++) {
            uint32_t f; int g;
            if (i < 16)      { f = (B & C) | (~B & D);        g = i; }
            else if (i < 32) { f = (D & B) | (~D & C);        g = (5 * i + 1) & 15; }
            else if (i < 48) { f = B ^ C ^ D;                 g = (3 * i + 5) & 15; }
            else             { f = C ^ (B | ~D);              g = (7 * i) & 15; }
            uint32_t tmp = D; D = C; C = B;
            B = B + rol(A + f + K[i] + M[g], S[i]);
            A = tmp;
        }
        s[0] += A; s[1] += B; s[2] += C; s[3] += D;
    }

    void update(const uint8_t* data, size_t len) {
        bitlen += (uint64_t)len * 8;
        while (len > 0) {
            size_t take = 64 - buflen; if (take > len) take = len;
            memcpy(buf + buflen, data, take);
            buflen += take; data += take; len -= take;
            if (buflen == 64) { block(buf); buflen = 0; }
        }
    }

    std::string final_hex() {
        uint64_t bits = bitlen;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (buflen != 56) update(&zero, 1);
        uint8_t lenb[8];
        for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)((bits >> (8 * i)) & 0xFF);
        update(lenb, 8);
        char out[33];
        snprintf(out, sizeof(out), "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
            (s[0]) & 0xFF, (s[0] >> 8) & 0xFF, (s[0] >> 16) & 0xFF, (s[0] >> 24) & 0xFF,
            (s[1]) & 0xFF, (s[1] >> 8) & 0xFF, (s[1] >> 16) & 0xFF, (s[1] >> 24) & 0xFF,
            (s[2]) & 0xFF, (s[2] >> 8) & 0xFF, (s[2] >> 16) & 0xFF, (s[2] >> 24) & 0xFF,
            (s[3]) & 0xFF, (s[3] >> 8) & 0xFF, (s[3] >> 16) & 0xFF, (s[3] >> 24) & 0xFF);
        return std::string(out, 32);
    }
};

}

std::string md5_hex(const std::string& input) {
    MD5 m;
    m.update((const uint8_t*)input.data(), input.size());
    return m.final_hex();
}

bool is_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elev{};
    DWORD size = sizeof(elev);
    bool ok = GetTokenInformation(token, TokenElevation, &elev, size, &size) && elev.TokenIsElevated;
    CloseHandle(token);
    return ok;
}

std::string read_machine_guid() {
    return reg_read_str(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid");
}

std::string read_hw_profile_guid() {
    return reg_read_str(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\IDConfigDB\\Hardware Profiles\\0001", L"HwProfileGuid");
}

std::string read_install_date() {
    DWORD v = reg_read_dword(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"InstallDate", 0);
    if (!v) return "";
    char t[32]; snprintf(t, sizeof(t), "%lu", (unsigned long)v);
    return t;
}

std::string read_product_id() {
    return reg_read_str(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"ProductId");
}

std::string read_volume_serial(char drive) {
    wchar_t root[4] = { (wchar_t)drive, L':', L'\\', 0 };
    DWORD serial = 0;
    if (!GetVolumeInformationW(root, nullptr, 0, &serial, nullptr, nullptr, nullptr, 0))
        return "";
    char t[16];
    snprintf(t, sizeof(t), "%04lX-%04lX",
        (unsigned long)((serial >> 16) & 0xFFFF), (unsigned long)(serial & 0xFFFF));
    return t;
}

std::string read_disk_serial() {
    HANDLE h = CreateFileW(L"\\\\.\\PhysicalDrive0", 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    BYTE buf[8192]{};
    DWORD ret = 0;
    std::string serial;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &ret, nullptr)) {
        auto* d = (STORAGE_DEVICE_DESCRIPTOR*)buf;
        if (d->SerialNumberOffset && d->SerialNumberOffset < sizeof(buf)) {
            serial = trim(std::string((const char*)buf + d->SerialNumberOffset));
            std::replace(serial.begin(), serial.end(), ' ', '_');
        }
    }
    CloseHandle(h);
    return serial;
}

std::string read_cpu_id() {
    int regs[4] = { 0, 0, 0, 0 };
    __cpuid(regs, 0x80000000);
    if ((unsigned)regs[0] >= 0x80000004u) {
        char brand[0x40]{};
        for (int i = 0; i < 3; i++) {
            __cpuid(regs, 0x80000002 + i);
            memcpy(brand + i * 16, regs, 16);
        }
        std::string s = trim(brand);
        if (!s.empty()) return s;
    }

    __cpuid(regs, 0); int a0 = regs[0] ^ regs[1] ^ regs[2] ^ regs[3];
    __cpuid(regs, 1); int a1 = regs[0] ^ regs[3];
    char t[32]; snprintf(t, sizeof(t), "%08X%08X", (unsigned)a0, (unsigned)a1);
    return t;
}

namespace {

std::string mac_to_string(const BYTE* mac, DWORD len, const char* sep = ":") {
    if (len < 6) return "";
    char t[18];
    snprintf(t, sizeof(t), "%02X%s%02X%s%02X%s%02X%s%02X%s%02X",
        mac[0], sep, mac[1], sep, mac[2], sep, mac[3], sep, mac[4], sep, mac[5]);
    return t;
}

std::string find_adapter_class_key(const std::string& instance_guid) {
    static const wchar_t* CLASS_PATH =
        L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e972-e325-11ce-bfc1-08002be10318}";
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, CLASS_PATH, 0, KEY_READ | KEY_ENUMERATE_SUB_KEYS | KEY_WOW64_64KEY, &root) != ERROR_SUCCESS)
        return "";
    std::string result;
    wchar_t name[16]{};
    for (DWORD i = 0; ; i++) {
        DWORD nlen = 16;
        FILETIME ft{};
        LONG r = RegEnumKeyExW(root, i, name, &nlen, nullptr, nullptr, nullptr, &ft);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) continue;
        HKEY sub = nullptr;
        if (RegOpenKeyExW(root, name, 0, KEY_READ | KEY_WOW64_64KEY, &sub) != ERROR_SUCCESS) continue;
        wchar_t guid[128]{}; DWORD gsize = sizeof(guid), gtype = 0;
        if (RegQueryValueExW(sub, L"NetCfgInstanceId", nullptr, &gtype, (LPBYTE)guid, &gsize) == ERROR_SUCCESS) {
            std::string g = lower(narrow(std::wstring(guid)));
            if (g == lower(instance_guid))
                result = narrow(std::wstring(CLASS_PATH) + L"\\" + name);
        }
        RegCloseKey(sub);
        if (!result.empty()) break;
    }
    RegCloseKey(root);
    return result;
}

}

std::vector<AdapterInfo> read_adapters() {
    std::vector<AdapterInfo> out;
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 0;
    if (GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW || !size)
        return out;
    std::vector<BYTE> heapbuf(size);
    auto* addrs = (IP_ADAPTER_ADDRESSES*)heapbuf.data();
    if (GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addrs, &size) != NO_ERROR)
        return out;

    for (auto* a = addrs; a; a = a->Next) {
        AdapterInfo ai;
        ai.friendly_name = narrow(a->FriendlyName ? a->FriendlyName : L"");
        ai.mac = mac_to_string(a->PhysicalAddress, a->PhysicalAddressLength);
        ai.index = a->IfIndex;
        ai.is_up = (a->OperStatus == IfOperStatusUp);
        ai.registry_path = find_adapter_class_key(a->AdapterName ? a->AdapterName : "");
        if (!ai.mac.empty()) {
            bool physical = (a->IfType == IF_TYPE_ETHERNET_CSMACD || a->IfType == IF_TYPE_IEEE80211);
            if (physical)
                out.push_back(ai);
        }
    }

    for (auto& ai : out) {
        if (ai.is_up && !ai.registry_path.empty()) { ai.is_primary = true; break; }
    }
    return out;
}

std::string compute_udid(const std::string& mac) {

    std::string mac_clean;
    for (char c : mac) if (c != ':' && c != '-') mac_clean += (char)toupper((unsigned char)c);
    std::string raw = mac_clean + read_cpu_id() + read_disk_serial() + read_machine_guid();
    std::string h = md5_hex(raw);
    std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return (char)toupper(c); });
    return h;
}

std::string read_smbios_serial() {
    UINT size = GetSystemFirmwareTable('RSMB', 0, nullptr, 0);
    if (!size) return "";
    std::vector<BYTE> buf(size);
    if (GetSystemFirmwareTable('RSMB', 0, buf.data(), size) != size) return "";

    BYTE* table = buf.data() + 8;
    DWORD table_len = size - 8;
    BYTE* p = table;
    BYTE* end = table + table_len;

    while (p + 4 <= end) {
        BYTE type = p[0];
        BYTE len = p[1];
        if (len < 4) break;
        if (type == 1 && len >= 8) {
            BYTE serial_idx = p[7];
            if (serial_idx) {
                const char* strtab = (const char*)p + len;
                int idx = 1;
                const char* q = strtab;
                while (q < (const char*)end && *q) {
                    if (idx == serial_idx) {
                        std::string s(q);
                        size_t z = s.find('\0');
                        return trim(s.substr(0, z));
                    }
                    q += strlen(q) + 1;
                    idx++;
                }
            }
        }
        BYTE* n = p + len;
        while (n + 1 < end && !(n[0] == 0 && n[1] == 0)) n++;
        p = n + 2;
    }
    return "";
}

std::string gen_guid_plain() {
    static const char* hex = "0123456789abcdef";
    auto blk = [](int n) { std::string s; for (int i = 0; i < n; i++) s += hex[rng()() & 15]; return s; };
    return blk(8) + "-" + blk(4) + "-4" + blk(3) + "-" +
        "89ab"[rng()() % 4] + blk(3) + "-" + blk(12);
}

std::string gen_guid() { return "{" + gen_guid_plain() + "}"; }

std::string gen_mac() {

    static const char* lo = "0123456789ABCDEF";
    static const char* second = "26AE";
    std::string s;
    s += '0';
    s += second[rng()() % 4];
    for (int i = 0; i < 5; i++) {
        s += ':';
        s += lo[rng()() & 15];
        s += lo[rng()() & 15];
    }
    return s;
}

std::string gen_product_id() {

    std::string s;
    for (int blk = 0; blk < 4; blk++) {
        if (blk) s += '-';
        int n = (blk == 3) ? 3 : 5;
        for (int i = 0; i < n; i++) s += (char)('0' + rng()() % 10);
        if (blk == 3) s += (char)('A' + rng()() % 26);
    }
    return s;
}

std::string gen_install_date() {

    uint64_t lo = 1640995200ull, hi = 1767225599ull;
    uint64_t v = lo + (uint64_t)((rng()() << 17) ^ rng()()) % (hi - lo);
    char t[32]; snprintf(t, sizeof(t), "%llu", (unsigned long long)v);
    return t;
}

std::string gen_volume_serial() {

    std::string s;
    s += "123456789ABCDEF"[rng()() % 15];
    for (int i = 0; i < 7; i++) s += "0123456789ABCDEF"[rng()() & 15];
    return s;
}

std::string gen_profile_name() {

    static const char* pre[] = { "ka","lo","vi","mi","nu","ar","zy","vx","ro","te","ki","sa","do","fe","ru","ni","xa","po","lu","me" };
    static const char* suf[] = { "ren","dor","vex","lin","sar","tor","mex","nix","rav","kel","zon","pir","tav","sol","mur","nex" };
    const int np = (int)(sizeof(pre) / sizeof(pre[0]));
    const int ns = (int)(sizeof(suf) / sizeof(suf[0]));

    char b[40];
    snprintf(b, sizeof(b), "%s%s%02d", pre[rng()() % np], suf[rng()() % ns], 10 + (int)(rng()() % 90));
    return b;
}

std::string utc_now() {
    time_t t = time(nullptr);
    struct tm g{};
    gmtime_s(&g, &t);
    char b[40];
    strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &g);
    return b;
}

Profile capture_current() {
    Profile p;
    p.name = "backup_original";
    p.created_utc = utc_now();
    p.is_backup = true;

    p.orig_machine_guid   = p.machine_guid   = read_machine_guid();
    p.orig_install_date   = p.install_date   = read_install_date();
    p.orig_product_id     = p.product_id     = read_product_id();
    p.orig_hw_profile_guid = p.hw_profile_guid = read_hw_profile_guid();

    auto adapters = read_adapters();
    for (auto& a : adapters) {
        if (a.is_primary) {
            p.orig_network_address = p.network_address = a.mac;
            p.adapter_index = std::to_string(a.index);
            break;
        }
    }
    if (p.network_address.empty() && !adapters.empty()) {
        p.orig_network_address = p.network_address = adapters.front().mac;
        p.adapter_index = std::to_string(adapters.front().index);
    }
    std::string vs = read_volume_serial('C');
    std::string raw;
    for (char c : vs) if (c != '-') raw += c;
    p.volume_serial = raw;
    return p;
}

Profile make_synthetic(const std::string& name) {
    Profile p;
    p.name = name.empty() ? gen_profile_name() : name;
    p.created_utc = utc_now();
    p.is_backup = false;

    p.orig_machine_guid   = read_machine_guid();
    p.orig_install_date   = read_install_date();
    p.orig_product_id     = read_product_id();
    p.orig_hw_profile_guid = read_hw_profile_guid();
    auto adapters = read_adapters();
    for (auto& a : adapters) {
        if (a.is_primary) {
            p.orig_network_address = a.mac;
            p.adapter_index = std::to_string(a.index);
            break;
        }
    }

    p.machine_guid    = gen_guid_plain();
    p.install_date    = gen_install_date();
    p.product_id      = gen_product_id();
    p.hw_profile_guid = gen_guid();
    p.network_address = gen_mac();
    p.volume_serial   = gen_volume_serial();
    return p;
}

bool save_profile(const Profile& p, const std::string& path) {
    tj::Value v;
    v.type = tj::Type::Object;
    v.set("profile_name", tj::Value(p.name));
    v.set("created_utc", tj::Value(p.created_utc));
    v.set("is_backup", tj::Value(p.is_backup));
    v.set("machine_guid", tj::Value(p.machine_guid));
    v.set("install_date", tj::Value(p.install_date));
    v.set("product_id", tj::Value(p.product_id));
    v.set("hw_profile_guid", tj::Value(p.hw_profile_guid));
    v.set("network_address", tj::Value(p.network_address));
    v.set("adapter_index", tj::Value(p.adapter_index));
    v.set("volume_serial", tj::Value(p.volume_serial));
    v.set("orig_machine_guid", tj::Value(p.orig_machine_guid));
    v.set("orig_install_date", tj::Value(p.orig_install_date));
    v.set("orig_product_id", tj::Value(p.orig_product_id));
    v.set("orig_hw_profile_guid", tj::Value(p.orig_hw_profile_guid));
    v.set("orig_network_address", tj::Value(p.orig_network_address));

    std::string text = tj::dump(v);
    std::ofstream f(widen(path), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(text.data(), (std::streamsize)text.size());
    return f.good();
}

bool load_profile(const std::string& path, Profile& out) {
    std::ifstream f(widen(path), std::ios::binary);
    if (!f) return false;
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    tj::Value v;
    if (!tj::parse(text, v) || !v.is_object()) return false;
    out = Profile{};
    out.name              = v.get_str("profile_name");
    out.created_utc       = v.get_str("created_utc");
    out.is_backup         = v.get_bool("is_backup");
    out.machine_guid      = v.get_str("machine_guid");
    out.install_date      = v.get_str("install_date");
    out.product_id        = v.get_str("product_id");
    out.hw_profile_guid   = v.get_str("hw_profile_guid");
    out.network_address   = v.get_str("network_address");
    out.adapter_index     = v.get_str("adapter_index");
    out.volume_serial     = v.get_str("volume_serial");
    out.orig_machine_guid = v.get_str("orig_machine_guid");
    out.orig_install_date = v.get_str("orig_install_date");
    out.orig_product_id   = v.get_str("orig_product_id");
    out.orig_hw_profile_guid = v.get_str("orig_hw_profile_guid");
    out.orig_network_address = v.get_str("orig_network_address");
    return !out.empty();
}

std::vector<std::string> list_profiles(const std::string& dir) {
    std::vector<std::string> out;
    std::wstring pattern = widen(dir);
    if (!pattern.empty() && pattern.back() != L'\\') pattern += L'\\';
    pattern += L"*.json";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            out.push_back(narrow(fd.cFileName));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

}
