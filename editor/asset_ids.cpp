#include "asset_ids.h"

#include <files.h>
#include <log.h>
#include <project.h>

#include <nlohmann/json.hpp>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <map>
#include <random>
#include <regex>

namespace {

namespace fs = std::filesystem;

// path upper-cased: equal keys are the same file on Windows.
std::wstring caseKey(std::string_view path)
{
    std::wstring key = fromUtf8(path).generic_wstring();
    CharUpperBuffW(key.data(), static_cast<DWORD>(key.size()));
    return key;
}

// The newer of file's creation and modification times (a copy is created now but keeps its modification time).
std::uint64_t newestTime(const fs::path& file)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data))
        return 0;
    const auto value = [](const FILETIME& time) {
        return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    return std::max(value(data.ftCreationTime), value(data.ftLastWriteTime));
}

void collectFiles(const std::vector<AssetEntry>& entries, std::vector<std::string>& out)
{
    for (const AssetEntry& entry : entries)
        if (entry.folder)
            collectFiles(entry.children, out);
        else if (!entry.name.ends_with(".tmp")) // ponytail: a writeFileAtomic temporary, gone by the next scan
            out.push_back(entry.path);
}

// Folder and extension of a project-relative path, upper-cased: an orphan's re-pairing group.
std::wstring pairGroup(const std::string& path)
{
    const fs::path file = fromUtf8(path);
    return caseKey(utf8(file.parent_path())) + L'|' + caseKey(utf8(file.extension()));
}

} // namespace

std::string newAssetId()
{
    static std::mt19937_64 random = [] {
        std::random_device device;
        std::seed_seq seed{device(), device(), device(), device()};
        return std::mt19937_64(seed);
    }();
    std::uint8_t bytes[16];
    for (int i = 0; i < 16; i += 8) {
        const std::uint64_t value = random();
        for (int b = 0; b < 8; ++b)
            bytes[i + b] = static_cast<std::uint8_t>(value >> (b * 8));
    }
    bytes[6] = (bytes[6] & 0x0f) | 0x40; // version 4
    bytes[8] = (bytes[8] & 0x3f) | 0x80; // RFC 4122 variant
    std::string id;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            id += '-';
        id += std::format("{:02x}", bytes[i]);
    }
    return id;
}

bool isAssetId(std::string_view text)
{
    static const std::regex uuid("[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}");
    return std::regex_match(text.begin(), text.end(), uuid);
}

std::optional<std::string> readAssetMeta(const fs::path& meta)
{
    std::string text;
    if (!kast::readFile(meta, text))
        return std::nullopt;
    const nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
    if (!json.is_object() || !json.contains("id") || !json["id"].is_string() || !isAssetId(json["id"].get<std::string>()))
        return std::nullopt;
    return json["id"].get<std::string>();
}

bool writeAssetMeta(const fs::path& meta, std::string_view id, std::string& error)
{
    return writeTextFile(meta, nlohmann::ordered_json{{"id", id}}.dump(2) + "\n", error);
}

void AssetIds::scan(const fs::path& root)
{
    std::vector<fs::directory_entry> metas;
    const std::vector<AssetEntry> entries = scanAssets(root, &metas);
    scan(root, entries, metas);
}

