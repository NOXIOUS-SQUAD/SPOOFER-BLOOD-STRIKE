#pragma once
#include <string>
#include <vector>
#include <functional>

#include "identity.h"

namespace spoofer {

using LogFn = std::function<void(int level, const std::string& msg)>;

struct Options {

    bool layer_driver_block = true;
    bool layer_purge_cache  = true;
    bool layer_registry     = true;
    bool layer_volume       = true;
    bool layer_mac          = true;

    bool sinkhole_telemetry = true;
    bool clean_traces       = true;
    bool make_backup        = true;
    bool launch_after_spoof = true;
};

struct PreflightItem {
    std::string label;
    std::string current;
    std::string will_be;
    std::string note;
    bool ok = true;
};

std::vector<PreflightItem> preflight(const Options& opt, const identity::Profile& profile);

bool apply(const Options& opt, const identity::Profile& profile, const std::string& profile_dir, LogFn log);

bool restore(const identity::Profile& backup, LogFn log);

bool launch_game(const std::string& exe_path, LogFn log);

std::string find_game_exe();
bool game_running();

std::vector<std::string> cache_paths();

bool sinkhole_add(LogFn log);
bool sinkhole_remove(LogFn log);

bool service_ne_protect_disable(LogFn log);
bool service_ne_protect_restore(LogFn log);

}
