#pragma once

#include "audio.h"
#include "auth_client.h"
#include "chat_client.h"
#include "event_bus.h"
#include "display_access.h"
#include "gui_access.h"
#include "input.h"
#include "network.h"
#include "physics.h"
#include "scene.h"
#include "state_machines.h"
#include "terrain.h"
#include "replication.h"
#include "world_client.h"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

// Lua systems: global ECS systems written in Lua, listed in Scene::systems and run once per frame in Play mode after
// the C++ systems. One LuaJIT state per Play session; each file runs in its own sandboxed environment. See
// docs/engine/lua-systems.html for the file shape and the kast.* API.
namespace kast {

struct LuaSystemStatus {
    std::string path;  // as listed in Scene::systems
    bool ok = false;   // false: disabled until the next start() or a successful hot reload
    std::string error; // last error, empty when ok
};

class LuaSystems {
public:
    LuaSystems();
    ~LuaSystems();
    LuaSystems(const LuaSystems&) = delete;
    LuaSystems& operator=(const LuaSystems&) = delete;

    // Starts a session: resets the event bus (queue, subscriptions, history) and the state machines, closes every
    // network connection and request, loads the project's state machine definitions (fsms/) and every script in
    // scene.systems (paths relative to projectRoot), calls each init, then enters gameMachines (project.kast
    // "stateMachines") and every StateMachine entity. With no project every listed system is disabled ("no
    // project"). With several loaded scenes (additive scenes) every one's systems run, each path once. scenePath is
    // the active scene's project-relative path, empty if unsaved or outside a project (kast.currentScene returns nil);
    // the others' come from their LoadedScene paths. Stops any running session first
    // (without its shutdowns: the old scene may be gone). scene and input must outlive the session; input may be
    // null (no keys down, no mouse motion). auth: the project's Online Services auth endpoint (authConfig in auth_client.h);
    // nullopt leaves kast.auth and kast.world nil. The session starts logged out.
    void start(Scene& scene, const std::optional<std::filesystem::path>& projectRoot, const std::string& scenePath,
               Input* input, std::vector<std::string> gameMachines = {},
               std::optional<AuthConfig> auth = std::nullopt);
    // Publishes the network results received since the last tick, reloads scripts and state machine definitions whose
    // file changed, enters the machines of entities that gained StateMachine, then calls each enabled system's
    // update(dt) in listed order, then drains the event bus. Then applies the frame's kast.loadScene / unloadScene /
    // setActiveScene calls in call order. A switch (kast.loadScene without additive): the old systems' shutdowns
    // run, queued events are dropped, the loaded scene replaces scene (same object) and a new session starts for its
    // systems (C++ subscriptions, the bus history, the game state machines' states and the network connections are
    // kept). An additive load appends the scene (Scene::loadedScenes) and inits its systems not already running; an
    // unload shuts down the systems no other loaded scene lists and destroys its entities; see
    // docs/engine/additive-scenes.html. Then (unless switched) streams tile scenes (tile_scenes.h) around the tiles
    // terrain() streamed this frame, through the same additive load and unload: out-of-range ones unload, at most one
    // in-range one loads, each publishing map.tileScene.loaded / unloaded {map, tx, tz, path} on bus(). A tile scene
    // that fails to load logs one error and is skipped for the session. Returns true if the scene was switched.
    bool tick(float dt);
    // Bumped by every tick that applied scene requests (a switch, additive loads and unloads, active scene changes) or
    // streamed a tile scene in or out:
    // entity handles held across it may be stale.
    uint32_t sceneChanges() const { return sceneChanges_; }
    // Calls each enabled system's shutdown, ends the session, resets the state machines, drops queued events and
    // closes every network connection and request. Does nothing if none is running.
    void stop();

    bool running() const { return state_ != nullptr; }
    // One entry per running system (every loaded scene's Scene::systems, each path once) while running; empty otherwise.
    std::vector<LuaSystemStatus> status() const;
    // The running active scene's project-relative path; empty if unsaved, outside a project or no session is running.
    std::string scenePath() const;

