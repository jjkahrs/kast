#pragma once

#include <material.h>
#include <undo.h>

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
// Every asset file's id lives in a sidecar "<file name>.meta" beside it (asset_ids.h); folders have none.
constexpr std::string_view kMetaExtension = ".meta";

// name ends in ".meta" (ignoring case): a sidecar, never an asset.
bool isMetaFile(const std::filesystem::path& name);
// file's sidecar: "<file>.meta".
std::filesystem::path metaFileOf(const std::filesystem::path& file);

std::string utf8(const std::filesystem::path& path);   // generic ('/') UTF-8 form
std::filesystem::path fromUtf8(std::string_view text); // the inverse

// What the Assets panel lists under root: everything except dot-entries (and their contents), project.kast at the
// root, .meta files and the last Build output folder (loadBuildFolder) if it is inside root. Empty folders are listed.
// metas, if given, gets the .meta files left out (from the listed folders only).
std::vector<AssetEntry> scanAssets(const std::filesystem::path& root,
                                   std::vector<std::filesystem::directory_entry>* metas = nullptr);
// The entry with this project-relative path, or null.
const AssetEntry* findAsset(const std::vector<AssetEntry>& entries, std::string_view path);

// Why name can't be an asset's new name; empty if it can. Refuses empty names, names starting with '.' or ending in
// ".meta" (the panel hides them), invalid characters, a trailing space or '.', and reserved device names.
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

// Writes text to file (creating its folder), replacing any file there. False + error if that fails.
bool writeTextFile(const std::filesystem::path& file, std::string_view text, std::string& error);
// Writes material to file as a .mat (2-space indented JSON, fixed key order).
bool saveMaterial(const std::filesystem::path& file, const kast::Material& material, std::string& error);
// Creates "new_material.mat" (else "new_material_2.mat", ...) with the default material in dir; file is where.
bool writeNewMaterial(const std::filesystem::path& dir, std::filesystem::path& file, std::string& error);
// Records on history an Inspector edit that saved after over before in the .mat file: undo and redo save that version
// again (logging a failure). Nothing is recorded if before and after are the same.
void pushMaterialStep(UndoStack& history, const std::filesystem::path& file, std::string name, const kast::Material& before,
                      const kast::Material& after);
// Creates "new_shader.kshader" (else "new_shader_2.kshader", ...) in dir: a commented starter with params, a
// fragment() and a commented-out vertex(). file is where.
bool writeNewShader(const std::filesystem::path& dir, std::filesystem::path& file, std::string& error);
// Creates "new_shader_graph.kshadergraph" (else "new_shader_graph_2.kshadergraph", ...) in dir: starterShaderGraph(), a
// Color param into a Fragment Output's ALBEDO. file is where.
bool writeNewShaderGraph(const std::filesystem::path& dir, std::filesystem::path& file, std::string& error);
// Rewrites the paths of every valid .mat under root that point at from or inside it (project-relative, as
// remapProjectPaths): its textures, texture2D param values and .kshader. Returns the .mat files changed
// (project-relative); a file that can't be written is logged.
std::vector<std::string> remapMaterialPaths(const std::filesystem::path& root, const std::string& from,
                                            const std::string& to);
// Creates folder name in dir. False + error (nothing created) if the name is invalid or taken.
bool createAssetFolder(const std::filesystem::path& dir, const std::string& name, std::string& error);
// Renames source in place, a file's .meta with it. A name differing only in case is allowed. False + error (disk
// unchanged) if the name is invalid or taken (ignoring case) or the rename fails. Never overwrites.
bool renameAsset(const std::filesystem::path& source, const std::string& name, std::string& error);
// Moves source into folder destDir, keeping its name, a file's .meta with it. False + error (disk unchanged) if
// destDir is source or inside it, the name is taken there (ignoring case) or the move fails. Never overwrites.
bool moveAsset(const std::filesystem::path& source, const std::filesystem::path& destDir, std::string& error);
// Sends target to the Recycle Bin (kast::recycleFile), a file's .meta with it. False + error if that fails.
bool recycleAsset(const std::filesystem::path& target, std::string& error);

// Prefabs (see prefab.h).
// "<name>.prefab", else "<name>_2.prefab", ...: the first not taken in dir (ignoring case). name is an entity name with
// characters a file name can't hold replaced by '_' ("Prefab" if nothing usable is left).
std::string uniquePrefabName(const std::filesystem::path& dir, const std::string& entityName);
// The .scene and .prefab files under root (project-relative, sorted) with an instance of a prefab at target or inside
// it (target: project-relative, a .prefab or a folder).
std::vector<std::string> prefabUsers(const std::filesystem::path& root, const std::string& target);
// Rewrites the prefab references in every .scene and .prefab under root that point at from or inside it (as
// remapProjectPaths). Returns the files changed (project-relative); a file that can't be written is logged.
std::vector<std::string> remapPrefabReferences(const std::filesystem::path& root, const std::string& from,
                                               const std::string& to);
