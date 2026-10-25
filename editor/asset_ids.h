#pragma once

#include "assets.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
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

// Asset references ({"id", "path"} in project JSON, engine/asset_ref.h; in GUI markup the path attribute plus
// kGuiAssetIdAttribute) through the registry.

// Plugs assetIds() into kast::setAssetRefHooks: refs read go through resolveAssetRef, paths written get assetRefId.
void hookAssetRefs();
// The path to load for a ref read from a file: its id's registry path (if that isn't the hint, staleAssetRefs counts
// it), else the hint. A ref with neither id nor hint in the registry is missing: logged, kept as-is (its id remembered
// for assetRefId).
std::string resolveAssetRef(const std::string& id, const std::string& path);
// Refs read so far whose hint wasn't their id's path (a doc loaded with one is saved to write the fresh hint).
size_t staleAssetRefs();
// The id to write for a project-relative path: the registry's, else a missing ref's remembered by resolveAssetRef.
std::optional<std::string> assetRefId(const std::string& path);

// GUI markup's refs: data-prefab="<project path>" (a GUI prefab instance) and the href of <link type="text/rcss">
// (relative to the document), each with its asset id in this attribute beside it. RmlUi ignores it.
constexpr const char* kGuiAssetIdAttribute = "data-kast-id";

// Edits one ref: path (its hint, or a plain string's text) and id (empty: a plain string, a legacy ref or no ref at
// all). Setting an id on a plain string makes it a ref.
using AssetRefEdit = std::function<void(std::string& path, std::string& id)>;
// Applies edit to every ref and every other string value in json. True if anything changed.
bool editAssetRefs(nlohmann::ordered_json& json, const AssetRefEdit& edit);
// The project file at path (project-relative) holds refs: .scene, .prefab, .mat, project.kast, maps/<map>/map.json and
// objects/*.json (JSON), .rml and .guiprefab (GUI markup).
bool holdsAssetRefs(std::string_view path);
// text (the file at path, which holdsAssetRefs) with edit applied to its refs (GUI: only its refs; JSON: as above),
// rewritten in the writers' format (GUI: only the edited attributes). nullopt if nothing changed or it doesn't parse.
std::optional<std::string> editAssetRefs(std::string_view path, std::string_view text, const AssetRefEdit& edit);

// Where a path that was from or inside it is after a rename or move to to (project-relative; remapPath). A path equal
// to from moves only if it has an extension (from is a file then, not e.g. the folder "ui" matching the bus "UI"). True
// if path changed.
bool remapAssetPath(std::string& path, const std::string& from, const std::string& to);
// remapAssetPath on every ref and other string value in json: an open document's in-memory refs (the zone's map.json
// and objects) after a rename or move. True if anything changed.
bool remapAssetPaths(nlohmann::ordered_json& json, const std::string& from, const std::string& to);
// The rename / move pass, after from became to on disk and ids was rescanned: in every file under root that
// holdsAssetRefs, a ref whose id ids knows gets its path as the hint (the moved files, and a linked file that moved
// with them, e.g. renameGui's .rcss), any other ref or plain string remapAssetPath's. Returns the files changed (project-relative); a file that can't be written is logged.
std::vector<std::string> remapAssetRefs(const std::filesystem::path& root, const std::string& from, const std::string& to,
                                        const AssetIds& ids);
