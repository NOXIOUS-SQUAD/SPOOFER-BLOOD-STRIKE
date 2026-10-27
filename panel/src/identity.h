#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace identity {

std::string md5_hex(const std::string& input);

bool is_elevated();

std::string read_machine_guid();
std::string read_hw_profile_guid();
std::string read_install_date();
std::string read_product_id();
std::string read_volume_serial(char drive_letter);
std::string read_disk_serial();
std::string read_cpu_id();

struct AdapterInfo {
    std::string friendly_name;
    std::string mac;
    std::string registry_path;
    uint32_t    index = 0;
    bool        is_up = false;
    bool        is_primary = false;
};
std::vector<AdapterInfo> read_adapters();

std::string compute_udid(const std::string& mac);

std::string read_smbios_serial();

std::string gen_guid();
std::string gen_guid_plain();
std::string gen_mac();
std::string gen_product_id();
std::string gen_install_date();
std::string gen_volume_serial();
std::string gen_profile_name();

struct Profile {
    std::string name;
    std::string created_utc;
    bool is_backup = false;

    std::string machine_guid;
    std::string install_date;
    std::string product_id;
    std::string hw_profile_guid;
    std::string network_address;
    std::string adapter_index;
    std::string volume_serial;

    std::string orig_machine_guid;
    std::string orig_install_date;
    std::string orig_product_id;
    std::string orig_hw_profile_guid;
    std::string orig_network_address;

    bool empty() const {
        return machine_guid.empty() && hw_profile_guid.empty() &&
               product_id.empty() && install_date.empty() &&
               network_address.empty() && volume_serial.empty();
    }
};

Profile capture_current();
Profile make_synthetic(const std::string& name = std::string());

bool save_profile(const Profile& p, const std::string& path);
bool load_profile(const std::string& path, Profile& out);
std::vector<std::string> list_profiles(const std::string& dir);

std::string utc_now();

}
