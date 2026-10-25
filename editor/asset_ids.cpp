#include "asset_ids.h"

#include <asset_ref.h>
#include <files.h>
#include <gui_prefabs.h>
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
#include <unordered_map>

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

size_t staleRefs = 0;
// A missing ref's id by its hint (caseKey), so writing the hint keeps the id.
std::unordered_map<std::wstring, std::string>& rememberedIds()
{
    static std::unordered_map<std::wstring, std::string> ids;
    return ids;
}

bool hiddenPath(std::string_view path) { return path.starts_with('.') || path.find("/.") != std::string_view::npos; }

// editAssetRefs for GUI markup (the document at project path document): each start tag holding a ref is rewritten in
// place, only the values that changed; the rest of the text stays as written.
// ponytail: regex over start tags (quoted '>' allowed), double-quoted attributes only, as the GUI Builder writes them.
bool editGuiRefs(std::string& text, std::string_view document, const AssetRefEdit& edit)
{
    static const std::regex tag(R"re(<[A-Za-z][\w-]*(?:[^>"']|"[^"]*"|'[^']*')*>)re");
    static const std::regex attribute(R"re(([\w:-]+)\s*=\s*"([^"]*)")re");
    const fs::path dir = fromUtf8(document).parent_path();
    std::string out;
    bool changed = false;
    auto last = text.cbegin();
    for (auto it = std::sregex_iterator(text.cbegin(), text.cend(), tag); it != std::sregex_iterator(); ++it) {
        std::string element = (*it)[0].str();
        struct Value {
            size_t at = std::string::npos, size = 0; // in element
            std::string text;
        };
        std::map<std::string, Value> values;
        for (auto a = std::sregex_iterator(element.cbegin(), element.cend(), attribute); a != std::sregex_iterator(); ++a)
            values[(*a)[1].str()] = {static_cast<size_t>(a->position(2)), static_cast<size_t>(a->length(2)), (*a)[2].str()};
        const bool link = caseKey(element.substr(0, 6)) == L"<LINK " && values["type"].text == "text/rcss";
        const char* key = values.contains(kast::kGuiPrefabAttribute) ? kast::kGuiPrefabAttribute
                          : link && values.contains("href")         ? "href"
                                                                    : nullptr;
        if (!key)
            continue;
        const Value value = values[key], id = values[kGuiAssetIdAttribute];
        std::string path = link ? utf8((dir / fromUtf8(value.text)).lexically_normal()) : value.text;
        std::string newId = id.text;
        const std::string oldPath = path;
        edit(path, newId);
        if (path == oldPath && newId == id.text)
            continue;
        if (link && !dir.empty())
            path = utf8(fromUtf8(path).lexically_relative(dir));
        // From the back, so the earlier offsets stay right.
        if (id.at != std::string::npos && id.at > value.at)
            element.replace(id.at, id.size, newId);
        else if (id.at == std::string::npos && !newId.empty())
            element.insert(value.at + value.size + 1, std::format(" {}=\"{}\"", kGuiAssetIdAttribute, newId));
        element.replace(value.at, value.size, path);
        if (id.at != std::string::npos && id.at < value.at)
            element.replace(id.at, id.size, newId);
        out.append(last, (*it)[0].first).append(element);
        last = (*it)[0].second;
        changed = true;
    }
    if (changed)
        text = out.append(last, text.cend());
    return changed;
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

    // adopt: a meta-less file whose .meta another editor scanning the project is writing at the same time (its
    // temporary file in the way) takes that id.
    // ponytail: up to 10 tries 20 ms apart; enough for editors started together on one project (the parallel tests).
    const auto giveNewId = [&](const std::string& file, bool adopt) {
        const std::string id = newAssetId();
        const fs::path meta = metaFileOf(root / fromUtf8(file));
        for (int attempt = 0; attempt < (adopt ? 10 : 1); ++attempt) {
            if (attempt > 0) {
                Sleep(20);
                if (const std::optional<std::string> written = readAssetMeta(meta)) {
                    idOfFile[file] = *written;
                    return;
                }
            }
            if (writeAssetMeta(meta, id, error)) {
                idOfFile[file] = id;
                return;
            }
        }
        kast::log::error(std::format("Cannot write the asset id of '{}': {}", file, error));
    };
    for (const std::string& file : metaless)
        giveNewId(file, true);
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
            giveNewId(byAge[i].second, false);
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

std::string resolveAssetRef(const std::string& id, const std::string& path)
{
    const AssetIds& ids = assetIds();
    if (const std::optional<std::string> now = ids.pathOf(id)) {
        staleRefs += *now != path;
        return *now;
    }
    rememberedIds()[caseKey(path)] = id;
    if (!ids.root().empty() && !ids.idOf(path))
        kast::log::warn(std::format("Missing asset '{}' (id {}): no file has that id or path; the reference is kept", path, id));
    return path;
}

size_t staleAssetRefs() { return staleRefs; }

std::optional<std::string> assetRefId(const std::string& path)
{
    if (std::optional<std::string> id = assetIds().idOf(path))
        return id;
    const auto remembered = rememberedIds().find(caseKey(path));
    return remembered == rememberedIds().end() ? std::nullopt : std::optional(remembered->second);
}

void hookAssetRefs() { kast::setAssetRefHooks({.resolve = resolveAssetRef, .idOf = assetRefId}); }

bool editAssetRefs(nlohmann::ordered_json& json, const AssetRefEdit& edit)
{
    if (json.is_string() || kast::isAssetRef(json)) {
        const bool ref = json.is_object();
        std::string path = (ref ? json["path"] : json).get<std::string>();
        std::string id = ref ? json["id"].get<std::string>() : std::string();
        const std::string oldPath = path, oldId = id;
        edit(path, id);
        if (path == oldPath && id == oldId)
            return false;
        json = id.empty() ? nlohmann::ordered_json(path) : nlohmann::ordered_json{{"id", id}, {"path", path}};
        return true;
    }
    bool changed = false;
    if (json.is_structured())
        for (auto& element : json)
            changed = editAssetRefs(element, edit) || changed;
    return changed;
}

bool holdsAssetRefs(std::string_view path)
{
    const fs::path file = fromUtf8(path);
    const std::wstring extension = caseKey(utf8(file.extension()));
    if (extension == L".SCENE" || extension == L".PREFAB" || extension == L".MAT" || extension == L".RML" ||
        extension == L".GUIPREFAB" || caseKey(path) == caseKey(kast::kProjectFileName))
        return true;
    std::vector<std::wstring> parts;
    for (const fs::path& part : file)
        parts.push_back(caseKey(utf8(part)));
    return extension == L".JSON" && parts.size() >= 3 && parts[0] == L"MAPS" &&
           ((parts.size() == 3 && parts[2] == L"MAP.JSON") || (parts.size() == 4 && parts[2] == L"OBJECTS"));
}

std::optional<std::string> editAssetRefs(std::string_view path, std::string_view text, const AssetRefEdit& edit)
{
    const std::wstring extension = caseKey(utf8(fromUtf8(path).extension()));
    if (extension == L".RML" || extension == L".GUIPREFAB") {
        std::string out(text);
        return editGuiRefs(out, path, edit) ? std::optional(std::move(out)) : std::nullopt;
    }
    nlohmann::ordered_json json = nlohmann::ordered_json::parse(text, nullptr, false);
    if (json.is_discarded() || !editAssetRefs(json, edit))
        return std::nullopt;
    return json.dump(2) + "\n"; // the engine writers' format
}

bool remapAssetPath(std::string& path, const std::string& from, const std::string& to)
{
    const fs::path file = fromUtf8(path);
    if (path.empty() || (!file.has_extension() && isSameOrInside(fromUtf8(from), file)))
        return false;
    const std::optional<fs::path> moved = remapPath(file, fromUtf8(from), fromUtf8(to));
    if (!moved || utf8(*moved) == path)
        return false;
    path = utf8(*moved);
    return true;
}

bool remapAssetPaths(nlohmann::ordered_json& json, const std::string& from, const std::string& to)
{
    return editAssetRefs(json, [&](std::string& path, std::string&) { remapAssetPath(path, from, to); });
}

std::vector<std::string> remapAssetRefs(const fs::path& root, const std::string& from, const std::string& to,
                                        const AssetIds& ids)
{
    const AssetRefEdit edit = [&](std::string& path, std::string& id) {
        if (const std::optional<std::string> now = id.empty() ? std::nullopt : ids.pathOf(id))
            path = *now;
        else
            remapAssetPath(path, from, to);
    };
    std::vector<std::string> changed;
    for (const fs::path& file : kast::listFiles(root)) {
        const std::optional<std::string> path = kast::toProjectPath(root, file);
        std::string text, error;
        if (!path || hiddenPath(*path) || !holdsAssetRefs(*path) || !kast::readFile(file, text))
            continue;
        const std::optional<std::string> out = editAssetRefs(*path, text, edit);
        if (!out)
            continue;
        if (writeTextFile(file, *out, error))
            changed.push_back(*path);
        else
            kast::log::error(std::format("Cannot update the asset references in '{}': {}", *path, error));
    }
    return changed;
}
