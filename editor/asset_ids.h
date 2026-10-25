#pragma once

#include "assets.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Stable asset ids (editor only): every asset file the Assets panel lists has a sidecar "<file name>.meta" beside it
// holding {"id": "<uuid>"} (2-space indented JSON, trailing newline). Folders, dot-entries, project.kast, the build
// folder and ".tmp" files (writeFileAtomic's temporaries) have none. The engine, player and pak never read them.

// A fresh lowercase UUIDv4 ("xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx", y in 8-b). Main thread only.
std::string newAssetId();
// text is a lowercase UUIDv4 as newAssetId writes it.
bool isAssetId(std::string_view text);
// The id in a .meta file, or nullopt if it can't be read or holds no valid id.
std::optional<std::string> readAssetMeta(const std::filesystem::path& meta);
// Writes {"id": id} to meta. False + error if that fails.
bool writeAssetMeta(const std::filesystem::path& meta, std::string_view id, std::string& error);

// A project's id <-> path maps, refreshed from its .meta files by scan. Paths are project-relative ('/'), looked up
// ignoring case.
class AssetIds {
public:
    // Rebuilds the maps for root from scanAssets(root, &metas), first fixing the disk (each fix is logged):
    // - a file without a valid .meta gets one with a fresh id;
    // - an orphan .meta (its file is gone) is adopted by the meta-less file of its folder with the same extension if
    //   both are the only such ones there (a rename in Explorer without the .meta); the rest go to the Recycle Bin;
    // - of files sharing an id (a file copied with its .meta), all but the oldest (creation or modification time,
    //   whichever is newer; ties: the first path A-Z) get a fresh id.
    void scan(const std::filesystem::path& root, const std::vector<AssetEntry>& entries,
              const std::vector<std::filesystem::directory_entry>& metas);
    // scan with root's own scanAssets.
    void scan(const std::filesystem::path& root);

    const std::filesystem::path& root() const { return root_; } // empty until the first scan
    // The project-relative path of the asset with this id, or nullopt.
    std::optional<std::string> pathOf(std::string_view id) const;
    // The id of the asset at this project-relative path (ignoring case), or nullopt.
    std::optional<std::string> idOf(std::string_view path) const;
    size_t size() const { return paths_.size(); }

private:
    struct CachedMeta {
        std::filesystem::file_time_type time;
        std::optional<std::string> id;
    };
    std::filesystem::path root_;
    std::unordered_map<std::string, std::string> paths_; // id -> path
    std::unordered_map<std::wstring, std::string> ids_;  // path, upper-cased -> id
    // ponytail: .meta reads cached by modification time; every scan still lists every folder (as scanAssets does).
    std::unordered_map<std::wstring, CachedMeta> metaCache_; // .meta file -> what it held
};

// The editor's registry: the open project's, scanned on project open and every Assets panel rescan.
AssetIds& assetIds();
