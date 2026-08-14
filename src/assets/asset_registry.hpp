#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace irx::assets {

struct AssetEntry {
    std::string id;
    std::filesystem::path path;
    std::string category;
};

class Registry {
public:
    void scan(const std::filesystem::path& root);
    const AssetEntry* find(const std::string& id) const;
    std::vector<AssetEntry> byCategory(const std::string& category) const;
    std::size_t size() const noexcept;

private:
    std::unordered_map<std::string, AssetEntry> entries_;
};

std::string makeAssetId(const std::filesystem::path& relative);

}