void AssetIds::scan(const fs::path& root, const std::vector<AssetEntry>& entries,
                    const std::vector<fs::directory_entry>& metas)
{
    if (root != root_) {
        root_ = root;
        metaCache_.clear();
    }
    // Every .meta, by its asset's path: what it holds (cached while the file is unchanged).
    struct Meta {
        std::string asset; // project-relative
        std::optional<std::string> id;
    };
    std::unordered_map<std::wstring, Meta> metaByAsset;
    for (const fs::directory_entry& meta : metas) {
        std::error_code ec;
        const fs::file_time_type time = meta.last_write_time(ec);
        auto cached = metaCache_.find(meta.path().native());
        if (cached == metaCache_.end() || cached->second.time != time)
            cached = metaCache_.insert_or_assign(meta.path().native(), CachedMeta{time, readAssetMeta(meta.path())}).first;
        std::string asset = kast::toProjectPath(root, meta.path()).value_or(utf8(meta.path()));
        asset.resize(asset.size() - kMetaExtension.size());
        metaByAsset[caseKey(asset)] = {asset, cached->second.id};
    }

    std::vector<std::string> files;
    collectFiles(entries, files);
    std::map<std::string, std::string> idOfFile; // path -> id, for files with a valid .meta
    std::vector<std::string> metaless;
    for (const std::string& file : files) {
        const auto meta = metaByAsset.find(caseKey(file));
        if (meta != metaByAsset.end() && meta->second.id)
            idOfFile[file] = *meta->second.id;
        else
            metaless.push_back(file);
        if (meta != metaByAsset.end()) {
            if (!meta->second.id)
                kast::log::warn(std::format("'{}{}' holds no valid asset id: replaced", file, kMetaExtension));
            metaByAsset.erase(meta);
        }
    }
    // What is left in metaByAsset are orphans. Re-pair the unambiguous ones: one orphan, one meta-less file per group.
    std::map<std::wstring, std::pair<int, int>> groups; // folder|extension -> orphans, meta-less files
    for (const auto& [key, orphan] : metaByAsset)
        ++groups[pairGroup(orphan.asset)].first;
    for (const std::string& file : metaless)
        ++groups[pairGroup(file)].second;
    std::string error;
    for (auto orphan = metaByAsset.begin(); orphan != metaByAsset.end();) {
        const std::wstring group = pairGroup(orphan->second.asset);
        const auto adopter = std::ranges::find_if(metaless, [&](const std::string& file) { return pairGroup(file) == group; });
        if (orphan->second.id && groups[group] == std::pair(1, 1) &&
            MoveFileExW(metaFileOf(root / fromUtf8(orphan->second.asset)).c_str(), metaFileOf(root / fromUtf8(*adopter)).c_str(),
                        0)) {
            kast::log::info(std::format("'{}' took the asset id of missing '{}' (its .meta was left behind)", *adopter,
                                        orphan->second.asset));
            idOfFile[*adopter] = *orphan->second.id;
            metaless.erase(adopter);
            orphan = metaByAsset.erase(orphan);
        } else {
            ++orphan;
        }
    }
    for (const auto& [key, orphan] : metaByAsset) {
        const fs::path meta = metaFileOf(root / fromUtf8(orphan.asset));
        if (kast::recycleFile(meta, error))
            kast::log::info(std::format("Moved orphan '{}{}' to the Recycle Bin: '{}' is gone", orphan.asset,
                                        kMetaExtension, orphan.asset));
        else
            kast::log::error(std::format("Cannot recycle orphan '{}{}': {}", orphan.asset, kMetaExtension, error));
    }

    const auto giveNewId = [&](const std::string& file) {
        const std::string id = newAssetId();
        if (writeAssetMeta(metaFileOf(root / fromUtf8(file)), id, error))
            idOfFile[file] = id;
        else
            kast::log::error(std::format("Cannot write the asset id of '{}': {}", file, error));
    };
    for (const std::string& file : metaless)
        giveNewId(file);
    if (!metaless.empty())
        kast::log::info(std::format("Created {} asset id file(s) (.meta)", metaless.size()));

    std::map<std::string, std::vector<std::string>> filesOfId;
    for (const auto& [file, id] : idOfFile)
        filesOfId[id].push_back(file);
    for (auto& [id, sharing] : filesOfId) {
        if (sharing.size() < 2)
            continue;
        std::vector<std::pair<std::uint64_t, std::string>> byAge;
        for (const std::string& file : sharing)
            byAge.emplace_back(newestTime(root / fromUtf8(file)), file);
        std::ranges::sort(byAge, [](const auto& a, const auto& b) {
            return a.first != b.first ? a.first < b.first : caseKey(a.second) < caseKey(b.second);
        });
        for (size_t i = 1; i < byAge.size(); ++i) {
            kast::log::warn(std::format("'{}' and '{}' had the same asset id (a copy with its .meta?): '{}' got a new one",
                                        byAge[0].second, byAge[i].second, byAge[i].second));
            giveNewId(byAge[i].second);
        }
    }

    paths_.clear();
    ids_.clear();
    for (const auto& [file, id] : idOfFile) {
        paths_[id] = file;
        ids_[caseKey(file)] = id;
    }
}

std::optional<std::string> AssetIds::pathOf(std::string_view id) const
{
    const auto it = paths_.find(std::string(id));
    return it == paths_.end() ? std::nullopt : std::optional(it->second);
}

std::optional<std::string> AssetIds::idOf(std::string_view path) const
{
    const auto it = ids_.find(caseKey(path));
    return it == ids_.end() ? std::nullopt : std::optional(it->second);
}

AssetIds& assetIds()
{
    static AssetIds ids;
    return ids;
}
