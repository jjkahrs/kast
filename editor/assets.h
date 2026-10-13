#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Assets panel file logic, no UI: the project tree as the panel shows it and the file operations it offers. Names and
// project-relative paths are UTF-8; paths compare ignoring case, as Windows does.

struct AssetEntry {
    std::string name; // file or folder name
    std::string path; // project-relative, '/' separators
    bool folder = false;
    std::vector<AssetEntry> children; // folders only: folders first, then files, each A-Z ignoring case
};

constexpr const char* kNewFolderName = "New Folder";

std::string utf8(const std::filesystem::path& path);   // generic ('/') UTF-8 form
std::filesystem::path fromUtf8(std::string_view text); // the inverse

// What the Assets panel lists under root: everything except dot-entries (and their contents), project.kast at the
// root and the last Build output folder (loadBuildFolder) if it is inside root. Empty folders are listed.
std::vector<AssetEntry> scanAssets(const std::filesystem::path& root);
// The entry with this project-relative path, or null.
const AssetEntry* findAsset(const std::vector<AssetEntry>& entries, std::string_view path);

// Why name can't be an asset's new name; empty if it can. Refuses empty names, names starting with '.' (the panel hides
// them), invalid characters, a trailing space or '.', and reserved device names.
std::string assetNameError(std::string_view name);
// A name in dir equals name ignoring case.
bool assetNameTaken(const std::filesystem::path& dir, const std::filesystem::path& name);
// "New Folder", else "New Folder 2", "New Folder 3", ...: the first not taken in dir (ignoring case).
std::string uniqueFolderName(const std::filesystem::path& dir);
// path is ancestor or ancestor itself (absolute or both relative to the same place; compared ignoring case).
bool isSameOrInside(const std::filesystem::path& path, const std::filesystem::path& ancestor);
// Where file ends up when from is renamed or moved to to: to itself, or the same place under it. nullopt if file is
// not from or inside it.
std::optional<std::filesystem::path> remapPath(const std::filesystem::path& file, const std::filesystem::path& from,
                                               const std::filesystem::path& to);
// Rewrites each project-relative ('/') path in paths that is from or inside it to where remapPath puts it (from and to
// project-relative too). True if any changed.
bool remapProjectPaths(std::vector<std::string>& paths, const std::string& from, const std::string& to);
// Why target can't be deleted while openScene (the edited scene's file, maybe empty) is open; empty if it can.
std::string deleteBlocker(const std::filesystem::path& target, const std::filesystem::path& openScene);

// Creates folder name in dir. False + error (nothing created) if the name is invalid or taken.
bool createAssetFolder(const std::filesystem::path& dir, const std::string& name, std::string& error);
// Renames source in place. A name differing only in case is allowed. False + error (disk unchanged) if the name is
// invalid or taken (ignoring case) or the rename fails. Never overwrites.
bool renameAsset(const std::filesystem::path& source, const std::string& name, std::string& error);
// Moves source into folder destDir, keeping its name. False + error (disk unchanged) if destDir is source or inside
// it, the name is taken there (ignoring case) or the move fails. Never overwrites.
bool moveAsset(const std::filesystem::path& source, const std::filesystem::path& destDir, std::string& error);
// Sends target to the Recycle Bin, without a prompt unless Windows can't recycle it (it then asks before deleting it
// for good). False + error if that fails or is cancelled.
bool recycleAsset(const std::filesystem::path& target, std::string& error);