    // The session's event bus, also for C++ publishers and subscribers. Its history outlives stop().
    EventBus& bus() { return bus_; }
    const EventBus& bus() const { return bus_; }
    // The session's physics world (kast.physics.* from C++): started with the session (and on a scene switch),
    // stopped with it. tickPlay steps it.
    Physics& physics() { return physics_; }
    const Physics& physics() const { return physics_; }
    // The session's state machines (kast.fsm.* from C++). Usable while running; reset by start() and stop().
    // The session's sound (kast.audio.* from C++): start() stops every voice and resets the buses to 1.0 (apply the
    // project's defaults after it); a scene switch and stop() stop every voice; AudioSources with playOnStart start
    // with each scene. tick() frees ended voices.
    Audio& audio() { return audio_; }
    const Audio& audio() const { return audio_; }
    // The session's connections and HTTP requests (kast.net.* from C++, the editor's Network panel): kept across
    // kast.loadScene switches; start(), stop() and destruction close them all. tick() publishes their results on
    // bus() before the systems' updates.
    Network& network() { return network_; }
    const Network& network() const { return network_; }
    // The session's auth client (kast.auth.* from C++): its requests run on network(), its "auth.*" events are
    // published on bus() by tick(). Kept across kast.loadScene switches; start() and stop() log it out (memory only).
    AuthClient& auth() { return auth_; }
    const AuthClient& auth() const { return auth_; }
    // The session's world server client (kast.world.* from C++): enabled with auth, its connection runs on network(),
    // its "world.*" events are published on bus() by tick(). Kept across kast.loadScene switches; start() and stop()
    // drop the connection. Its address override (editor only) outlives them.
    WorldClient& world() { return world_; }
    const WorldClient& world() const { return world_; }
    // The session's chat service client (kast.chat.* from C++): enabled with auth, it follows the world connection
    // (connects where world.chatServer says, ends with world.disconnected); its "chat.*" events are published on bus()
    // by tick(). Kept across kast.loadScene switches.
    ChatClient& chat() { return chat_; }
    const ChatClient& chat() const { return chat_; }
    // The session's entity replication (docs/engine/online-services.html#replication): started with each scene before
    // physics and the inits (it takes the template entity out), updated by tick() after the systems.
    Replication& replication() { return replication_; }
    // The session's terrain streaming (docs/engine/terrain.html): its maps load with start(), tickPlay updates it before
    // physics so new tiles collide on the step they appear.
    TerrainStreamer& terrain() { return terrain_; }
    StateMachines& fsm() { return fsm_; }
    const StateMachines& fsm() const { return fsm_; }
    // The scene's game GUIs for kast.gui.* (the editor's Game view, kast_player's window; must outlive the session),
    // reset() before each scene's systems start. Null (the default) leaves kast.gui nil. Takes effect at the next
    // start or scene switch.
    void setGui(GuiAccess* gui) { gui_ = gui; }
    // The game's window for kast.display.* (kast_player's; the editor's Game view simulates one); null (the default)
    // leaves kast.display nil. Takes effect like setGui.
    void setDisplay(DisplayAccess* display) { display_ = display; }

private:
    struct State;
    void launch(Scene& scene, const std::optional<std::filesystem::path>& projectRoot, const std::string& scenePath,
                Input* input);
    void endSession(); // shutdowns, then the Lua state goes; the state machines keep their game states
    bool streamTileScenes(); // true if it loaded or unloaded one

    // Tile scene streaming, for the session (start() clears them).
    // ponytail: each map's scenes/ listed once per session; a tile scene saved during Play streams from the next one.
    std::map<uint16_t, std::vector<map::TileKey>> tileSceneFiles_;
    std::set<std::string> failedTileScenes_; // logged once, never retried

    EventBus bus_; // declared before state_: the systems' subscriptions unsubscribe from it when state_ goes
    Physics physics_; // publishes on bus_
    TerrainStreamer terrain_;
    Audio audio_;
    Network network_;
    AuthClient auth_{network_};
    WorldClient world_{network_, auth_};
    ChatClient chat_{network_, auth_};
    Replication replication_; // subscribes on bus_, sends through world_
    GuiAccess* gui_ = nullptr;
    DisplayAccess* display_ = nullptr;
    std::unique_ptr<State> state_;
    uint32_t sceneChanges_ = 0;
    StateMachines fsm_{bus_}; // declared after state_, so its Lua callbacks are released while the Lua state lives
};

} // namespace kast
