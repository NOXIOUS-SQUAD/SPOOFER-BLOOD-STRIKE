#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <functional>

#include "spoofer.h"

namespace cleaner {

struct CleanTarget {
    std::string label;
    std::string path;
    uint64_t    bytes = 0;
    uint64_t    files = 0;
    bool        exists = false;
    bool        needs_admin = false;
};

std::vector<CleanTarget> scan_temp();
std::vector<CleanTarget> scan_own_traces();
std::vector<CleanTarget> scan_game_traces();

struct WipeReport {
    uint64_t files_wiped = 0;
    uint64_t files_failed = 0;
    uint64_t bytes_freed = 0;
};
WipeReport wipe(const std::vector<CleanTarget>& targets, spoofer::LogFn log);

void wipe_self_traces(spoofer::LogFn log);

std::string current_exe_name();

}
