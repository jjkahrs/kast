// kast_editor's zone edit flags (the Zone Builder's command line), for tests and scripts: --map or
// --new-map opens a map of the project into the ZoneSession, then edits run in command-line order (one undo step each)
// and --save-zone writes it. They run before the editor window opens, which then shows that map as the scene's zone
// (main.cpp points the Terrain entity at it), or with --headless instead of the window.
// Syntax: docs/editor/command-line.html#zone; the forms are in kUsage below.
#pragma once

#include "camera.h"
#include "zone_session.h"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zone_cli {

constexpr const char* kUsage =
    "Zone edits (need --project or --scene), run in order: --map folder | --new-map folder[,W,H[,id]] --falloff F "
    "--sculpt x,z,radius,metres (negative lowers) --flatten x,z,radius,height[,fraction] --height-range min,max "
    "--map-id N --material path --instance maxPlayers,emptyTimeoutSec,exitMap,x,y,z[,heading] --world-map "
    "--import-png tx,tz,file --export-png tx,tz,file "
    "--add-area id,priority,x0,z0,x1,z1 --add-polygon id,priority,x,z,x,z,x,z... --area-parent id,parent "
    "--area-portal id,map,x,y,z,heading --move-area-vertex id,i,x,z --delete-area id "
    "--add-spawn id,npc,x,z[,heading[,respawnSec]] --move-spawn id,x,z --delete-spawn id "
    "--add-path spawnId,wait,loop,x,z,x,z... --add-tile tx,tz --remove-tile tx,tz --set-spawn x,z[,heading] "
    "--add-object type,prefab,x,z[,yaw[,scale[,box,hx,hy,hz|cylinder,r,h]]] --set-interactable id,range,event[,name[,data]] "
    "--move-object tx,tz,i,x,z --delete-object tx,tz,i --scatter x,z,radius,count,seed,scaleMin,scaleMax,prefab[+prefab] "
    "--erase-visuals x,z,radius --generate-heights seed[,scale,octaves,persistence,lacunarity] "
    "--terrain-colors pct,r,g,b,pct,r,g,b... --create-terrain-material --save-zone --validate --expect-save-fail --undo "
    "--redo. Then: [--headless] [--no-recycle] [--focus x,z,distance] [--sculpt-loop] [--prompt-remove-tile tx,tz]";

// One zone edit flag and its value (empty for flags without one).
struct Op {
    std::string flag, value;
};

// Whether arg is a zone edit flag that takes a value (true) or none (false); nullopt: not one.
std::optional<bool> takesValue(std::string_view arg);
// Runs ops in order on zone (whose project must be open: ZoneSession::openProject). False (logged) on the first that
// fails: the run exits 1.
bool run(ZoneSession& zone, const std::vector<Op>& ops);
// --headless --bake: Bake Navmesh on the open zone, waiting for kast_mapbake (its output to the log). The exit code
// for the run: 0 when it exited 0.
int bakeHeadless(ZoneSession& zone, const std::filesystem::path& settingsFile, const std::string& mapbake);
// "x,z,distance": --focus's camera pivot (on the ground) and distance; nullopt if malformed.
std::optional<std::array<float, 3>> parseFocus(std::string_view text);
// "tx,tz": --prompt-remove-tile's tile; nullopt if malformed.
std::optional<std::array<int, 2>> parseTile(std::string_view text);
// The ground height at (x, z) of zone (0 off the map or with no zone), for --focus.
float groundY(const zone::ZoneDoc* zone, float x, float z);
// Points camera at the whole of zone's tiles (a map past 9 x 9: the ones the Scene view meshes around its centre), as the
// window opens on a map the zone flags opened.
void frameZone(EditorCamera& camera, const zone::ZoneDoc& zone);
// One frame of --sculpt-loop (the --log-fps sculpting benchmark): a brush circling the map's centre across tile
// borders, raising for 2 s then lowering for 2 s, straight on the state, outside undo.
void sculptLoop(zone::ZoneDoc& zone, int frame);

} // namespace zone_cli
