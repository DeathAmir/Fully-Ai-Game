#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace irx::security {

struct IntegrityRecord {
    std::filesystem::path path;
    std::uint64_t hash = 0;
    std::uintmax_t size = 0;
};

std::uint64_t hashFile(const std::filesystem::path& path);
std::vector<IntegrityRecord> scanCriticalFiles(const std::filesystem::path& root);
std::string fingerprint(const std::vector<IntegrityRecord>& records);
bool debuggerPresent();

}