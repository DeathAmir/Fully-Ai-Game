#include "security/integrity.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace irx::security {

std::uint64_t hashFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return 0;
    std::uint64_t hash = 1469598103934665603ull;
    char buffer[16384];
    while (file) {
        file.read(buffer, sizeof(buffer));
        const auto count = file.gcount();
        for (std::streamsize i = 0; i < count; ++i) {
            hash ^= static_cast<unsigned char>(buffer[i]);
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

std::vector<IntegrityRecord> scanCriticalFiles(const std::filesystem::path& root) {
    std::vector<IntegrityRecord> records;
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return records;
    for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) continue;
        const auto extension = it->path().extension().string();
        if (extension != ".exe" && extension != ".dll" && extension != ".na1" && extension != ".naupk" && extension != ".json") continue;
        IntegrityRecord record;
        record.path = std::filesystem::relative(it->path(), root, ec);
        if (ec) { ec.clear(); record.path = it->path().filename(); }
        record.size = it->file_size(ec);
        if (ec) { ec.clear(); record.size = 0; }
        record.hash = hashFile(it->path());
        records.push_back(std::move(record));
    }
    std::sort(records.begin(), records.end(), [](const IntegrityRecord& a, const IntegrityRecord& b) { return a.path.generic_string() < b.path.generic_string(); });
    return records;
}

std::string fingerprint(const std::vector<IntegrityRecord>& records) {
    std::uint64_t combined = 1469598103934665603ull;
    for (const auto& record : records) {
        const std::string path = record.path.generic_string();
        for (unsigned char c : path) { combined ^= c; combined *= 1099511628211ull; }
        combined ^= record.hash;
        combined *= 1099511628211ull;
        combined ^= static_cast<std::uint64_t>(record.size);
        combined *= 1099511628211ull;
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << combined;
    return out.str();
}

bool debuggerPresent() {
#ifdef _WIN32
    return IsDebuggerPresent() != FALSE;
#else
    return false;
#endif
}

}