#include "assets/asset_registry.hpp"

#include <algorithm>
#include <cctype>

namespace irx::assets {

std::string makeAssetId(const std::filesystem::path& relative) {
    std::string id = relative.generic_string();
    std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) {
        if (c == '/' || c == '\\' || c == ' ') return '_';
        return static_cast<char>(std::tolower(c));
    });
    return id;
}

void Registry::scan(const std::filesystem::path& root) {
    entries_.clear();
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return;
    for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) continue;
        const auto relative = std::filesystem::relative(it->path(), root, ec);
        if (ec) { ec.clear(); continue; }
        const auto extension = it->path().extension().string();
        if (extension != ".glb" && extension != ".gltf" && extension != ".png" && extension != ".jpg" && extension != ".jpeg" && extension != ".wav" && extension != ".ogg" && extension != ".json") continue;
        std::string category = "misc";
        if (!relative.empty()) category = (*relative.begin()).string();
        AssetEntry entry{makeAssetId(relative), it->path(), std::move(category)};
        entries_.insert_or_assign(entry.id, std::move(entry));
    }
}

const AssetEntry* Registry::find(const std::string& id) const {
    const auto it = entries_.find(id);
    return it == entries_.end() ? nullptr : &it->second;
}

std::vector<AssetEntry> Registry::byCategory(const std::string& category) const {
    std::vector<AssetEntry> result;
    for (const auto& [id, entry] : entries_) {
        (void)id;
        if (entry.category == category) result.push_back(entry);
    }
    std::sort(result.begin(), result.end(), [](const AssetEntry& a, const AssetEntry& b) { return a.id < b.id; });
    return result;
}

std::size_t Registry::size() const noexcept { return entries_.size(); }

}