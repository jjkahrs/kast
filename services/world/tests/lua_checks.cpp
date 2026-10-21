// Assert-style checks for the Lua layer: component definitions (every type, option and error), reserved and duplicate
// names, the sandbox, kast.* semantics, hooks, error handling, persistence + drift, component replication routing, the
// event bus (kast.publish/subscribe/unsubscribe), client events (kast.sendTo/sendNearby/broadcast) and state machines
// (definitions, rules, queueing, callback errors, kast.fsm, the StateMachine built-in, C++ / Lua parity) and NPCs
// (kast.create / kast.destroy, Transform rules, AOI and replication routing, shutdown, the sample lua/), NPC spawns
// (parse_spawns, spawn file loading errors, respawn timing, MAX_NPCS, the Spawn built-in) and maps (tile math,
// manifests and Maps::load errors, bounds, per-map AOI, World::transfer, kast.transfer / Transform.map), ground and the
// navmesh (an in-memory bake, Nav::path / on_mesh, navtile load errors, off_mesh moves, kast.map.path).
// Lua files are written to a temp folder; the World runs with captured Send/Save, so there is no network or database.
#include "core/log.h"
#include "lua/components.h"
#include "world/components.h"
#include "world/maps.h"
#include "world/world.h"
#include <bake.h>
#include <heights.h>
#include <navtile.h>

#include <codec.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <map>
#include <numbers>
#include <random>
#include <set>
#include <source_location>

using namespace kast;
namespace fs = std::filesystem;
using Json = nlohmann::ordered_json;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const std::string& what, std::source_location where = std::source_location::current()) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::printf("FAILED lua_checks.cpp line %u: %s\n", static_cast<unsigned>(where.line()), what.c_str());
}

const TimePoint T0{std::chrono::seconds(1000)};

// A LUA_ROOT in the temp folder, removed afterwards.
struct LuaRoot {
    fs::path root;
    LuaRoot() {
        root = fs::temp_directory_path() / ("kast_lua_check_" + std::to_string(std::random_device{}()));
        fs::create_directories(root);
    }
    ~LuaRoot() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    LuaRoot& write(const std::string& rel, const std::string& text) {
        fs::create_directories((root / rel).parent_path());
        std::ofstream(root / rel, std::ios::binary) << text;
        return *this;
    }
    lua::DefsResult defs() const { return lua::load_component_defs(root, world::kReservedComponentNames); }
};

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

// The single error for a definitions folder holding one file with this body.
std::string def_error(const std::string& body, const std::string& file = "components/c.lua") {
    LuaRoot r;
    r.write(file, body);
    const auto d = r.defs();
    return d.errors.size() == 1 && d.defs.empty() ? d.errors[0] : "<" + std::to_string(d.errors.size()) + " errors>";
}

std::string def_with_field(const std::string& field) { return "return {name = 'C', fields = {" + field + "}}"; }

// --- definitions ---

void checkDefinitionParsing() {
    LuaRoot r;
    r.write("components/sub/health.lua", R"(return {
        name = "Health", displayName = "Hit Points", persist = true, replicate = "aoi",
        fields = {
            { name = "current", type = "float", default = 100, min = 0 },
            { name = "lives", type = "int", default = 3, min = 0, max = 9 },
            { name = "regen", type = "bool", default = true },
            { name = "title", type = "string", default = "Hero" },
            { name = "tint", type = "vec3", color = true, default = {x = 1, y = 0, z = 0} },
            { name = "facing", type = "quat" },
            { name = "faction", type = "enum", values = {"neutral", "ally"}, default = "ally" },
            { name = "drops", type = "array", of = "string", default = {"gold", "potion"} },
            { name = "low", type = "float", min = 5 },
        } })");
    r.write("components/wallet.lua", "return { name = 'Wallet', replicate = 'owner', fields = {} }");
    r.write("components/plain.lua", "return { name = 'Plain', persist = false, replicate = 'none', fields = {} }");
    r.write("components/notes.txt", "not lua");
    const auto d = r.defs();
    check(d.errors.empty(), "valid definitions load: " + (d.errors.empty() ? "" : d.errors[0]));
    check(d.defs.size() == 3, "three definitions (recursive scan, .lua only)");
    if (d.defs.size() != 3) return;
    const auto& h = *d.defs[0];
    check(h.name == "Health" && h.displayName == "Hit Points" && h.path == "components/sub/health.lua", "name/displayName/path");
    check(h.persist && h.replicate == lua::Replicate::aoi, "persist + replicate aoi");
    check(d.defs[1]->name == "Plain" && !d.defs[1]->persist && d.defs[1]->replicate == lua::Replicate::none, "explicit defaults");
    check(d.defs[2]->name == "Wallet" && !d.defs[2]->persist && d.defs[2]->replicate == lua::Replicate::owner,
          "persist defaults to false; replicate owner");
    const Json def = lua::default_component(h);
    check(def.dump() == R"({"current":100.0,"lives":3,"regen":true,"title":"Hero","tint":[1.0,0.0,0.0],)"
                        R"("facing":[0.0,0.0,0.0,1.0],"faction":"ally","drops":["gold","potion"],"low":5.0})",
          "default instance in field order: " + def.dump());
    check(h.fields[4].color && h.fields[7].array && h.fields[7].kind == lua::FieldKind::String, "color, array of");
}

void checkDefinitionErrors() {
    const std::pair<std::string, std::string> cases[] = {
        {"return 5", "components/c.lua: must return exactly one table {name = ..., fields = {...}}"},
        {"return {name='C', fields={}}, 2", "must return exactly one table"},
        {"return setmetatable({name='C', fields={}}, {})", "components/c.lua: tables must be plain data (no metatables)"},
        {"return {name='9C', fields={}}", "components/c.lua: \"name\" must be a name (letters, digits, _; not starting with a digit)"},
        {"return {fields={}}", "\"name\" must be a name"},
        {"return {name='C', fields={}, colour=1}", "components/c.lua: unknown key colour"},
        {"return {name='C', displayName='', fields={}}", "components/c.lua: \"displayName\" must be a non-empty string"},
        {"return {name='C', fields=5}", "components/c.lua: \"fields\" must be a list {...}"},
        {"return {name='C', fields={[2]={name='a', type='int'}}}", "\"fields\" must be a list {...}"},
        {"return {name='C', persist='yes', fields={}}", "components/c.lua: \"persist\" must be true or false"},
        {"return {name='C', replicate='all', fields={}}", "components/c.lua: \"replicate\" must be \"none\", \"owner\" or \"aoi\", got all"},
        {"return {name='C', replicate=true, fields={}}", "\"replicate\" must be \"none\", \"owner\" or \"aoi\", got true"},
        {def_with_field("5"), "components/c.lua: fields[1]: must be a table {name = ..., type = ...}"},
        {def_with_field("{name='1a', type='int'}"), "fields[1]: \"name\" must be a name"},
        {def_with_field("{name='a', type='int'}, {name='a', type='int'}"), "fields[2] (\"a\"): duplicate field name"},
        {def_with_field("{name='a', type='int', step=1}"), "fields[1] (\"a\"): unknown key step"},
        {def_with_field("{name='a', type='double'}"), "fields[1] (\"a\"): unknown \"type\" double (expected one of float, int, bool, string, vec3, quat, enum, array)"},
        {def_with_field("{name='a', type='array', of='array'}"), "unknown \"of\" array (expected one of float, int, bool, string, vec3, quat, enum; arrays can't nest)"},
        {def_with_field("{name='a', type='int', of='int'}"), "\"of\" is only allowed on array fields"},
        {def_with_field("{name='a', type='enum'}"), "fields[1] (\"a\"): \"values\" must be a list {...}"},
        {def_with_field("{name='a', type='enum', values={}}"), "\"values\" must not be empty"},
        {def_with_field("{name='a', type='enum', values={'x', 'x'}}"), "duplicate value \"x\" in \"values\""},
        {def_with_field("{name='a', type='enum', values={''}}"), "\"values\" must be non-empty strings"},
        {def_with_field("{name='a', type='int', values={'x'}}"), "\"values\" is only allowed on enum fields"},
        {def_with_field("{name='a', type='bool', min=0}"), "\"min\" is only allowed on float and int fields"},
        {def_with_field("{name='a', type='float', max='9'}"), "\"max\" must be a finite number"},
        {def_with_field("{name='a', type='int', min=0.5}"), "\"min\" must be an integer on an int field"},
        {def_with_field("{name='a', type='float', min=2, max=1}"), "\"min\" must not be greater than \"max\""},
        {def_with_field("{name='a', type='float', color=true}"), "\"color\" must be true or false, on vec3 fields only"},
        {def_with_field("{name='a', type='int', default='x'}"), "fields[1] (\"a\"): default \"x\" must be an integer"},
        {def_with_field("{name='a', type='int', default=20, max=10}"), "default 20 must be <= 10"},
        {def_with_field("{name='a', type='vec3', default=5}"), "default: \"a\" must be a table {x=, y=, z=} of finite numbers"},
        {def_with_field("{name='a', type='enum', values={'x'}, default='y'}"), "unknown value \"y\" (expected one of \"x\")"},
        {def_with_field("{name='a', type='array', of='int', default={1, 'b'}}"), "default [1,\"b\"] element 2 must be an integer"},
        {"return {name='C', fields={}", "components/c.lua:1:"},       // syntax error, with line
        {"\n\nerror('boom')", "components/c.lua:3: boom"},             // runtime error, with line
        {"while true do end", "script ran longer than 2 seconds"},     // watchdog
    };
    for (const auto& [body, expected] : cases) {
        const std::string error = def_error(body);
        check(contains(error, expected), "definition error: want [" + expected + "] got [" + error + "]");
    }
}

void checkNameRules() {
    for (const char* reserved : {"Character", "Transform", "StateMachine", "Npc", "Viewers", "Dirty", "SessionLink", "Visible",
                                 "StateMachineRuntime"}) {
        const std::string e = def_error(std::string("return {name='") + reserved + "', fields={}}");
        check(e == std::string("components/c.lua: name \"") + reserved + "\" is taken by a built-in component",
              "reserved name " + std::string(reserved) + ": " + e);
    }
    LuaRoot r;
    r.write("components/a.lua", "return {name='Dup', fields={}}");
    r.write("components/z/b.lua", "return {name='Dup', fields={}}");
    r.write("components/ok.lua", "return {name='Ok', fields={}}");
    r.write("components/bad.lua", "return 1");
    const auto d = r.defs();
    check(d.defs.size() == 1 && d.defs[0]->name == "Ok", "only the unique valid definition loads");
    check(d.errors.size() == 3, "every error is reported, not just the first");
    const auto has = [&](const std::string& text) {
        return std::ranges::any_of(d.errors, [&](const std::string& e) { return e == text; });
    };
    check(has("components/a.lua: name \"Dup\" is also declared in components/z/b.lua"), "dup names the other file (a)");
    check(has("components/z/b.lua: name \"Dup\" is also declared in components/a.lua"), "dup names the other file (b)");

    LuaRoot none;
    check(none.defs().defs.empty() && none.defs().errors.empty(), "no components/ folder: no definitions, no errors");
}

void checkSandbox() {
    for (const char* escape : {"io.open('x')", "os.exit(1)", "ffi.cdef('')", "debug.getinfo(1)", "package.loaded",
                               "require('x')", "loadfile('x')", "dofile('x')", "load('return 1')", "loadstring('x')",
                               "jit.on()", "setfenv(1, {})", "getmetatable('').__index.rep = nil"}) {
        const std::string e = def_error(std::string("local x = ") + escape + "\nreturn {name='C', fields={}}");
        check(contains(e, "components/c.lua:1:"), std::string("definition can't reach ") + escape + ": " + e);
    }
    check(def_error("x = string.rep('a', 3) .. math.floor(2.5) .. table.concat({'b'})\nreturn {name='C', fields={}}") ==
              "<0 errors>",
          "base/table/string/math work");
}

// --- systems ---

struct Sent {
    std::map<uint32_t, std::vector<proto::Message>> bySession;
    std::vector<db::CharSave> saves;
    void clear() { bySession.clear(); saves.clear(); }

    template <typename T>
    std::vector<T> of(uint32_t sid) const {
        std::vector<T> out;
        if (auto it = bySession.find(sid); it != bySession.end())
            for (const auto& m : it->second)
                if (auto* t = std::get_if<T>(&m)) out.push_back(*t);
        return out;
    }
    // Component updates for entity on sid, by name.
    std::vector<proto::ComponentUpdate> updates(uint32_t sid, uint32_t entity, const std::string& name = "") const {
        std::vector<proto::ComponentUpdate> out;
        for (const auto& u : of<proto::ComponentUpdate>(sid))
            if (u.entityId == entity && (name.empty() || u.name == name)) out.push_back(u);
        return out;
    }
};

// Map 1 "start": tiles 0..7 (the old 2048 m zone), spawn (1024, 0, 1024). Map 2 "cave": tiles 0..1, spawn
// (256, 0, 256).
world::Maps test_maps() {
    world::Maps maps;
    map::MapInfo start;
    start.id = 1;
    start.name = "start";
    start.tiles = {{0, 0, 7, 7}};
    start.spawn = {1024, 0, 1024};
    maps.add(start);
    map::MapInfo cave;
    cave.id = 2;
    cave.name = "cave";
    cave.tiles = {{0, 0, 1, 1}};
    cave.spawn = {256, 0, 256};
    cave.spawnHeading = 1;
    maps.add(cave);
    return maps;
}

struct TestWorld {
    Sent sent;
    world::Maps maps; // before world: it holds a reference
    world::World world;
    explicit TestWorld(lua::Defs defs, world::WorldRules rules = {}, world::Maps m = test_maps())
        : maps(std::move(m)), world(rules, maps, [this](uint32_t sid, const proto::Message& m) { sent.bySession[sid].push_back(m); },
                [this](const db::CharSave& s) { sent.saves.push_back(s); }, std::move(defs)) {}
    entt::entity spawn(uint32_t sid, uint64_t charId, const std::string& name, float x, float z,
                       const std::string& components = "", uint16_t map = 1, proto::CharOptions options = {}) {
        db::CharRow row{charId, 1, name, map, {x, 0, z}, 0.5f, components, std::move(options)};
        return world.spawn(sid, row, T0);
    }
    // A Move of e's current epoch, as from a client that has heard every Correction so far.
    void move(entt::entity e, proto::Move m) {
        m.epoch = world.registry().get<world::MoveState>(e).epoch;
        world.push_move(e, m);
    }
    Json value(entt::entity e, const std::string& name) {
        auto& p = lua::pool(world.registry(), name);
        return p.contains(e) ? p.get(e).value : Json();
    }
};

uint32_t wire(entt::entity e) { return entt::to_integral(e); }

constexpr const char* kStats = R"(return { name = "Stats", fields = {
    { name = "hp", type = "int", default = 10, min = 0, max = 100 },
    { name = "flag", type = "bool" },
    { name = "kind", type = "enum", values = {"a", "b"}, default = "b" },
    { name = "pos", type = "vec3", default = {x = 1, y = 2, z = 3} },
    { name = "tags", type = "array", of = "string", default = {"a"} },
} })";
constexpr const char* kReport = R"(return { name = "Report", fields = { { name = "lines", type = "array", of = "string" } } })";

// Lua-side asserts: each failure becomes a line of the Report component on the last character.
constexpr const char* kApiSystem = R"(
local fails = {}
local function expect(ok, what) if not ok then fails[#fails + 1] = what end end
local function raises(want, f, ...)
    local ok, err = pcall(f, ...)
    if ok then fails[#fails + 1] = "no error, wanted: " .. want
    elseif not string.find(err, want, 1, true) then fails[#fails + 1] = "got [" .. tostring(err) .. "] wanted [" .. want .. "]" end
end
local done = false
leak = "from api"
return { update = function(dt)
    if done then return end
    done = true
    local ids = {}
    for e in kast.query("Character", "Transform") do ids[#ids + 1] = e end
    expect(#ids == 2 and ids[1] < ids[2], "query finds both characters in wire id order")
    local e = ids[1]
    local ch = kast.get(e, "Character")
    expect(ch.name == "Alice" and ch.charId == 11, "Character {charId, name}")
    local tf = kast.get(e, "Transform")
    expect(tf.position.x == 1000 and tf.position.y == 0 and tf.position.z == 1000 and tf.heading == 0.5, "Transform")
    expect(kast.has(e, "Character") and kast.has(e, "Transform"), "built-ins always present")
    raises('"Transform" is read-only on the server', kast.set, e, "Transform", {heading = 1})
    raises('"Character" is read-only on the server', kast.add, e, "Character")
    raises('"Transform" cannot be removed', kast.remove, e, "Transform")
    raises('unknown component "Nope"', kast.get, e, "Nope")
    raises('unknown component "Nope"', kast.query, "Nope")
    raises('component name must be a string, got 5', kast.query, 5)
    raises('invalid entity id 999999', kast.get, 999999, "Character")
    raises('invalid entity id 1.5', kast.has, 1.5, "Character")
    raises('invalid entity id x', kast.add, "x", "Stats")
    expect(not kast.has(e, "Stats"), "no Stats yet")
    raises('entity ' .. e .. ' has no "Stats"', kast.get, e, "Stats")
    raises('entity ' .. e .. ' has no "Stats"', kast.set, e, "Stats", {hp = 1})
    kast.add(e, "Stats")
    kast.add(e, "Stats") -- already there: nothing happens
    local s = kast.get(e, "Stats")
    expect(s.hp == 10 and s.flag == false and s.kind == "b" and s.pos.x == 1 and s.pos.z == 3 and s.tags[1] == "a" and #s.tags == 1, "defaults")
    kast.set(e, "Stats", {hp = 20, tags = {"x", "y"}})
    s = kast.get(e, "Stats")
    expect(s.hp == 20 and s.kind == "b" and s.tags[2] == "y", "partial set keeps the other fields")
    raises('"Stats": unknown field "nope"', kast.set, e, "Stats", {hp = 30, nope = 1})
    raises('"Stats": field names must be strings, got 1', kast.set, e, "Stats", {5})
    raises('"Stats": "hp" must be <= 100', kast.set, e, "Stats", {hp = 101})
    raises('"Stats": "hp" must be >= 0', kast.set, e, "Stats", {hp = -1})
    raises('"Stats": "hp" must be an integer', kast.set, e, "Stats", {hp = 1.5})
    raises('"Stats": "kind" unknown value "zzz" (expected one of "a", "b")', kast.set, e, "Stats", {kind = "zzz"})
    raises('"pos" must be a table {x=, y=, z=} of finite numbers', kast.set, e, "Stats", {hp = 30, pos = 5})
    raises('"tags" must be a sequence table {v1, v2, ...}', kast.set, e, "Stats", {tags = {x = 1}})
    raises('"Stats": "tags" element 2 must be a string', kast.set, e, "Stats", {hp = 30, tags = {"a", 3}})
    raises('"Stats": "flag" must be true or false', kast.set, e, "Stats", {hp = 50, flag = "x"})
    raises('"hp" must be a finite number', kast.set, e, "Stats", {hp = 1 / 0})
    s = kast.get(e, "Stats")
    expect(s.hp == 20 and s.flag == false and s.kind == "b" and s.tags[1] == "x" and s.pos.x == 1, "every failed set left Stats unchanged")
    kast.remove(e, "Stats")
    kast.remove(e, "Stats") -- not there: nothing happens
    expect(not kast.has(e, "Stats"), "removed")
    local n = 0
    for _ in kast.query("Stats") do n = n + 1 end
    expect(n == 0, "query Stats after remove")
    raises("kast is read-only", function() kast.get = nil end)
    expect(io == nil and os == nil and ffi == nil and debug == nil and package == nil and jit == nil and require == nil
        and loadfile == nil and dofile == nil and load == nil and loadstring == nil, "systems are sandboxed")
    kast.add(ids[2], "Report")
    kast.set(ids[2], "Report", {lines = fails})
end })";

constexpr const char* kIsolationSystem = R"(
local seen = leak
return { update = function() if seen ~= nil then kast.log("leak") ; error("globals leaked between systems") end end })";

void checkKastApi() {
    LuaRoot r;
    r.write("components/stats.lua", kStats).write("components/report.lua", kReport);
    r.write("systems/a_api.lua", kApiSystem).write("systems/b_isolation.lua", kIsolationSystem);
    TestWorld w(r.defs().defs);
    const auto errors = w.world.load_scripts(r.root);
    check(errors.empty(), "API systems load" + (errors.empty() ? "" : ": " + errors[0]));
    w.spawn(1, 11, "Alice", 1000, 1000);
    const auto bob = w.spawn(2, 12, "Bob", 1010, 1000);
    w.world.tick(T0);
    const Json report = w.value(bob, "Report");
    check(report.is_object(), "API system wrote its report");
    for (const auto& line : report.value("lines", Json::array())) check(false, "kast.*: " + line.get<std::string>());
    for (const auto& s : w.world.script_status()) check(s.ok, "system still enabled: " + s.path + " " + s.error);
}

// Hook order: init, onEnter (after the persisted components are restored), update, onLeave (before the save), and
// on shutdown onLeave for everyone, then shutdown, then the saves.
void checkHooks() {
    LuaRoot r;
    r.write("components/health.lua", R"(return { name = "Health", persist = true, replicate = "aoi",
        fields = { { name = "current", type = "float", default = 100 } } })");
    r.write("components/report.lua", kReport);
    r.write("systems/hooks.lua", R"(
local events = {}
local function note(s) events[#events + 1] = s end
return {
    init = function() note("init") ; for _ in kast.query("Character") do note("char at init") end end,
    onEnter = function(e)
        note("enter " .. kast.get(e, "Character").name .. " health " .. (kast.has(e, "Health") and kast.get(e, "Health").current or "none"))
        if not kast.has(e, "Report") then kast.add(e, "Report") end
    end,
    update = function(dt)
        note("update " .. dt)
        for e in kast.query("Report") do kast.set(e, "Report", {lines = events}) end
    end,
    onLeave = function(e) kast.set(e, "Health", {current = 42}) end,
    shutdown = function() for e in kast.query("Health") do kast.set(e, "Health", {current = kast.get(e, "Health").current + 1}) end end,
})");
    TestWorld w(r.defs().defs);
    check(w.world.load_scripts(r.root).empty(), "hooks system loads");
    const auto a = w.spawn(1, 21, "Alice", 1000, 1000, R"({"Health":{"current":55}})");
    w.world.tick(T0);
    const Json lines = w.value(a, "Report").value("lines", Json::array());
    check(lines.size() == 3 && lines[0] == "init" && lines[1] == "enter Alice health 55" &&
              lines[2].get<std::string>().starts_with("update 0.05"),
          "init, then onEnter after restore, then update(dt): " + lines.dump());

    w.sent.clear();
    w.world.despawn(a);
    check(w.sent.saves.size() == 1 && w.sent.saves[0].charId == 21 && w.sent.saves[0].components &&
              Json::parse(*w.sent.saves[0].components)["Health"]["current"] == 42.0,
          "onLeave runs before the despawn save");

    const auto b = w.spawn(2, 22, "Bob", 1000, 1000, R"({"Health":{"current":1}})");
    w.sent.clear();
    w.world.shutdown_scripts();
    check(w.sent.saves.empty(), "shutdown_scripts saves nothing itself");
    w.world.despawn(b);
    check(w.sent.saves.size() == 1 && Json::parse(*w.sent.saves[0].components)["Health"]["current"] == 43.0,
          "shutdown: onLeave (42), then shutdown (+1), then the final save; onLeave not run twice");
}

// Character creation options: validateCharacter(name, options) in any system decides; without one only empty options
// pass. Every hook must return true; an error rejects and disables its system. kast.get(e, "Character").options.
void checkCharacterCreation() {
    {
        LuaRoot r;
        r.write("systems/plain.lua", "return { update = function() end }");
        TestWorld w({});
        check(w.world.load_scripts(r.root).empty(), "creation: plain system loads");
        check(w.world.validate_character("Ann", {}) && !w.world.validate_character("Ann", {{"class", "mage"}}),
              "creation: no hook -> empty options pass, any option is refused");
    }
    LuaRoot r;
    r.write("components/report.lua", kReport);
    r.write("systems/a_class.lua", R"(return {
    update = function() end,
    validateCharacter = function(name, options) return options.class == "mage" and name ~= "Bad" end,
    onEnter = function(e)
        local o = kast.get(e, "Character").options
        kast.add(e, "Report")
        kast.set(e, "Report", {lines = {tostring(o.class), tostring(o.hair)}})
    end,
})");
    r.write("systems/b_hair.lua", R"(return {
    update = function() end,
    validateCharacter = function(name, options)
        if options.hair == "boom" then error("bad hair") end
        if options.hair == "yes" then return "yes" end
        return options.hair ~= "green"
    end,
})");
    TestWorld w(r.defs().defs);
    check(w.world.load_scripts(r.root).empty(), "creation: hook systems load");
    check(w.world.validate_character("Ann", {{"class", "mage"}}) && w.world.validate_character("Ann", {{"class", "mage"}, {"hair", "red"}}),
          "creation: accepted when every hook returns true");
    check(!w.world.validate_character("Ann", {}) && !w.world.validate_character("Ann", {{"class", "rogue"}}) &&
              !w.world.validate_character("Bad", {{"class", "mage"}}),
          "creation: the first hook refuses a missing class, another class and the name Bad");
    check(!w.world.validate_character("Ann", {{"class", "mage"}, {"hair", "green"}}) &&
              !w.world.validate_character("Ann", {{"class", "mage"}, {"hair", "yes"}}),
          "creation: the second hook refuses too (false, and a non-boolean)");
    const auto a = w.spawn(1, 41, "Ann", 1000, 1000, "", 1, {{"class", "mage"}, {"hair", "red"}});
    const auto b = w.spawn(2, 42, "Old", 1010, 1000);
    check(w.value(a, "Report")["lines"] == Json::parse(R"(["mage", "red"])") &&
              w.value(b, "Report")["lines"] == Json::parse(R"(["nil", "nil"])"),
          "creation: kast.get(e, \"Character\").options as created; none for an old character");
    check(!w.world.validate_character("Ann", {{"class", "mage"}, {"hair", "boom"}}), "creation: an error in a hook refuses");
    const auto status = w.world.script_status();
    check(status.size() == 2 && status[0].ok && !status[1].ok && contains(status[1].error, "systems/b_hair.lua:4: bad hair"),
          "creation: the failing hook's system is disabled: " + status[1].error);
    check(w.world.validate_character("Ann", {{"class", "mage"}, {"hair", "green"}}), "creation: a disabled system's hook no longer runs");
}

void checkErrorsDisableOneSystem() {
    LuaRoot r;
    r.write("components/report.lua", kReport);
    r.write("systems/a_bad.lua", "local n = 0\nreturn { update = function()\n n = n + 1\n if n == 2 then error('boom') end\nend }");
    r.write("systems/b_good.lua", R"(local n = 0
return { update = function()
    n = n + 1
    for e in kast.query("Report") do kast.set(e, "Report", {lines = {tostring(n)}}) end
end, onEnter = function(e) kast.add(e, "Report") end })");
    r.write("systems/c_enter.lua", "return { update = function() end, onEnter = function(e)\n kast.get(e, 'Nope')\nend }");
    TestWorld w(r.defs().defs);
    check(w.world.load_scripts(r.root).empty(), "error systems load");
    const auto a = w.spawn(1, 31, "Alice", 1000, 1000);
    for (int i = 0; i < 3; ++i) w.world.tick(T0 + std::chrono::milliseconds(50 * i));
    check(w.value(a, "Report")["lines"] == Json::parse(R"(["3"])"), "the good system kept running every tick");
    const auto status = w.world.script_status();
    check(status.size() == 3 && !status[0].ok && contains(status[0].error, "systems/a_bad.lua:4: boom"),
          "runtime error disables that system, logged with path:line: " + status[0].error);
    check(status[1].ok, "other system still enabled");
    check(!status[2].ok && contains(status[2].error, "systems/c_enter.lua:2: unknown component \"Nope\""),
          "an onEnter error disables only its system: " + status[2].error);
    check(w.value(a, "Report").is_object(), "the next system's onEnter still ran");
}

void checkStartupErrors() {
    LuaRoot r;
    r.write("systems/a_syntax.lua", "return {");
    r.write("systems/b_noupdate.lua", "return {}");
    r.write("systems/c_number.lua", "return 5");
    r.write("systems/d_hook.lua", "return { update = function() end, onEnter = 5 }");
    r.write("systems/e_init.lua", "return { update = function() end, init = function()\n error('init failed') end }");
    r.write("systems/f_io.lua", "io.write('x')\nreturn { update = function() end }");
    r.write("systems/g_ok.lua", "return { update = function() end }");
    r.write("systems/sub/ignored.lua", "return 5"); // not recursive
    TestWorld w({});
    const auto errors = w.world.load_scripts(r.root);
    const auto has = [&](const std::string& part) {
        return std::ranges::any_of(errors, [&](const std::string& e) { return contains(e, part); });
    };
    check(errors.size() == 6, "every bad system is reported: " + std::to_string(errors.size()));
    check(has("systems/a_syntax.lua:1:"), "syntax error with line");
    check(has("systems/b_noupdate.lua: returned table has no update function"), "no update");
    check(has("systems/c_number.lua: must return a table with an update function"), "not a table");
    check(has("systems/d_hook.lua: \"onEnter\" must be a function"), "bad hook");
    check(has("systems/e_init.lua:2: init failed"), "init error refuses the start");
    check(has("systems/f_io.lua:1:"), "io is not reachable");
    const auto status = w.world.script_status();
    check(status.size() == 7 && status[6].path == "systems/g_ok.lua" && status[6].ok, "alphabetical, the good one ok");

    LuaRoot empty;
    TestWorld e({});
    check(e.world.load_scripts(empty.root).empty() && e.world.script_status().empty(), "no systems/ folder is fine");
}

// --- persistence ---

void checkPersistence() {
    LuaRoot r;
    r.write("components/health.lua", R"(return { name = "Health", persist = true, fields = {
        { name = "current", type = "float", default = 100, min = 0 }, { name = "max", type = "float", default = 100 } } })");
    r.write("components/temp.lua", "return { name = 'Temp', fields = { { name = 'v', type = 'int' } } }");
    TestWorld w(r.defs().defs);
    const auto a = w.spawn(1, 41, "Alice", 1000, 1000,
                           R"({"Health":{"current":"bad","max":50,"extra":1},"Old":{"x":[1,2],"s":"keep"},"Temp":{"v":9}})");
    check(w.value(a, "Health") == Json::parse(R"({"current":100.0,"max":50.0})"),
          "drift on load: bad value reset to default, unknown field dropped: " + w.value(a, "Health").dump());
    check(w.value(a, "Temp").is_null(), "a non-persist component in the column is not restored");
    auto& reg = w.world.registry();
    lua::pool(reg, "Temp").emplace(a, lua::ComponentData{Json::parse(R"({"v":1})")});
    lua::pool(reg, "Health").get(a).value["current"] = 12.5;
    const auto column = lua::components_column(reg, a, r.defs().defs);
    const Json saved = column ? Json::parse(*column) : Json();
    check(saved["Health"] == Json::parse(R"({"current":12.5,"max":50.0})"), "persist component saved");
    check(saved["Old"] == Json::parse(R"({"x":[1,2],"s":"keep"})"), "unknown component written back verbatim");
    check(saved["Temp"] == Json::parse(R"({"v":9})"), "non-persist entry kept verbatim, not the runtime value");

    // Round trip: what was saved restores to the same values.
    const auto b = w.spawn(2, 42, "Bob", 1000, 1000, *column);
    check(w.value(b, "Health") == w.value(a, "Health"), "save -> load round trip");

    const auto c = w.spawn(3, 43, "Carol", 1000, 1000, "[1,2]");
    check(!lua::components_column(reg, c, r.defs().defs), "unreadable column is never overwritten");
    const auto d = w.spawn(4, 44, "Dan", 1000, 1000);
    check(lua::components_column(reg, d, r.defs().defs) == std::optional<std::string>("{}"), "empty column -> {}");

    // kast changes to a persisted component mark the character Dirty, so save_system picks it up.
    r.write("systems/s.lua", "return { update = function() for e in kast.query('Health') do kast.set(e, 'Health', {max = 77}) end end }");
    TestWorld w2(r.defs().defs);
    check(w2.world.load_scripts(r.root).empty(), "persist system loads");
    const auto e = w2.spawn(1, 45, "Eve", 1000, 1000, R"({"Health":{"current":1,"max":2}})");
    check(!w2.world.registry().all_of<world::Dirty>(e), "not dirty after spawn");
    w2.world.tick(T0 + std::chrono::seconds(120)); // past the save interval
    check(w2.sent.saves.size() == 1 && Json::parse(*w2.sent.saves[0].components)["Health"]["max"] == 77.0,
          "a kast.set on a persist component is saved by the periodic save");
}

// --- replication ---

void checkReplication() {
    LuaRoot r;
    r.write("components/health.lua", "return { name = 'Health', replicate = 'aoi', fields = { { name = 'hp', type = 'int', default = 5 } } }");
    r.write("components/wallet.lua", "return { name = 'Wallet', replicate = 'owner', fields = { { name = 'gold', type = 'int' } } }");
    r.write("components/secret.lua", "return { name = 'Secret', fields = { { name = 'v', type = 'int' } } }");
    r.write("components/big.lua", "return { name = 'Big', replicate = 'aoi', fields = { { name = 's', type = 'string' } } }");
    TestWorld w(r.defs().defs);
    auto& reg = w.world.registry();
    const auto set = [&](entt::entity e, const char* name, const char* json) {
        auto& p = lua::pool(reg, name);
        if (p.contains(e)) p.get(e).value = Json::parse(json);
        else p.emplace(e, lua::ComponentData{Json::parse(json)});
    };
    const auto a = w.spawn(1, 51, "Alice", 1000, 1000);
    const auto b = w.spawn(2, 52, "Bob", 1050, 1000);
    const auto c = w.spawn(3, 53, "Carol", 1800, 1800);
    const uint32_t A = wire(a);
    set(a, "Health", R"({"hp":5})");
    set(a, "Wallet", R"({"gold":7})");
    set(a, "Secret", R"({"v":1})");
    w.world.tick(T0);

    check(w.sent.updates(1, A, "Health").size() == 1 && w.sent.updates(1, A, "Health")[0].json == R"({"hp":5})", "owner gets aoi");
    check(w.sent.updates(1, A, "Wallet").size() == 1 && w.sent.updates(1, A, "Wallet")[0].json == R"({"gold":7})", "owner gets owner");
    check(w.sent.updates(2, A, "Health").size() == 1, "observer gets aoi");
    check(w.sent.updates(2, A, "Wallet").empty(), "observer never gets owner");
    check(w.sent.updates(1, A, "Secret").empty() && w.sent.updates(2, A, "Secret").empty(), "none is never sent");
    check(w.sent.updates(3, A).empty(), "out of range gets nothing");
    // Spawn before any component data for that entity.
    const auto& bMsgs = w.sent.bySession[2];
    const auto spawnAt = std::ranges::find_if(bMsgs, [&](const auto& m) { auto* s = std::get_if<proto::Spawn>(&m); return s && s->entityId == A; });
    const auto updateAt = std::ranges::find_if(bMsgs, [&](const auto& m) { auto* u = std::get_if<proto::ComponentUpdate>(&m); return u && u->entityId == A; });
    check(spawnAt != bMsgs.end() && updateAt != bMsgs.end() && spawnAt < updateAt, "Spawn arrives before ComponentUpdate");

    w.sent.clear();
    w.world.tick(T0 + std::chrono::milliseconds(50));
    check(w.sent.of<proto::ComponentUpdate>(1).empty() && w.sent.of<proto::ComponentUpdate>(2).empty(), "unchanged is not resent");

    w.sent.clear();
    set(a, "Health", R"({"hp":6})");
    lua::pool(reg, "Wallet").remove(a);
    w.world.tick(T0 + std::chrono::milliseconds(100));
    check(w.sent.updates(1, A).size() == 1 && w.sent.updates(2, A).size() == 1 && w.sent.updates(2, A)[0].json == R"({"hp":6})",
          "a change goes to owner + observers once");
    check(w.sent.of<proto::ComponentRemove>(1).size() == 1 && w.sent.of<proto::ComponentRemove>(1)[0].name == "Wallet" &&
              w.sent.of<proto::ComponentRemove>(2).empty(),
          "owner component removal goes to the owner only");

    // Carol walks into range in the same tick Alice's Health changes: one full-set update, after Spawn.
    w.sent.clear();
    set(a, "Health", R"({"hp":7})");
    reg.get<world::Transform>(c).pos = {1000, 0, 1060};
    reg.emplace_or_replace<world::Moved>(c);
    w.world.tick(T0 + std::chrono::milliseconds(150));
    const auto cMsgs = w.sent.bySession[3];
    check(w.sent.updates(3, A).size() == 1 && w.sent.updates(3, A)[0].json == R"({"hp":7})", "AOI enter sends the full set once");
    check(!cMsgs.empty() && std::holds_alternative<proto::Spawn>(cMsgs.front()), "Spawn first for the new viewer");
    check(w.sent.updates(2, A).size() == 1, "existing observer gets the change once");

    // Big component: not sent, logged once; Despawn means nothing more for that id.
    w.sent.clear();
    set(a, "Big", (R"({"s":")" + std::string(70000, 'x') + R"("})").c_str());
    w.world.tick(T0 + std::chrono::milliseconds(200));
    check(w.sent.updates(1, A, "Big").empty() && w.sent.updates(2, A, "Big").empty(), "too-large component not sent");
    w.world.despawn(a);
    w.sent.clear();
    w.world.tick(T0 + std::chrono::milliseconds(250));
    for (uint32_t sid : {1u, 2u, 3u}) check(w.sent.updates(sid, A).empty(), "nothing after Despawn");
    (void)b;
}

// --- events (docs/services/world/events.html): the same cases as the engine's checks ---

std::vector<std::string> g_log; // log lines while a LogCapture lives; the log backend thread appends

// The captured lines, once the log backend has written everything logged so far.
const std::vector<std::string>& captured() {
    log::flush();
    return g_log;
}
void clear_log() {
    log::flush();
    g_log.clear();
}

struct LogCapture {
    LogCapture() {
        clear_log();
        log::set_test_sink([](const std::string& line) { g_log.push_back(line); });
    }
    ~LogCapture() {
        log::flush();
        log::set_test_sink(nullptr);
    }
};

// Index of the first captured log line containing part, or SIZE_MAX.
size_t log_index(const std::string& part) {
    const auto& lines = captured();
    for (size_t i = 0; i < lines.size(); ++i)
        if (contains(lines[i], part)) return i;
    return SIZE_MAX;
}
bool logged(const std::string& part) { return log_index(part) != SIZE_MAX; }

void checkEventBus() {
    LogCapture capture;
    EventBus bus;
    std::vector<std::string> seen;
    const auto record = [&](std::string tag) {
        return [&seen, tag](const nlohmann::json& payload) { seen.push_back(tag + payload.dump()); };
    };
    EventBus::Subscription a1 = bus.subscribe("a", record("a1"));
    EventBus::Subscription b1 = bus.subscribe("b", record("b1"));
    EventBus::Subscription a2 = bus.subscribe("a", record("a2"));
    bus.publish("b", 1);
    bus.publish("a", 2);
    check(seen.empty(), "bus: publish only queues");
    bus.drain();
    check(seen == std::vector<std::string>{"b11", "a12", "a22"}, "bus: publish order, then subscription order");

    // A subscriber that re-publishes, subscribes and unsubscribes during a drain.
    seen.clear();
    std::vector<EventBus::Subscription> added;
    EventBus::Subscription cut;
    EventBus::Subscription actor = bus.subscribe("act", [&](const nlohmann::json&) {
        seen.push_back("actor");
        bus.publish("act", "again");                      // next drain
        added.push_back(bus.subscribe("act", record("late"))); // from the next event
        cut.unsubscribe();                                  // immediately, even for this event
        cut.unsubscribe();                                  // twice: no-op
    });
    cut = bus.subscribe("act", record("cut"));
    bus.publish("act", nullptr);
    bus.drain();
    check(seen == std::vector<std::string>{"actor"}, "bus: unsubscribe mid-drain is immediate; a new subscription waits");
    seen.clear();
    bus.drain();
    check(seen == std::vector<std::string>{"actor", "late\"again\""}, "bus: an event published in a drain arrives in the next");
    actor.unsubscribe();
    added.clear();
    bus.drain(); // the last "again"
    seen.clear();

    bool after = false;
    EventBus::Subscription thrower = bus.subscribe("t", [](const nlohmann::json&) { throw std::runtime_error("bad"); });
    EventBus::Subscription next = bus.subscribe("t", [&](const nlohmann::json&) { after = true; });
    bus.publish("t", nullptr);
    bus.drain();
    check(after && logged("Event 't': C++ subscriber failed: bad"), "bus: a throwing C++ subscriber is logged, the rest run");

    {
        EventBus::Subscription scoped = bus.subscribe("s", record("scoped"));
        EventBus::Subscription moved = std::move(scoped);
    }
    bus.publish("s", nullptr);
    bus.drain();
    check(seen.empty(), "bus: a destroyed handle unsubscribes (after a move too)");

    bus.publish("a", 3);
    bus.dropQueued();
    bus.drain();
    check(seen.empty(), "bus: dropQueued discards undelivered events");

    const uint64_t staleId = a1.id();
    a1.unsubscribe();
    a1.unsubscribe(); // stale: no-op
    EventBus::Subscription fresh = bus.subscribe("a", record("fresh"));
    check(fresh.id() > staleId, "bus: ids are never reused");
    a2.unsubscribe();
    bus.publish("a", 6);
    bus.drain();
    check(seen == std::vector<std::string>{"fresh6"}, "bus: stale handles never remove newer subscriptions");
}

// Lua -> Lua and Lua -> C++: same tick after every update, one subscription order across Lua and C++, independent
// payload copies, payload conversion.
void checkLuaEvents() {
    LogCapture capture;
    LuaRoot r;
    r.write("systems/a.lua", R"(local tick = 0
assert(type(kast.publish) == "function" and type(kast.subscribe) == "function" and type(kast.unsubscribe) == "function")
assert(not pcall(function() kast.publish = nil end), "kast.publish can't be overwritten")
return { update = function(dt)
    tick = tick + 1
    kast.log("ev a update " .. tick)
    if tick == 1 then
        kast.publish("first", {n = 1})
        kast.publish("data", {list = {1, 2, 3}, obj = {a = "x"}, empty = {}, flag = true, s = "str", f = 1.5, i = -7})
        kast.publish("bare")
    end
end })");
    r.write("systems/b.lua", R"(return {
    init = function()
        kast.subscribe("first", function(p) kast.log("ev b first " .. p.n); p.n = 99; p.extra = true end)
        kast.subscribe("data", function(p) kast.log("ev b data") end)
    end,
    update = function(dt) kast.log("ev b update") end,
})");
    r.write("systems/d.lua", R"(local subscribed = false
return { update = function(dt)
    if not subscribed then
        subscribed = true
        kast.subscribe("first", function(p)
            assert(p.n == 1 and p.extra == nil, "each subscriber gets its own copy")
            kast.log("ev d first")
        end)
    end
end })");
    TestWorld w({});
    const auto errors = w.world.load_scripts(r.root);
    check(errors.empty(), "event systems load" + (errors.empty() ? "" : ": " + errors[0]));
    nlohmann::json first, data, bare = "unset";
    EventBus::Subscription s1 = w.world.bus().subscribe("first", [&](const nlohmann::json& p) {
        log::info("ev C++ first");
        first = p;
    });
    EventBus::Subscription s2 = w.world.bus().subscribe("data", [&](const nlohmann::json& p) { data = p; });
    EventBus::Subscription s3 = w.world.bus().subscribe("bare", [&](const nlohmann::json& p) { bare = p; });
    w.world.tick(T0);
    for (const auto& s : w.world.script_status()) check(s.ok, "events: system still enabled: " + s.path + " " + s.error);
    check(log_index("ev a update 1") < log_index("ev b update") && log_index("ev b update") < log_index("ev b first 1"),
          "events: Lua -> Lua in the same tick, after every update");
    check(log_index("ev b first 1") < log_index("ev C++ first") && log_index("ev C++ first") < log_index("ev d first") &&
              log_index("ev d first") < log_index("ev b data") && logged("ev b data"),
          "events: publish order, then one subscription order across Lua and C++");
    check(first == nlohmann::json{{"n", 1}}, "events: Lua -> C++ payload, unchanged by Lua subscribers");
    check(data == nlohmann::json::parse(
                      R"({"list": [1, 2, 3], "obj": {"a": "x"}, "empty": {}, "flag": true, "s": "str", "f": 1.5, "i": -7})"),
          "events: payload conversion (arrays, objects, empty table, scalars)");
    check(data["i"].is_number_integer() && bare.is_null(), "events: integral numbers are integers; no payload is null");
}

// C++ -> Lua, JSON -> Lua conversion, re-publish and subscribe during a drain, unsubscribe during a drain.
void checkLuaEventsFromCpp() {
    LogCapture capture;
    LuaRoot r;
    r.write("systems/r.lua", R"(local late = 0
local second
return {
    init = function()
        kast.subscribe("cpp", function(p)
            assert(#p.arr == 2 and p.arr[1] == 1 and p.arr[2] == 2, "array -> sequence")
            assert(p.o.k == "v" and p.b == false and p.nul == nil and p.i == 7 and p.s == "x", "object -> table")
            assert(next(p.e) == nil, "empty object -> empty table")
            kast.log("ev r cpp")
            kast.publish("pong", {from = "r"}) -- next tick
            kast.subscribe("cpp", function() late = late + 1; kast.log("ev r late " .. late) end) -- from the next event
        end)
        kast.subscribe("cut", function()
            kast.unsubscribe(second)
            kast.unsubscribe(second) -- twice: no-op
            kast.log("ev r cutter")
        end)
        second = kast.subscribe("cut", function() kast.log("ev r second") end)
    end,
    update = function(dt) end,
})");
    TestWorld w({});
    check(w.world.load_scripts(r.root).empty(), "C++ -> Lua system loads");
    int pongs = 0;
    EventBus::Subscription thrower =
        w.world.bus().subscribe("pong", [](const nlohmann::json&) { throw std::runtime_error("pong failed"); });
    EventBus::Subscription counter = w.world.bus().subscribe("pong", [&](const nlohmann::json&) { ++pongs; });
    const nlohmann::json payload =
        nlohmann::json::parse(R"({"arr": [1, 2], "o": {"k": "v"}, "b": false, "nul": null, "i": 7, "s": "x", "e": {}})");
    w.world.bus().publish("cpp", payload);
    w.world.bus().publish("cut", nullptr);
    w.world.tick(T0);
    check(w.world.script_status()[0].ok, "events: C++ -> Lua payload converts to tables");
    check(logged("ev r cpp") && pongs == 0 && !logged("ev r late"),
          "events: published during a drain waits; a subscription added during a drain misses the current event");
    check(logged("ev r cutter") && !logged("ev r second"), "events: unsubscribe during a drain is immediate");
    w.world.bus().publish("cpp", payload);
    w.world.tick(T0 + std::chrono::milliseconds(50));
    check(pongs == 1 && logged("Event 'pong': C++ subscriber failed: pong failed"),
          "events: re-published event arrives next tick; a throwing C++ subscriber doesn't stop the rest");
    check(logged("ev r late 1") && !logged("ev r late 2"), "events: the late subscription gets later events only");
    check(w.world.script_status()[0].ok, "events: the system is still fine");
}

// Rejections raise at the calling file:line and queue nothing; the same for the client sends.
void checkLuaEventErrors() {
    LogCapture capture;
    LuaRoot r;
    r.write("systems/rej.lua", R"(local cyc = {}; cyc.self = cyc
local deep = {}; local t = deep; for i = 1, 32 do t.x = {}; t = t.x end
local ok32 = {}; t = ok32; for i = 1, 31 do t.x = {}; t = t.x end
local function try(f) local ok, err = pcall(f); kast.log("rej " .. tostring(ok) .. " " .. tostring(err)) end
return { update = function(dt)
    try(function() kast.publish(42) end)
    try(function() kast.publish("") end)
    try(function() kast.publish("e", {f = print}) end)
    try(function() kast.publish("e", {u = newproxy()}) end)
    try(function() kast.publish("e", {co = coroutine.create(print)}) end)
    try(function() kast.publish("e", cyc) end)
    try(function() kast.publish("e", deep) end)
    try(function() kast.publish("e", {1, a = 2}) end)
    try(function() kast.publish("e", {1, nil, 3}) end)
    try(function() kast.publish("e", {[0] = 1}) end)
    try(function() kast.publish("e", {x = 0 / 0}) end)
    try(function() kast.subscribe("e", 5) end)
    try(function() kast.subscribe("", print) end)
    try(function() kast.unsubscribe(5) end)
    try(function() kast.unsubscribe({}) end)
    kast.publish("ok32", ok32)
    try(function() kast.sendTo("x", "e") end)
    try(function() kast.sendTo(1.5, "e") end)
    try(function() kast.sendNearby(-1, "e") end)
    try(function() kast.sendTo(0, 5) end)
    try(function() kast.sendNearby(0, "") end)
    try(function() kast.broadcast(nil) end)
    try(function() kast.sendTo(0, "e", {f = print}) end)
    try(function() kast.broadcast("e", {x = 1 / 0}) end)
    try(function() kast.broadcast("e", cyc) end)
    try(function() kast.broadcast("big", string.rep("x", 65527)) end)
    kast.broadcast("big", string.rep("x", 65526))
end })");
    TestWorld w({});
    check(w.world.load_scripts(r.root).empty(), "rejection system loads");
    int queued = 0, ok32 = 0;
    EventBus::Subscription e = w.world.bus().subscribe("e", [&](const nlohmann::json&) { ++queued; });
    EventBus::Subscription deep = w.world.bus().subscribe("ok32", [&](const nlohmann::json&) { ++ok32; });
    const auto sid = 9u;
    w.spawn(sid, 61, "Watcher", 1000, 1000);
    w.sent.clear();
    w.world.tick(T0);
    check(w.world.script_status()[0].ok, "events: rejections are catchable Lua errors");
    const std::pair<int, const char*> cases[] = {
        {6, "kast.publish: event name must be a non-empty string, got 42"},
        {7, "kast.publish: event name must be a non-empty string"},
        {8, "kast.publish: payload.f: a function can't be sent in an event"},
        {9, "kast.publish: payload.u: a userdata can't be sent in an event"},
        {10, "kast.publish: payload.co: a thread can't be sent in an event"},
        {11, "kast.publish: payload.self: the table contains itself (cycle)"},
        {12, "kast.publish: payload.x.x.x"}, // ... 32 levels: checked below
        {13, "kast.publish: payload: table keys must be exactly 1..n (an array) or all strings (an object)"},
        {14, "kast.publish: payload: table keys must be exactly 1..n"},
        {15, "kast.publish: payload: table keys must be exactly 1..n"},
        {16, "kast.publish: payload.x must be a finite number"},
        {17, "kast.subscribe: callback must be a function, got 5"},
        {18, "kast.subscribe: event name must be a non-empty string"},
        {19, "kast.unsubscribe: expected a handle from kast.subscribe, got 5"},
        {20, "kast.unsubscribe: expected a handle from kast.subscribe, got table"},
        {22, "kast.sendTo: invalid entity id x"},
        {23, "kast.sendTo: invalid entity id 1.5"},
        {24, "kast.sendNearby: invalid entity id -1"},
        {25, "kast.sendTo: event name must be a non-empty string, got 5"},
        {26, "kast.sendNearby: event name must be a non-empty string"},
        {27, "kast.broadcast: event name must be a non-empty string, got nil"},
        {28, "kast.sendTo: payload.f: a function can't be sent in an event"},
        {29, "kast.broadcast: payload.x must be a finite number"},
        {30, "kast.broadcast: payload.self: the table contains itself (cycle)"},
        {31, "kast.broadcast: event \\\"big\\\" is too large to send: 65532 bytes of name and JSON payload, at most 65531"},
    };
    for (const auto& [line, message] : cases)
        check(logged(std::format("rej false systems/rej.lua:{}: {}", line, message)),
              std::format("events: rejected at systems/rej.lua:{}: {}", line, message));
    check(logged("x: tables are nested more than 32 deep"), "events: nesting deeper than 32 is rejected");
    check(queued == 0 && ok32 == 1, "events: rejected calls queue nothing; 32 nested tables are fine");
    const auto events = w.sent.of<proto::ServerEvent>(sid);
    check(events.size() == 1 && events[0].name == "big" && events[0].json.size() == 65528 &&
              proto::encode(events[0]).size() == proto::kHeaderSize + proto::kMaxPayloadBytes,
          "events: rejected sends send nothing; one at the frame limit goes out");
}

// A failing subscriber disables only its system and removes its subscriptions.
void checkLuaEventFailures() {
    LogCapture capture;
    LuaRoot r;
    r.write("systems/bad.lua", R"(return {
    init = function()
        kast.subscribe("hit", function(p)
            error("subscriber failed")
        end)
        kast.subscribe("hit", function(p) kast.log("ev bad second") end)
        kast.subscribe("other", function(p) kast.log("ev bad other") end)
    end,
    update = function(dt) end,
})");
    r.write("systems/good.lua", R"(return {
    init = function() kast.subscribe("hit", function(p) kast.log("ev good " .. p.n) end) end,
    update = function(dt) end,
})");
    TestWorld w({});
    check(w.world.load_scripts(r.root).empty(), "failure systems load");
    w.world.bus().publish("hit", {{"n", 1}});
    w.world.tick(T0);
    const auto status = w.world.script_status();
    check(!status[0].ok && contains(status[0].error, "systems/bad.lua:4: subscriber failed") &&
              logged(R"("msg":"Lua system disabled")") && logged(R"("callback":"event 'hit'")") &&
              logged(R"("error":"systems/bad.lua:4: subscriber failed)"),
          "events: a failing subscriber disables its system, logged with the callback and path:line");
    check(!logged("ev bad second") && logged("ev good 1") && status[1].ok,
          "events: the failed system's other subscribers stop; other systems' still run");
    w.world.bus().publish("other", nullptr);
    w.world.tick(T0 + std::chrono::milliseconds(50));
    check(!logged("ev bad other"), "events: disabling removes all of the system's subscriptions");
}

// Events published from init, onEnter, onLeave and subscribers wait for the next tick's drain; a graceful shutdown
// drops the queue.
void checkLuaEventHooks() {
    LogCapture capture;
    LuaRoot r;
    r.write("systems/h.lua", R"(return {
    init = function() kast.publish("hook", {from = "init"}) end,
    onEnter = function(e) kast.publish("hook", {from = "enter"}) end,
    onLeave = function(e) kast.publish("hook", {from = "leave"}) end,
    shutdown = function() kast.publish("hook", {from = "shutdown"}) end,
    update = function(dt) end,
})");
    TestWorld w({});
    check(w.world.load_scripts(r.root).empty(), "hook event system loads");
    std::vector<std::string> got;
    EventBus::Subscription sub =
        w.world.bus().subscribe("hook", [&](const nlohmann::json& p) { got.push_back(p["from"].get<std::string>()); });
    const auto a = w.spawn(1, 71, "Alice", 1000, 1000);
    check(got.empty(), "events: init and onEnter events wait for the tick");
    w.world.tick(T0);
    check(got == std::vector<std::string>{"init", "enter"}, "events: init and onEnter events arrive in the next tick's drain");
    w.world.despawn(a);
    check(got.size() == 2, "events: onLeave events wait for the tick");
    w.world.tick(T0 + std::chrono::milliseconds(50));
    check(got.size() == 3 && got[2] == "leave", "events: onLeave events arrive in the next tick's drain");

    w.spawn(2, 72, "Bob", 1000, 1000);
    w.world.shutdown_scripts();
    w.world.bus().drain();
    check(got.size() == 3, "events: a graceful shutdown drops the queued events (onEnter, onLeave, shutdown)");
}

// kast.world.request's server side: "client.<name>" carries requestId (none for kast.world.send); kast.reply answers
// it to the owner alone as Reply{requestId, ok, data or the reason}; bad arguments raise.
void checkReply() {
    LuaRoot r;
    r.write("systems/ask.lua", R"(return {
    init = function()
        kast.subscribe("client.ask", function(e)
            if e.data.yes then kast.reply(e.entity, e.requestId, true, {echo = e.data.n})
            else kast.reply(e.entity, e.requestId, false, "nope") end
        end)
        kast.subscribe("client.plain", function(e) kast.sendTo(e.entity, "plain", {request = tostring(e.requestId)}) end)
        kast.subscribe("client.bad", function(e)
            local errors = {}
            for _, a in ipairs({{e.entity, 0, true}, {e.entity, "x", true}, {e.entity, e.requestId, "yes"},
                                {e.entity, e.requestId, false, 5}, {e.entity, e.requestId, false, nil}}) do
                local ok, err = pcall(kast.reply, a[1], a[2], a[3], a[4])
                errors[#errors + 1] = ok and "no error" or err
            end
            kast.reply(e.entity, e.requestId, true) -- nil data: {}
            kast.sendTo(e.entity, "errors", {list = errors})
        end)
    end,
    update = function(dt) end,
})");
    TestWorld w({});
    const auto loadErrors = w.world.load_scripts(r.root);
    check(loadErrors.empty(), "reply system loads " + (loadErrors.empty() ? "" : loadErrors[0]));
    const auto a = w.spawn(1, 91, "Alice", 1000, 1000);
    w.spawn(2, 92, "Bob", 1010, 1000);
    w.sent.clear();
    w.world.push_client_event(a, "ask", nlohmann::json{{"yes", true}, {"n", 4}}, 11);
    w.world.push_client_event(a, "ask", nlohmann::json::object(), 12);
    w.world.push_client_event(a, "plain", nlohmann::json::object());
    w.world.push_client_event(a, "bad", nlohmann::json::object(), 13);
    w.world.tick(T0);
    w.world.tick(T0 + std::chrono::milliseconds(50));
    check(w.sent.of<proto::Reply>(1) == std::vector<proto::Reply>{{11, true, R"({"echo":4})"}, {12, false, R"("nope")"},
                                                                  {13, true, "{}"}},
          "kast.reply: Reply{requestId, ok, data JSON / the reason as a JSON string}, in call order");
    check(w.sent.of<proto::Reply>(2).empty(), "kast.reply: the owner alone");
    const auto events = w.sent.of<proto::ServerEvent>(1);
    check(events.size() == 2 && events[0].json == R"({"request":"nil"})", "kast.world.send's client event has no requestId");
    const Json errors = events.size() == 2 ? Json::parse(events[1].json)["list"] : Json();
    const auto has = [&](size_t i, const std::string& part) {
        return errors.is_array() && i < errors.size() && errors[i].get<std::string>().find(part) != std::string::npos;
    };
    check(has(0, "kast.reply: requestId must be a client event's requestId, got 0") &&
              has(1, "kast.reply: requestId must be a client event's requestId, got x") &&
              has(2, "kast.reply: ok must be a boolean, got yes") &&
              has(3, "kast.reply: a rejection's reason must be a string, got 5") &&
              has(4, "kast.reply: a rejection's reason must be a string, got nil"),
          "kast.reply: bad arguments raise " + errors.dump());
}

// kast.sendTo / sendNearby / broadcast: audiences, call order, after component replication, before Despawn.
void checkClientEvents() {
    LuaRoot r;
    r.write("components/health.lua", "return { name = 'Health', replicate = 'aoi', fields = { { name = 'hp', type = 'int' } } }");
    r.write("systems/send.lua", R"(local ids, tick = {}, 0
local gone
return {
    onEnter = function(e) ids[kast.get(e, "Character").name] = e ; kast.add(e, "Health") end,
    onLeave = function(e)
        kast.sendNearby(e, "bye", {who = e})
        gone = e
    end,
    update = function(dt)
        tick = tick + 1
        if tick == 1 then
            kast.set(ids.Alice, "Health", {hp = 5}) -- replicated this tick, before the events
            kast.sendTo(ids.Alice, "to", {n = 1})
            kast.sendNearby(ids.Alice, "near", {n = 2})
            kast.broadcast("all", {n = 3})
            kast.sendTo(ids.Bob, "bob")
            kast.sendTo(4000000000, "nobody") -- a valid id not in the world: nothing
        elseif gone then
            kast.sendTo(gone, "late")    -- left: nothing
            kast.sendNearby(gone, "late")
            gone = nil
        end
    end,
})");
    TestWorld w(r.defs().defs);
    check(w.world.load_scripts(r.root).empty(), "client event system loads");
    const auto a = w.spawn(1, 81, "Alice", 1000, 1000);
    w.spawn(2, 82, "Bob", 1010, 1000);
    w.spawn(3, 83, "Carol", 1800, 1800); // out of view
    const uint32_t A = wire(a);
    w.sent.clear();
    w.world.tick(T0);
    const auto names = [&](uint32_t sid) {
        std::vector<std::string> out;
        for (const auto& e : w.sent.of<proto::ServerEvent>(sid)) out.push_back(e.name);
        return out;
    };
    check(names(1) == std::vector<std::string>{"to", "near", "all"}, "sendTo/sendNearby/broadcast reach the owner, in call order");
    check(names(2) == std::vector<std::string>{"near", "all", "bob"}, "sendNearby reaches a viewer; sendTo only its target");
    check(names(3) == std::vector<std::string>{"all"}, "broadcast reaches a player out of view; nothing else does");
    check(w.sent.of<proto::ServerEvent>(1)[0].json == R"({"n":1})", "the payload is compact JSON");
    const auto index = [&](uint32_t sid, auto pred) {
        const auto& msgs = w.sent.bySession[sid];
        for (size_t i = 0; i < msgs.size(); ++i)
            if (pred(msgs[i])) return i;
        return msgs.size();
    };
    const auto isEvent = [](const proto::Message& m) { return std::holds_alternative<proto::ServerEvent>(m); };
    const auto isHealthA = [&](const proto::Message& m) {
        auto* u = std::get_if<proto::ComponentUpdate>(&m);
        return u && u->entityId == A && u->json == R"({"hp":5})";
    };
    const auto isSpawnA = [&](const proto::Message& m) {
        auto* s = std::get_if<proto::Spawn>(&m);
        return s && s->entityId == A;
    };
    check(index(1, isHealthA) < index(1, isEvent) && index(2, isHealthA) < index(2, isEvent) &&
              index(2, isSpawnA) < index(2, isEvent),
          "client events go out after Spawn and component replication");

    w.sent.clear();
    w.world.despawn(a);
    const auto isDespawnA = [&](const proto::Message& m) {
        auto* d = std::get_if<proto::Despawn>(&m);
        return d && d->entityId == A;
    };
    check(names(1) == std::vector<std::string>{"bye"} && names(2) == std::vector<std::string>{"bye"} && names(3).empty(),
          "onLeave sends go out at the despawn, to the leaver and its viewers");
    check(index(2, isEvent) < index(2, isDespawnA) && index(2, isDespawnA) < w.sent.bySession[2].size(),
          "onLeave sends go out before the Despawn");
    w.sent.clear();
    w.world.tick(T0 + std::chrono::milliseconds(50));
    check(names(1).empty() && names(2).empty() && names(3).empty(), "a target that left is a silent no-op");
    for (const auto& s : w.world.script_status()) check(s.ok, "client events: system still enabled: " + s.error);

    // Graceful shutdown: onLeave sends go out before the despawns.
    w.sent.clear();
    w.world.shutdown_scripts();
    check(names(2) == std::vector<std::string>{"bye"} && names(3) == std::vector<std::string>{"bye"},
          "shutdown: onLeave sends go out before the despawns");
}

void checkCodec() {
    for (const proto::Message& m : {proto::Message{proto::ComponentUpdate{7, "Health", R"({"current":1.5})"}},
                                    proto::Message{proto::ComponentRemove{7, "Health"}},
                                    proto::Message{proto::ServerEvent{"quest.update", R"({"quest":7})"}}}) {
        const auto frame = proto::encode(m);
        const auto d = proto::decode_frame(frame, proto::kMaxPayloadBytes);
        check(d.status == proto::DecodeStatus::ok && d.msg == m, std::string("codec round trip ") + proto::opcode_name(proto::opcode_of(m)));
    }
    check(proto::ComponentUpdate::fits(6, 65535 - 14) && !proto::ComponentUpdate::fits(6, 65535 - 13), "frame size limit");
    check(proto::ServerEvent::fits(3, 65535 - 7) && !proto::ServerEvent::fits(3, 65535 - 6), "ServerEvent frame size limit");
}

// --- state machines (docs/services/world/state-machines.html) ---

// Indexes of the captured log lines whose "msg" contains part (log lines are JSON: quotes in msg are escaped).
std::vector<size_t> said_at(const std::string& part) {
    std::vector<size_t> out;
    const auto& lines = captured();
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto line = nlohmann::json::parse(lines[i], nullptr, false);
        if (line.is_object() && contains(line.value("msg", ""), part)) out.push_back(i);
    }
    return out;
}
bool said(const std::string& part) { return !said_at(part).empty(); }
size_t said_first(const std::string& part) {
    const auto at = said_at(part);
    return at.empty() ? SIZE_MAX : at[0];
}

// Every "FAIL ..." a Lua expect/raises logged becomes a failed check; done must have been logged.
void check_lua_expectations(const std::string& what, const std::string& done) {
    for (size_t i : said_at("FAIL ")) check(false, what + ": " + nlohmann::json::parse(g_log[i]).value("msg", ""));
    check(said(done), what + ": the Lua checks ran (" + done + ")");
}

constexpr const char* kLuaExpect = R"(local function expect(ok, what) if not ok then kast.error("FAIL " .. what) end end
local function raises(want, f)
    local ok, err = pcall(f)
    if ok then kast.error("FAIL no error, wanted: " .. want)
    elseif not string.find(err, want, 1, true) then kast.error("FAIL got [" .. tostring(err) .. "] wanted [" .. want .. "]") end
end
)";

// kast.debug/log/warn/error(message [, fields]): fields become JSON on the line; bad fields raise at file:line.
void checkLuaLogging() {
    LogCapture capture;
    log::set_level(log::Level::debug);
    LuaRoot r;
    r.write("systems/logs.lua", std::string(kLuaExpect) + R"lua(local t = {}
t.self = t
return {
    init = function()
        kast.debug("dbg line")
        kast.log("with fields", {player = 7, zone = "x"})
        kast.warn("nested", {pos = {x = 1, y = 2.5}, tags = {"a", "b"}})
        kast.error("no fields", {})
        raises("kast.log: fields must be a table, got 5", function() kast.log("m", 5) end)
        raises("kast.log: fields must be a table of named fields", function() kast.log("m", {1, 2}) end)
        raises("kast.warn: payload.cb: a function can't be sent", function() kast.warn("m", {cb = print}) end)
        raises("kast.error: payload.self: the table contains itself (cycle)", function() kast.error("m", t) end)
        raises('kast.log: "ts" is reserved', function() kast.log("m", {ts = 1}) end)
        raises('kast.debug: "level" is reserved', function() kast.debug("m", {level = "x"}) end)
        raises('kast.log: "msg" is reserved', function() kast.log("m", {nested = {}, msg = "x"}) end)
        local ok, err = pcall(function() kast.log("m", true) end)
        expect(not ok and string.find(err, "systems/logs.lua:%d+: kast.log: fields") ~= nil, "error at file:line: " .. tostring(err))
        kast.log("logging checks done")
    end,
    update = function() kast.debug("tick debug"); kast.log("tick info"); kast.warn("tick warn") end,
})lua");
    TestWorld w({});
    const auto errors = w.world.load_scripts(r.root);
    check(errors.empty(), "lua logging: loads" + (errors.empty() ? "" : ": " + errors[0]));
    check_lua_expectations("lua logging", "logging checks done");

    const auto line = [](const std::string& msg) {
        const size_t i = said_first("systems/logs.lua: " + msg);
        return i == SIZE_MAX ? nlohmann::ordered_json() : nlohmann::ordered_json::parse(g_log[i]);
    };
    const auto dbg = line("dbg line");
    check(dbg.value("level", "") == "debug", "kast.debug logs at debug");
    std::vector<std::string> keys;
    for (const auto& [key, unused] : dbg.items()) keys.push_back(key);
    check(keys == std::vector<std::string>{"ts", "level", "msg"}, "a line starts ts, level, msg");
    const std::string ts = dbg.value("ts", "");
    check(ts.size() == 24 && ts[10] == 'T' && ts[19] == '.' && ts.back() == 'Z', "ts is ISO 8601 UTC with ms: " + ts);
    const size_t withFields = said_first("with fields");
    check(withFields != SIZE_MAX &&
              contains(g_log[withFields], R"("msg":"systems/logs.lua: with fields","player":7,"zone":"x"})"),
          "kast.log fields: \"player\":7,\"zone\":\"x\" after msg");
    const auto nested = line("nested");
    check(nested.value("level", "") == "warn" && nested["pos"] == nlohmann::ordered_json{{"x", 1}, {"y", 2.5}} &&
              nested["tags"] == nlohmann::ordered_json{"a", "b"},
          "nested tables become nested JSON");
    check(line("no fields").size() == 3, "an empty fields table adds nothing");

    log::set_level(log::Level::warn);
    clear_log();
    w.world.tick(T0);
    check(!said("tick debug") && !said("tick info") && said("tick warn"), "LOG_LEVEL warn: info and debug are dropped");
    log::set_level(log::Level::debug);
    clear_log();
    w.world.tick(T0 + std::chrono::milliseconds(50));
    check(said("tick debug") && said("tick info"), "LOG_LEVEL debug: kast.debug shows");
    log::set_level(log::Level::info);
}

// Every callback logs "<enter|exit> <from>-><to> entity <id>".
constexpr const char* kDoor = R"(local function note(what) return function(ctx)
    kast.log(what .. " " .. tostring(ctx.from) .. "->" .. ctx.to .. " entity " .. tostring(ctx.entity))
end end
return {
    name = "door",
    initial = "closed",
    states = {
        closed = { allowedFrom = {"open"}, enter = note("enter"), exit = note("exit") },
        open   = { allowedFrom = {"closed", "locked"}, enter = note("enter"), exit = note("exit") },
        locked = { enter = note("enter"), exit = note("exit") },
        broken = { allowedFrom = {"*"}, enter = note("enter"), exit = note("exit") },
    },
})";
constexpr const char* kPlain = "return { name = 'plain', initial = 'a', states = { a = {}, b = { allowedFrom = {'a'} } } }";

void checkFsmDefinitions() {
    LuaRoot r;
    r.write("fsms/sub/door.lua", kDoor).write("fsms/plain.lua", kPlain).write("fsms/notes.txt", "not lua");
    TestWorld w({});
    const auto errors = w.world.load_scripts(r.root);
    check(errors.empty(), "fsm: valid definitions load" + (errors.empty() ? "" : ": " + errors[0]));
    const lua::StateMachineDef* door = w.world.fsm().definition("door");
    check(door && door->path == "fsms/sub/door.lua" && door->initial == "closed" &&
              door->stateNames() == std::vector<std::string>{"broken", "closed", "locked", "open"},
          "fsm: recursive scan; name, path, initial, sorted states");
    check(door && door->states.at("locked").fromAny && door->states.at("broken").fromAny &&
              !door->states.at("open").fromAny && door->states.at("open").from == std::set<std::string>{"closed", "locked"} &&
              door->states.at("closed").enter && door->states.at("closed").exit && !w.world.fsm().definition("notes"),
          "fsm: allowedFrom omitted / \"*\" / listed; callbacks bound; .lua files only");
    check(w.world.fsm().definition("plain") && !w.world.fsm().definition("plain")->states.at("a").enter,
          "fsm: a state without callbacks");

    const auto machine = [](const std::string& states, const std::string& extra = "") {
        return "return {name='m', initial='a', states={" + states + "}" + extra + "}";
    };
    const std::pair<std::string, std::string> cases[] = {
        {"return {", "fsms/m.lua:1:"},
        {"error('nope')", "fsms/m.lua:1: nope"},
        {"io.write('x')", "fsms/m.lua:1:"},
        {"return 5", "fsms/m.lua: must return exactly one table {name = ..., initial = ..., states = {...}}"},
        {machine("a={}") + ", 2", "fsms/m.lua: must return exactly one table"},
        {"return setmetatable({name='m', initial='a', states={a={}}}, {})", "fsms/m.lua: tables must be plain data"},
        {"return {initial='a', states={a={}}}", "fsms/m.lua: \"name\" must be a non-empty string"},
        {"return {name=5, initial='a', states={a={}}}", "fsms/m.lua: \"name\" must be a non-empty string"},
        {"return {name='m', initial='z', states={a={}}}", "fsms/m.lua: \"initial\" must name one of its states, got z"},
        {"return {name='m', states={a={}}}", "\"initial\" must name one of its states, got nil"},
        {machine("a={}", ", colour=1"), "fsms/m.lua: unknown key colour"},
        {"return {name='m', initial='a', states=5}", "fsms/m.lua: \"states\" must be a table {name = {...}, ...}"},
        {"return {name='m', initial='a', states={}}", "fsms/m.lua: \"states\" must not be empty"},
        {machine("['*']={}"), "fsms/m.lua: state names must be non-empty strings other than \"*\", got *"},
        {machine("a=5"), "fsms/m.lua: state \"a\" must be a table"},
        {machine("a={allowedFrom={'q'}}"), "fsms/m.lua: state \"a\": \"allowedFrom\" names unknown state \"q\""},
        {machine("a={allowedFrom='a'}"), "fsms/m.lua: state \"a\": \"allowedFrom\" must be a list {...}"},
        {machine("a={allowedFrom={5}}"), "fsms/m.lua: state \"a\": \"allowedFrom\" must hold state names, got 5"},
        {machine("a={enter=5}"), "fsms/m.lua: state \"a\": \"enter\" must be a function"},
        {machine("a={exit='x'}"), "fsms/m.lua: state \"a\": \"exit\" must be a function"},
        {machine("a={update=function() end}"), "fsms/m.lua: state \"a\": unknown key update"},
    };
    for (const auto& [body, want] : cases) {
        LuaRoot c;
        c.write("fsms/m.lua", body);
        TestWorld cw({});
        const auto e = cw.world.load_scripts(c.root);
        check(e.size() == 1 && contains(e[0], want) && !cw.world.fsm().definition("m"),
              "fsm: rejected with \"" + want + "\" (got " + (e.empty() ? "nothing" : e[0]) + ")");
    }

    LuaRoot d;
    d.write("fsms/a.lua", kPlain).write("fsms/z/b.lua", kPlain).write("fsms/door.lua", kDoor);
    TestWorld dw({});
    const auto dup = dw.world.load_scripts(d.root);
    check(dup.size() == 2 && dup[0] == "fsms/a.lua: name \"plain\" is also declared in fsms/z/b.lua" &&
              dup[1] == "fsms/z/b.lua: name \"plain\" is also declared in fsms/a.lua",
          "fsm: two files with the same name are both rejected, each naming the other");
    check(!dw.world.fsm().definition("plain") && dw.world.fsm().definition("door"), "fsm: the other definitions still load");

    // GAME_STATE_MACHINES: an unknown name (or one whose file was rejected, or with no fsms/ folder) refuses the start.
    TestWorld gw({});
    const auto unknown = gw.world.load_scripts(d.root, {"door", "nope", "plain"});
    check(unknown.size() == 4 && unknown[2] == "GAME_STATE_MACHINES: game state machine \"nope\" has no valid definition in fsms/" &&
              unknown[3] == "GAME_STATE_MACHINES: game state machine \"plain\" has no valid definition in fsms/",
          "fsm: listed game machines without a valid definition are startup errors");
    LuaRoot none;
    TestWorld nw({});
    check(nw.world.load_scripts(none.root).empty(), "fsm: no fsms/ folder and no game machines is fine");
    TestWorld lw({});
    const auto listed = lw.world.load_scripts(none.root, {"door"});
    check(listed.size() == 1 && contains(listed[0], "\"door\" has no valid definition"),
          "fsm: a listed game machine with no fsms/ folder refuses the start");
}

// Transition rules, callback and event order, C++ API (game machine), and the Lua subscriber.
void checkFsmRules() {
    LogCapture capture;
    LuaRoot r;
    r.write("fsms/door.lua", kDoor);
    r.write("systems/s.lua", R"(return {
    init = function()
        kast.subscribe("fsm.changed", function(p)
            kast.log("lua got " .. p.machine .. " " .. p.from .. "->" .. p.to .. " entity " .. tostring(p.entity))
        end)
    end,
    update = function(dt) end,
})");
    TestWorld w({});
    const auto errors = w.world.load_scripts(r.root, {"door"});
    check(errors.empty(), "fsm rules: loads" + (errors.empty() ? "" : ": " + errors[0]));
    auto& fsm = w.world.fsm();
    check(said_at("fsms/door.lua: enter nil->closed entity nil").size() == 1 && said_at("fsms/door.lua:").size() == 1,
          "fsm: a game machine enters its initial state after init, once, with from = nil (kast.log prefixed by path)");
    std::vector<nlohmann::json> changed;
    EventBus::Subscription sub = w.world.bus().subscribe("fsm.changed", [&](const nlohmann::json& p) { changed.push_back(p); });
    w.world.tick(T0);
    check(changed.empty() && fsm.state("door") == "closed", "fsm: no fsm.changed for the first entry");

    clear_log();
    check(fsm.canChange("door", "open") && fsm.canChange("door", "locked") && fsm.canChange("door", "broken") &&
              !fsm.canChange("door", "closed") && !fsm.canChange("door", "flying"),
          "fsm: canChange: listed, omitted and \"*\" allowed; self not listed and unknown denied");
    check(captured().empty() && fsm.state("door") == "closed", "fsm: canChange has no side effects and logs nothing");

    check(!fsm.change("door", "closed") &&
              said(R"(State machine "door": "closed" can't be entered from "closed" (allowedFrom); staying in "closed")"),
          "fsm: a self-transition not allowed by allowedFrom is denied with the engine's warning");
    check(!fsm.change("door", "flying") && said(R"(State machine "door": unknown state "flying"; staying in "closed")"),
          "fsm: an unknown state is denied with a warning");
    check(said_at("fsms/door.lua:").empty() && fsm.state("door") == "closed", "fsm: a denial runs no callbacks");
    w.world.tick(T0 + std::chrono::milliseconds(50));
    check(changed.empty(), "fsm: a denial publishes no event");

    clear_log();
    check(fsm.change("door", "open") && fsm.state("door") == "open", "fsm: an allowed change applies immediately");
    check(said_first("exit closed->open") < said_first("enter closed->open") && said("enter closed->open entity nil"),
          "fsm: exit(old), then enter(new) with the same ctx");
    check(changed.empty(), "fsm: fsm.changed waits for the drain");
    w.world.tick(T0 + std::chrono::milliseconds(100));
    check(changed.size() == 1 &&
              changed[0] == nlohmann::json{{"machine", "door"}, {"entity", nullptr}, {"from", "closed"}, {"to", "open"}},
          "fsm: fsm.changed {machine, entity = null, from, to} delivered to C++ at the drain");
    check(said("systems/s.lua: lua got door closed->open entity nil"), "fsm: ... and to Lua subscribers");

    changed.clear();
    check(fsm.change("door", "broken"), "fsm: \"*\" from open");
    clear_log();
    check(fsm.change("door", "broken") && said("exit broken->broken") && said("enter broken->broken"),
          "fsm: a self-transition allowed by \"*\" runs exit and enter");
    check(fsm.change("door", "locked") && fsm.change("door", "locked"), "fsm: a self-transition allowed by an omitted allowedFrom");
    check(fsm.change("door", "open") && fsm.change("door", "closed"), "fsm: locked -> open -> closed");
    check(fsm.states("door") == std::vector<std::string>{"broken", "closed", "locked", "open"}, "fsm: states() sorted");
    w.world.tick(T0 + std::chrono::milliseconds(150));
    check(changed.size() == 6 && changed[1]["from"] == "broken" && changed[1]["to"] == "broken" &&
              changed[3]["from"] == "locked" && changed[3]["to"] == "locked" && changed[5]["to"] == "closed",
          "fsm: one event per transition, self-transitions included, in order");
    for (const auto& s : w.world.script_status()) check(s.ok, "fsm rules: system still enabled: " + s.error);
}

// Changes requested inside enter/exit are queued and applied after the transition, in order; a queued denial warns.
void checkFsmQueue() {
    LogCapture capture;
    LuaRoot r;
    r.write("fsms/q.lua", R"(return { name = "q", initial = "a", states = {
    a = { allowedFrom = {"c"}, exit = function(ctx) kast.log("exit a") end },
    b = {
        enter = function(ctx)
            kast.log("enter b")
            kast.log("queued c " .. tostring(kast.fsm.change("q", "c")))
            kast.log("queued y " .. tostring(kast.fsm.change("r", "y")))
            kast.log("still " .. kast.fsm.state("q"))
            kast.log("enter b done")
        end,
        exit = function(ctx) kast.log("exit b") end,
    },
    c = { allowedFrom = {"b"}, enter = function(ctx)
        kast.log("enter c")
        kast.log("queued c again " .. tostring(kast.fsm.change("q", "c")))
    end },
} })");
    r.write("fsms/r.lua", R"(return { name = "r", initial = "x", states = {
    x = {}, y = { enter = function(ctx) kast.log("enter y") end },
} })");
    TestWorld w({});
    check(w.world.load_scripts(r.root, {"q", "r"}).empty(), "fsm queue: loads");
    std::vector<nlohmann::json> changed;
    EventBus::Subscription sub = w.world.bus().subscribe("fsm.changed", [&](const nlohmann::json& p) { changed.push_back(p); });
    clear_log();
    check(w.world.fsm().change("q", "b"), "fsm queue: change returns true");
    const std::vector<size_t> order = {said_first("exit a"),      said_first("enter b"),     said_first("queued c true"),
                                       said_first("queued y true"), said_first("still b"),   said_first("enter b done"),
                                       said_first("exit b"),      said_first("enter c"),     said_first("queued c again true"),
                                       said_first("enter y"),
                                       said_first(R"(State machine "q": "c" can't be entered from "c" (allowedFrom))")};
    check(std::ranges::is_sorted(order) && order.back() != SIZE_MAX,
          "fsm queue: requests inside callbacks return true, wait for the transition, then apply in request order; the "
          "denied one warns");
    check(w.world.fsm().state("q") == "c" && w.world.fsm().state("r") == "y", "fsm queue: final states");
    w.world.tick(T0);
    check(changed.size() == 3 && changed[0]["to"] == "b" && changed[1]["to"] == "c" && changed[2]["machine"] == "r",
          "fsm queue: one event per applied transition, in order");
}

// A failing or runaway callback is logged with path:line and a traceback; the transition commits and nothing is
// disabled. A runaway callback that a system triggered also uses up that system's budget: the system is disabled.
void checkFsmCallbackErrors() {
    LogCapture capture;
    LuaRoot r;
    r.write("fsms/err.lua", R"(return { name = "err", initial = "ok", states = {
    ok = {},
    bad = { enter = function(ctx)
        error("kaput")
    end },
    slow = { exit = function(ctx) kast.log("slow exit") end, enter = function(ctx) while true do end end },
} })");
    r.write("systems/a_count.lua", "local n = 0\nreturn { update = function() n = n + 1 ; kast.log('tick ' .. n) end }");
    r.write("systems/b_trigger.lua", R"(local go = false
return { update = function()
    if go then kast.fsm.change("err", "slow") end
end, onEnter = function(e) go = true end })");
    TestWorld w({});
    check(w.world.load_scripts(r.root, {"err"}).empty(), "fsm errors: loads");
    auto& fsm = w.world.fsm();
    std::vector<nlohmann::json> changed;
    EventBus::Subscription sub = w.world.bus().subscribe("fsm.changed", [&](const nlohmann::json& p) { changed.push_back(p); });

    check(fsm.change("err", "bad") && fsm.state("err") == "bad", "fsm: a failing enter still commits; change returns true");
    check(said(R"(State machine fsms/err.lua: enter "bad" failed: fsms/err.lua:4: kaput)") && said("stack traceback"),
          "fsm: the callback error is logged with path:line and a traceback");
    check(fsm.change("err", "ok") && fsm.state("err") == "ok", "fsm: the machine stays usable");

    const auto start = std::chrono::steady_clock::now();
    check(fsm.change("err", "slow") && fsm.state("err") == "slow", "fsm: a runaway enter commits too");
    check(std::chrono::steady_clock::now() - start < std::chrono::seconds(5) &&
              said(R"(State machine fsms/err.lua: enter "slow" failed: fsms/err.lua:6: script ran longer than 2 seconds)"),
          "fsm: a runaway callback is stopped by the script budget and logged");
    check(fsm.change("err", "ok") && said("slow exit"), "fsm: Lua runs again after the timeout; the machine still works");
    w.world.tick(T0);
    check(changed.size() == 4, "fsm: failing callbacks still publish fsm.changed");
    check(said("systems/a_count.lua: tick 1"), "fsm: systems keep running after a callback error");
    for (const auto& s : w.world.script_status()) check(s.ok, "fsm errors: no system disabled: " + s.error);

    // From inside a system's update: the shared 2 s budget runs out in the callback, so the system is disabled too.
    w.spawn(1, 91, "Alice", 1000, 1000);
    w.world.tick(T0 + std::chrono::milliseconds(50));
    const auto status = w.world.script_status();
    check(status.size() == 2 && status[0].ok && !status[1].ok && contains(status[1].error, "script ran longer than 2 seconds"),
          "fsm: a runaway callback triggered by a system disables that system (shared budget), only that one");
    check(fsm.state("err") == "slow" && fsm.change("err", "ok"), "fsm: ... and the machine still works");
}

// kast.fsm misuse raises at the calling line; kast.fsm is read-only; an init touching a game machine enters it once.
void checkFsmLuaApi() {
    LogCapture capture;
    LuaRoot r;
    r.write("fsms/door.lua", kDoor).write("fsms/plain.lua", kPlain);
    r.write("systems/a_line.lua", R"(local ok, err = pcall(function()
    local state = kast.fsm.state("nope") -- not a tail call: that would drop this frame and its line
end)
kast.log("line err " .. tostring(err))
return { init = function() kast.log("init sees " .. kast.fsm.state("door")) end, update = function() end })");
    r.write("systems/b_api.lua", std::string(kLuaExpect) + R"(local done = false
return { update = function(dt)
    if done then return end
    done = true
    local bob
    for e in kast.query("Character") do bob = e end
    raises('kast.fsm.state: no game state machine "nope" (GAME_STATE_MACHINES)', function() return kast.fsm.state("nope") end)
    raises('kast.fsm.change: no game state machine "plain"', function() return kast.fsm.change("plain", "b") end)
    raises('kast.fsm.state: invalid entity id 4000000000', function() return kast.fsm.state(4000000000) end)
    raises('kast.fsm.state: invalid entity id 1.5', function() return kast.fsm.state(1.5) end)
    raises('kast.fsm.canChange: invalid entity id -1', function() return kast.fsm.canChange(-1, "a") end)
    raises('kast.fsm.states: entity ' .. bob .. ' has no "StateMachine"', function() return kast.fsm.states(bob) end)
    raises('kast.fsm.state: target must be a state machine name or an entity id, got true', function() return kast.fsm.state(true) end)
    raises('kast.fsm.change: state must be a string, got 5', function() return kast.fsm.change("door", 5) end)
    raises('kast.fsm.canChange: state must be a string, got nil', function() return kast.fsm.canChange("door") end)
    expect(kast.fsm.change("door", "nowhere") == false, "an unknown state is a denial, not an error")
    expect(kast.fsm.canChange("door", "open") == true and kast.fsm.state("door") == "closed", "canChange / state")
    local names = kast.fsm.states("door")
    expect(#names == 4 and names[1] == "broken" and names[4] == "open", "states: a sorted table")
    raises("kast is read-only", function() kast.fsm.change = nil end)
    raises("kast is read-only", function() kast.fsm = nil end)
    raises("kast is read-only", function() kast.fsm.x = 1 end)
    expect(type(kast.fsm.change) == "function", "kast.fsm unchanged")
    kast.log("api checks done")
end })");
    r.write("systems/c_raise.lua", "return { update = function()\n kast.fsm.state('nope')\nend }");
    TestWorld w({});
    check(w.world.load_scripts(r.root, {"door"}).empty(), "fsm api: loads");
    check(said_first("fsms/door.lua: enter nil->closed") < said_first("systems/a_line.lua: init sees closed") &&
              said_at("fsms/door.lua: enter").size() == 1,
          "fsm: a game machine touched during init enters right then, and only once");
    check(said(R"(systems/a_line.lua: line err systems/a_line.lua:2: kast.fsm.state: no game state machine "nope")"),
          "fsm: a misuse raises at the calling line");
    w.spawn(1, 92, "Bob", 1000, 1000);
    w.world.tick(T0);
    check_lua_expectations("fsm api", "api checks done");
    const auto status = w.world.script_status();
    check(status.size() == 3 && status[1].ok && !status[2].ok &&
              contains(status[2].error, "systems/c_raise.lua:2: kast.fsm.state: no game state machine \"nope\""),
          "fsm: an uncaught misuse in a system follows the system error policy");
}

// The StateMachine built-in on characters: add, set machine / initialState, changes, remove, despawn, shutdown, not
// persisted or replicated; and C++ / Lua parity on entity machines.
void checkFsmEntities() {
    LogCapture capture;
    LuaRoot r;
    r.write("components/health.lua", "return { name = 'Health', persist = true, replicate = 'aoi', fields = { { name = 'hp', type = 'int' } } }");
    r.write("fsms/door.lua", kDoor).write("fsms/plain.lua", kPlain);
    r.write("systems/sm.lua", std::string(kLuaExpect) + R"lua(local parity = {}
local function run(e)
    local out = {}
    local function add(v) out[#out + 1] = tostring(v) end
    add(kast.fsm.state(e)); add(kast.fsm.canChange(e, "open")); add(kast.fsm.canChange(e, "closed"))
    add(kast.fsm.change(e, "closed")); add(kast.fsm.change(e, "flying")); add(kast.fsm.change(e, "open"))
    add(kast.fsm.state(e)); add(kast.fsm.change(e, "broken")); add(kast.fsm.change(e, "broken"))
    add(table.concat(kast.fsm.states(e), ","))
    return table.concat(out, " ")
end
return {
    onEnter = function(e)
        kast.add(e, "Health")
        local name = kast.get(e, "Character").name
        kast.add(e, "StateMachine")
        if name ~= "Alice" then
            kast.set(e, "StateMachine", {machine = "door"})
            parity[e] = name == "Carol"
            return
        end
        expect(kast.has(e, "StateMachine"), "has after add")
        local sm = kast.get(e, "StateMachine")
        expect(sm.machine == "" and sm.initialState == "", "add leaves machine and initialState empty")
        raises('kast.fsm.state: entity ' .. e .. ': StateMachine has no "machine"', function() return kast.fsm.state(e) end)
        kast.set(e, "StateMachine", {machine = "door", initialState = "bogus"})
        expect(kast.fsm.state(e) == "closed", "machine set: enters the definition's initial (bad initialState)")
        kast.set(e, "StateMachine", {initialState = "open"})
        expect(kast.fsm.state(e) == "closed", "changing only initialState does nothing")
        expect(kast.fsm.change(e, "open"), "change on an entity machine")
        kast.set(e, "StateMachine", {machine = "plain", initialState = "b"})
        expect(kast.fsm.state(e) == "b", "changing machine enters the new machine's initialState")
        kast.set(e, "StateMachine", {machine = "nope"})
        raises('kast.fsm.state: entity ' .. e .. ': no valid state machine definition "nope"', function() return kast.fsm.state(e) end)
        kast.set(e, "StateMachine", {initialState = ""})
        kast.set(e, "StateMachine", {machine = "door"})
        expect(kast.fsm.state(e) == "closed", "back to door: its initial (empty initialState)")
        kast.remove(e, "StateMachine")
        raises('kast.fsm.state: entity ' .. e .. ' has no "StateMachine"', function() return kast.fsm.state(e) end)
        kast.add(e, "StateMachine")
        kast.set(e, "StateMachine", {machine = "door", initialState = "locked"})
        expect(kast.fsm.state(e) == "locked", "re-added: enters a valid initialState")
        raises('"StateMachine": unknown field "current"', function() kast.set(e, "StateMachine", {current = "x"}) end)
        raises('must be a string', function() kast.set(e, "StateMachine", {machine = 5}) end)
        kast.log("entity checks done")
    end,
    update = function(dt)
        for e, lua in pairs(parity) do
            if lua then kast.log("parity " .. run(e)) end
            parity[e] = nil
        end
    end,
})lua");
    TestWorld w(r.defs().defs);
    check(w.world.load_scripts(r.root).empty(), "fsm entities: loads");
    const auto alice = w.spawn(1, 93, "Alice", 1000, 1000);
    const uint32_t A = wire(alice);
    check_lua_expectations("fsm entities", "entity checks done");
    const std::string who = "entity " + std::to_string(A);
    check(said(who + R"(: StateMachine "door": initialState "bogus" is not one of its states; using "closed")"),
          "fsm: a bad initialState falls back to initial with a warning");
    check(said(who + R"(: StateMachine "door": initialState is empty; using "closed")"), "fsm: an empty initialState warns");
    check(said_at(who + R"(: StateMachine: no valid state machine definition "nope")").size() == 1,
          "fsm: a machine with no valid definition is logged once");
    check(said("fsms/door.lua: enter nil->closed entity " + std::to_string(A)) &&
              said("fsms/door.lua: enter nil->locked entity " + std::to_string(A)),
          "fsm: entity machines enter with ctx.entity = the wire id and from = nil");
    check(said_at("fsms/door.lua: exit").size() == 1, "fsm: only the explicit change ran an exit (machine swap and remove don't)");

    // C++ / Lua parity: Carol is driven from Lua, Dave from C++, with the same operations.
    const auto carol = w.spawn(2, 94, "Carol", 1010, 1000);
    const auto dave = w.spawn(3, 95, "Dave", 1020, 1000);
    std::vector<nlohmann::json> changed;
    EventBus::Subscription sub = w.world.bus().subscribe("fsm.changed", [&](const nlohmann::json& p) { changed.push_back(p); });
    clear_log();
    w.world.tick(T0);
    auto& fsm = w.world.fsm();
    const lua::StateMachines::Target d = wire(dave);
    std::string cpp = fsm.state(d) + " " + (fsm.canChange(d, "open") ? "true" : "false") + " " +
                      (fsm.canChange(d, "closed") ? "true" : "false");
    for (const char* to : {"closed", "flying", "open"}) cpp += std::string(" ") + (fsm.change(d, to) ? "true" : "false");
    cpp += " " + fsm.state(d);
    for (const char* to : {"broken", "broken"}) cpp += std::string(" ") + (fsm.change(d, to) ? "true" : "false");
    std::string names;
    for (const auto& n : fsm.states(d)) names += (names.empty() ? "" : ",") + n;
    cpp += " " + names;
    check(cpp == "closed true false false false true open true true broken,closed,locked,open", "fsm C++: " + cpp);
    check(said("systems/sm.lua: parity " + cpp), "fsm: Lua gives the same results as C++");
    const auto warnings = [&](entt::entity e) {
        return said_at("State machine \"door\" (entity " + std::to_string(wire(e)) + ")").size();
    };
    check(warnings(carol) == 2 && warnings(dave) == 2, "fsm: the same warnings from Lua and C++");
    w.world.tick(T0 + std::chrono::milliseconds(50));
    const auto events_of = [&](entt::entity e) {
        return std::ranges::count_if(changed, [&](const nlohmann::json& p) { return p["entity"] == wire(e); });
    };
    check(events_of(carol) == 3 && events_of(dave) == 3, "fsm: the same events from Lua and C++, entity = wire id");

    try {
        fsm.state(std::string("nope"));
        check(false, "fsm C++: an unknown game machine throws");
    } catch (const std::invalid_argument& e) {
        check(contains(e.what(), "no game state machine \"nope\""), std::string("fsm C++: ") + e.what());
    }
    for (const lua::StateMachines::Target& bad : {lua::StateMachines::Target{std::string("door")}, lua::StateMachines::Target{999999u}}) {
        bool threw = false;
        try { fsm.change(bad, "open"); } catch (const std::invalid_argument&) { threw = true; }
        check(threw, "fsm C++: bad targets throw std::invalid_argument");
    }

    // Not persisted, not replicated; despawn and shutdown run no exit.
    bool replicated = false;
    for (const auto& [sid, msgs] : w.sent.bySession)
        for (const auto& m : msgs)
            if (auto* u = std::get_if<proto::ComponentUpdate>(&m); u && u->name == "StateMachine") replicated = true;
    check(!replicated, "fsm: StateMachine is never replicated");
    check(w.sent.of<proto::ComponentUpdate>(1).size() > 0, "fsm: other components still replicate");
    clear_log();
    w.sent.clear();
    w.world.despawn(alice);
    check(w.sent.saves.size() == 1 && w.sent.saves[0].components && contains(*w.sent.saves[0].components, "Health") &&
              !contains(*w.sent.saves[0].components, "StateMachine"),
          "fsm: StateMachine is not persisted");
    w.world.shutdown_scripts();
    w.world.despawn(carol);
    w.world.despawn(dave);
    check(said_at("fsms/door.lua: exit").empty(), "fsm: despawn and shutdown run no exit");
    check(w.world.registry().view<lua::StateMachineRuntime>().empty(), "fsm: machines go away with their characters");
}

} // namespace

// --- NPCs (docs/services/world/npcs.html) ---

// Every kast.create / kast.destroy / NPC Transform rule from Lua; each bad call raises and changes nothing.
constexpr const char* kNpcApiSystem = R"lua(
local EDGE = 2048 - 2 ^ -13 -- the largest float below 2048: still on tile 7
local npcs = function() local n = 0 ; for _ in kast.query("Npc") do n = n + 1 end ; return n end
local done = false
return { update = function()
    if done then return end
    done = true
    local players = {}
    for e in kast.query("Character") do players[#players + 1] = e end
    local p = players[1]
    local g = kast.create("Guard", {template = "GuardTpl"})
    expect(type(g) == "number" and g ~= p, "create returns a new wire id")
    expect(kast.has(g, "Npc") and kast.has(g, "Transform") and not kast.has(g, "Character"), "Npc + Transform, no Character")
    expect(not kast.has(p, "Npc"), "players have no Npc")
    local n = kast.get(g, "Npc")
    expect(n.name == "Guard" and n.template == "GuardTpl" and n.npc == "", "Npc {name, template, npc}; npc empty with a template")
    local tf = kast.get(g, "Transform")
    expect(tf.position.x == 1024 and tf.position.y == 0 and tf.position.z == 1024 and tf.heading == 0, "default: spawn point, heading 0")
    raises('entity ' .. g .. ' has no "Character"', function() kast.get(g, "Character") end)
    local h = kast.create("Far", {template = "T", position = {x = -5, y = 1000, z = 5000}, heading = 2})
    tf = kast.get(h, "Transform")
    expect(tf.position.x == 0 and tf.position.y == 0 and tf.position.z == EDGE and tf.heading == 2 and tf.map == 1, "position clamped onto the map (tile 7's far edge belongs to tile 8), y onto the ground")
    local ids, chars, all = {}, 0, {}
    for e in kast.query("Npc") do ids[#ids + 1] = e end
    for _ in kast.query("Character") do chars = chars + 1 end
    for e in kast.query() do all[#all + 1] = e end
    expect(#ids == 2 and ids[1] == g and ids[2] == h, "query Npc: NPCs only, in id order")
    expect(chars == 1, "query Character: players only")
    expect(#all == 3 and all[1] < all[2] and all[2] < all[3], "query(): players and NPCs in id order")

    local before = npcs()
    local long = string.rep("x", 256)
    local nameErr = "kast.create: name must be a non-empty string of at most 255 bytes, got "
    local tplErr = "kast.create: template must be a non-empty string of at most 255 bytes, got "
    local posErr = 'kast.create: "position" must be a table {x=, y=, z=} of finite numbers'
    for _, c in ipairs({
        {nameErr .. "nil", nil, {template = "T"}},
        {nameErr, "", {template = "T"}},
        {nameErr .. "5", 5, {template = "T"}},
        {nameErr .. "xxx", long, {template = "T"}},
        {"kast.create: opts must be a table {npc = ...} or {template = ...}, got nil", "N", nil},
        {"kast.create: opts must be a table {npc = ...} or {template = ...}, got 5", "N", 5},
        {tplErr, "N", {template = ""}},
        {tplErr .. "7", "N", {template = 7}},
        {tplErr .. "xxx", "N", {template = long}},
        {posErr, "N", {template = "T", position = 5}},
        {posErr, "N", {template = "T", position = {x = 1, y = 2}}},
        {posErr, "N", {template = "T", position = {x = 1 / 0, y = 0, z = 0}}},
        {posErr, "N", {template = "T", position = {x = 1e39, y = 0, z = 0}}},
        {'kast.create: "heading" must be a finite number', "N", {template = "T", heading = "x"}},
        {'kast.create: "heading" must be a finite number', "N", {template = "T", heading = 0 / 0}},
        {"kast.create: unknown option speed", "N", {template = "T", speed = 1}},
        {"kast.create: unknown option 1", "N", {template = "T", "extra"}},
    }) do raises(c[1], function() kast.create(c[2], c[3]) end) end
    expect(npcs() == before, "no failed create made an NPC")

    raises("kast.destroy: entity " .. p .. " is a player; only NPCs can be destroyed", function() kast.destroy(p) end)
    raises("kast.destroy: invalid entity id 1.5", function() kast.destroy(1.5) end)
    raises("kast.destroy: invalid entity id x", function() kast.destroy("x") end)
    raises("kast.destroy: invalid entity id nil", function() kast.destroy() end)
    raises("kast.destroy: invalid entity id 999999", function() kast.destroy(999999) end)
    expect(npcs() == before, "no failed destroy removed an NPC")
    kast.destroy(h)
    expect(npcs() == before - 1, "destroy removes the NPC")
    raises("kast.destroy: invalid entity id " .. h, function() kast.destroy(h) end)
    raises("invalid entity id " .. h, function() kast.get(h, "Transform") end)

    kast.set(g, "Transform", {heading = 1})
    tf = kast.get(g, "Transform")
    expect(tf.heading == 1 and tf.position.x == 1024, "partial Transform set: heading only")
    kast.set(g, "Transform", {position = {x = 3000, y = -500, z = 10}})
    tf = kast.get(g, "Transform")
    expect(tf.position.x == EDGE and tf.position.y == 0 and tf.position.z == 10 and tf.heading == 1, "Transform set clamps, y onto the ground")
    raises('"Transform": unknown field "pos"', function() kast.set(g, "Transform", {heading = 2, pos = {}}) end)
    raises('"Transform": field names must be strings, got 1', function() kast.set(g, "Transform", {5}) end)
    raises('"Transform": "position" must be a table {x=, y=, z=} of finite numbers', function() kast.set(g, "Transform", {heading = 2, position = {x = 1}}) end)
    raises('"Transform": "heading" must be a finite number', function() kast.set(g, "Transform", {position = {x = 1, y = 1, z = 1}, heading = 1 / 0}) end)
    tf = kast.get(g, "Transform")
    expect(tf.position.x == EDGE and tf.heading == 1, "every failed Transform set left it unchanged")
    raises('"Transform" is read-only on the server', function() kast.set(p, "Transform", {heading = 1}) end)
    raises('"Npc" is read-only on the server', function() kast.set(g, "Npc", {name = "x"}) end)
    raises('"Npc" is read-only on the server', function() kast.add(g, "Npc") end)
    raises('"Npc" cannot be removed', function() kast.remove(g, "Npc") end)
    raises('"Transform" cannot be removed', function() kast.remove(g, "Transform") end)
    raises('"Character" is read-only on the server', function() kast.add(g, "Character") end)

    kast.add(g, "Stats")
    kast.set(g, "Stats", {hp = 42})
    expect(kast.get(g, "Stats").hp == 42 and kast.has(g, "Stats"), "Lua components work on NPCs")
    kast.remove(g, "Stats")
    expect(not kast.has(g, "Stats"), "remove on an NPC")
    kast.add(g, "StateMachine")
    kast.set(g, "StateMachine", {machine = "plain"})
    expect(kast.fsm.state(g) == "a", "an NPC machine enters its initial state")
    expect(kast.fsm.change(g, "b") and kast.fsm.state(g) == "b", "kast.fsm.change on an NPC")
    kast.sendTo(g, "npc.hello", {x = 1}) -- an NPC has no client: nothing is sent, no error
    kast.log("npc api done")
end })lua";

void checkNpcApi() {
    LogCapture capture;
    LuaRoot r;
    r.write("components/stats.lua", kStats).write("fsms/plain.lua", kPlain);
    r.write("systems/npc.lua", std::string(kLuaExpect) + kNpcApiSystem);
    TestWorld w(r.defs().defs);
    check(w.world.load_scripts(r.root).empty(), "npc api: loads");
    w.spawn(1, 101, "Alice", 1000, 1000);
    w.sent.clear();
    w.world.tick(T0);
    check_lua_expectations("npc api", "npc api done");
    for (const auto& s : w.world.script_status()) check(s.ok, "npc api: system still enabled " + s.error);
    check(w.sent.of<proto::ServerEvent>(1).empty(), "sendTo(npc) sends nothing");
}

// MAX_NPCS: create past the cap raises and creates nothing; a destroy frees a slot.
void checkNpcCap() {
    LogCapture capture;
    LuaRoot r;
    r.write("systems/cap.lua", std::string(kLuaExpect) + R"lua(return { init = function()
    local a = kast.create("A", {template = "T"})
    kast.create("B", {template = "T"})
    raises("kast.create: too many NPCs (MAX_NPCS is 2)", function() kast.create("C", {template = "T"}) end)
    local n = 0 ; for _ in kast.query("Npc") do n = n + 1 end
    expect(n == 2, "the refused create made nothing")
    kast.destroy(a)
    kast.create("C", {template = "T"})
    kast.log("cap done")
end, update = function() end })lua");
    world::WorldRules rules;
    rules.maxNpcs = 2;
    TestWorld w({}, rules);
    check(w.world.load_scripts(r.root).empty(), "npc cap: loads (create works from init)");
    check_lua_expectations("npc cap", "cap done");

    LuaRoot zero;
    zero.write("systems/z.lua", "return { init = function() kast.create('A', {template = 'T'}) end, update = function() end }");
    rules.maxNpcs = 0;
    TestWorld z({}, rules);
    const auto errors = z.world.load_scripts(zero.root);
    check(errors.size() == 1 && contains(errors[0], "systems/z.lua:1: kast.create: too many NPCs (MAX_NPCS is 0)"),
          "MAX_NPCS 0: create raises at the call: " + (errors.empty() ? "" : errors[0]));
}

// AOI, replication and send routing for an NPC moved by a script (driven through the bus from C++), plus hooks,
// persistence and destroy.
void checkNpcRouting() {
    LogCapture capture;
    LuaRoot r;
    r.write("components/health.lua", "return { name = 'Health', persist = true, replicate = 'aoi', fields = { { name = 'hp', type = 'int', default = 5 } } }");
    r.write("components/wallet.lua", "return { name = 'Wallet', persist = true, replicate = 'owner', fields = { { name = 'gold', type = 'int', default = 1 } } }");
    r.write("fsms/plain.lua", kPlain);
    r.write("systems/guard.lua", R"lua(local g
return {
    init = function()
        g = kast.create("Guard", {template = "GuardTpl", position = {x = 1010, y = 0, z = 1000}, heading = 0.5})
        kast.add(g, "Health") ; kast.add(g, "Wallet") ; kast.add(g, "StateMachine")
        kast.set(g, "StateMachine", {machine = "plain"})
        kast.subscribe("move", function(p) kast.set(g, "Transform", {position = {x = p.x, y = 0, z = p.z}}) end)
        kast.subscribe("hp", function(p) kast.set(g, "Health", {hp = p}) ; kast.set(g, "Wallet", {gold = p}) end)
        kast.subscribe("nearby", function() kast.sendNearby(g, "guard.says", "hi") ; kast.sendTo(g, "guard.private") end)
        kast.subscribe("destroy", function() kast.destroy(g) end)
    end,
    update = function() end,
    onEnter = function(e) kast.log("onEnter " .. e .. (kast.has(e, "Npc") and " npc" or "")) end,
    onLeave = function(e) kast.log("onLeave " .. e .. (kast.has(e, "Npc") and " npc" or "")) end,
})lua");
    TestWorld w(r.defs().defs);
    check(w.world.load_scripts(r.root).empty(), "npc routing: loads");
    auto& reg = w.world.registry();
    check(reg.view<world::Npc>().size() == 1, "init created the NPC");
    const auto g = reg.view<world::Npc>().front();
    const uint32_t G = wire(g);
    const auto a = w.spawn(1, 111, "Alice", 1000, 1000);
    const auto b = w.spawn(2, 112, "Bob", 1800, 1800);
    TimePoint t = T0;
    const auto tick = [&] {
        t += std::chrono::milliseconds(50);
        w.world.tick(t);
    };
    // What session sid got about the NPC, in order: "spawn;update Health;move;..."
    const auto about = [&](uint32_t sid) {
        std::string out;
        for (const auto& m : w.sent.bySession[sid]) {
            if (auto* s = std::get_if<proto::Spawn>(&m); s && s->entityId == G) out += "spawn;";
            if (auto* d = std::get_if<proto::Despawn>(&m); d && d->entityId == G) out += "despawn;";
            if (auto* mv = std::get_if<proto::EntityMove>(&m); mv && mv->entityId == G) out += "move;";
            if (auto* u = std::get_if<proto::ComponentUpdate>(&m); u && u->entityId == G) out += "update " + u->name + ";";
            if (auto* x = std::get_if<proto::ComponentRemove>(&m); x && x->entityId == G) out += "remove " + x->name + ";";
        }
        return out;
    };

    w.sent.clear();
    tick();
    const auto spawns = w.sent.of<proto::Spawn>(1);
    const auto spawnG = std::ranges::find(spawns, G, &proto::Spawn::entityId);
    check(spawnG != spawns.end() && spawnG->kind == proto::SpawnKind::npc && spawnG->templateName == "GuardTpl" &&
              spawnG->name == "Guard" && spawnG->pos == proto::Vec3{1010, 0, 1000} && spawnG->heading == 0.5f,
          "a player in range gets Spawn{kind npc, template}");
    check(about(1) == "spawn;update Health;", "then the NPC's aoi components, never its owner ones: " + about(1));
    check(about(2).empty(), "a player out of range gets nothing about the NPC");
    check(reg.get<world::Viewers>(g).ids == std::vector<uint32_t>{wire(a)}, "the NPC's Viewers are the players that see it");
    check(!reg.all_of<world::Visible>(g), "an NPC has no Visible");

    w.sent.clear();
    for (int i = 1; i <= 3; ++i) {
        w.world.bus().publish("move", {{"x", 1010 + i}, {"z", 1000}});
        tick();
    }
    const auto moves = w.sent.of<proto::EntityMove>(1);
    check(moves.size() == 3 && moves[0].pos.x == 1011 && moves[2].pos.x == 1013 && moves[2].entityId == G,
          "each script move reaches the viewer as EntityMove, in the same tick");
    check(about(2).empty(), "the out-of-range player gets no moves");

    w.sent.clear();
    w.world.bus().publish("hp", 9);
    tick();
    check(about(1) == "update Health;", "an aoi change on the NPC reaches the viewer; owner (Wallet) nobody: " + about(1));
    bool walletSent = false;
    for (const auto& [sid, msgs] : w.sent.bySession)
        for (const auto& m : msgs)
            if (auto* u = std::get_if<proto::ComponentUpdate>(&m)) walletSent |= u->name == "Wallet" && u->entityId == G;
    check(!walletSent, "the NPC's owner component is sent to nobody");
    check(!reg.all_of<world::Dirty>(g), "a persisted component change never marks an NPC dirty");

    w.sent.clear();
    w.world.bus().publish("nearby", nullptr);
    tick();
    const auto evA = w.sent.of<proto::ServerEvent>(1);
    check(evA.size() == 1 && evA[0].name == "guard.says", "sendNearby(npc) reaches viewing players; sendTo(npc) nothing");
    check(w.sent.of<proto::ServerEvent>(2).empty(), "sendNearby(npc) skips players out of view");

    // Out of A's range and, in one jump, into B's: the grid follows (A gets Despawn, B gets Spawn and the full set).
    w.sent.clear();
    w.world.bus().publish("move", {{"x", 1790}, {"z", 1800}});
    tick();
    check(about(1) == "move;", "the jump tick: the old viewer gets the move: " + about(1));
    w.sent.clear();
    tick();
    check(about(1) == "despawn;", "next tick: the NPC left A's view: Despawn: " + about(1));
    check(about(2) == "spawn;update Health;", "and entered B's: Spawn + aoi full set: " + about(2));
    check(reg.get<world::Viewers>(g).ids == std::vector<uint32_t>{wire(b)}, "Viewers follow");
    w.sent.clear();
    tick();
    tick();
    check(about(1).empty(), "nothing after Despawn");

    // Back to A: Spawn again.
    w.world.bus().publish("move", {{"x", 1005}, {"z", 1000}});
    tick();
    w.sent.clear();
    tick();
    check(about(1) == "spawn;update Health;", "coming back: Spawn again + full set: " + about(1));

    // Both log out: the NPC is left with no viewers; it is never saved.
    w.world.despawn(b);
    w.world.despawn(a);
    check(reg.get<world::Viewers>(g).ids.empty(), "a player's despawn removes it from the NPC's Viewers");
    check(w.sent.saves.size() == 2 && w.sent.saves[0].charId == 112 && w.sent.saves[1].charId == 111,
          "only the characters are saved");
    w.world.bus().publish("move", {{"x", 1010}, {"z", 1000}});
    tick(); // the NPC moves with nobody watching

    // Destroy: the viewer gets Despawn, then nothing; hooks never ran for the NPC.
    const auto c = w.spawn(3, 113, "Carol", 1000, 1000);
    tick();
    w.sent.clear();
    w.world.bus().publish("destroy", nullptr);
    tick();
    check(about(3) == "despawn;", "kast.destroy sends Despawn to every viewer: " + about(3));
    check(!reg.valid(g) && reg.view<world::Npc>().empty(), "the NPC is gone");
    check(reg.get<world::Visible>(c).ids.empty(), "and out of the viewer's Visible");
    w.sent.clear();
    tick();
    check(about(3).empty(), "nothing after the destroy's Despawn");
    check(said("onEnter " + std::to_string(wire(a))) && said("onLeave " + std::to_string(wire(a))) && !said(" npc"),
          "onEnter / onLeave fire for players, never for NPCs");
    for (const auto& s : w.world.script_status()) check(s.ok, "npc routing: system still enabled " + s.error);
}

// An NPC destroyed in the tick a player first sees it: Spawn, then Despawn, and no component data after it.
void checkNpcDestroyedOnEnter() {
    LuaRoot r;
    r.write("components/health.lua", "return { name = 'Health', replicate = 'aoi', fields = { { name = 'hp', type = 'int', default = 5 } } }");
    r.write("systems/s.lua", R"lua(local g
return { init = function() g = kast.create("G", {template = "T"}) ; kast.add(g, "Health") end,
         update = function() if g then kast.destroy(g) ; g = nil end end })lua");
    TestWorld w(r.defs().defs);
    check(w.world.load_scripts(r.root).empty(), "destroyed-on-enter: loads");
    w.spawn(1, 121, "Alice", 1024, 1024);
    w.sent.clear();
    w.world.tick(T0);
    const auto& msgs = w.sent.bySession[1];
    check(msgs.size() == 2 && std::holds_alternative<proto::Spawn>(msgs[0]) && std::holds_alternative<proto::Despawn>(msgs[1]),
          "Spawn then Despawn, no component data after it (" + std::to_string(msgs.size()) + " messages)");
}

// Graceful shutdown: the players leave first, then remove_npcs removes every NPC without a message or a save.
void checkNpcShutdown() {
    LuaRoot r;
    r.write("systems/s.lua", "return { init = function() kast.create('A', {template = 'T'}) ; kast.create('B', {template = 'T'}) end, update = function() end }");
    TestWorld w({});
    check(w.world.load_scripts(r.root).empty(), "npc shutdown: loads");
    const auto a = w.spawn(1, 131, "Alice", 1024, 1024);
    w.world.tick(T0);
    w.world.shutdown_scripts();
    w.world.despawn(a);
    w.sent.clear();
    w.world.remove_npcs();
    check(w.world.registry().view<world::Npc>().empty(), "every NPC removed");
    check(w.sent.bySession.empty() && w.sent.saves.empty(), "silently: nothing sent, nothing saved");
}

// The sample lua/ (with lua/systems/npcs.lua): the Guard is created at the spawn point, patrols its waypoints through
// "walking" and "waiting", and a player there sees it move. 20 s of ticks.
#ifdef KAST_SAMPLE_MAPS
// The sample lua/ on the sample maps: the Guard and the Bat come from the spawn files; the Guard patrols its path; the
// test hook (systems/test_hooks.lua) destroys it for a "Respawntest..." character and it is back 5 s later.
void checkNpcSample() {
    LogCapture capture;
    const fs::path root = KAST_WORLD_LUA_SAMPLE;
    const auto defs = lua::load_component_defs(root, world::kReservedComponentNames);
    check(defs.errors.empty(), "sample definitions load");
    std::vector<std::string> mapErrors;
    auto maps = world::Maps::load(KAST_SAMPLE_MAPS, mapErrors);
    check(mapErrors.empty(), "sample maps load: " + (mapErrors.empty() ? "" : mapErrors[0]));
    TestWorld w(defs.defs, {}, std::move(maps));
    const auto errors = w.world.load_scripts(root, {"zone_cycle"});
    check(errors.empty(), "sample systems load: " + (errors.empty() ? "" : errors[0]));
    auto& reg = w.world.registry();
    check(reg.view<world::Npc>().empty(), "no NPC before the first tick (no Lua creates any)");
    const auto npc = [&](const std::string& name) {
        for (auto [e, n] : reg.view<world::Npc>().each())
            if (n.name == name) return e;
        return entt::entity{entt::null};
    };
    w.world.tick(T0);
    const auto g = npc("Guard"), bat = npc("Bat"), sentry = npc("Sentry");
    check(reg.view<world::Npc>().size() == 3 && g != entt::null && bat != entt::null && sentry != entt::null,
          "the spawn files place the Guard, the Bat and the Sentry");
    if (g == entt::null || bat == entt::null || sentry == entt::null) return;
    check(reg.get<world::MapRef>(sentry).map == 3, "the Sentry is on the hills (map 3)");
    check(reg.get<world::Npc>(g).templateName == "Guard" && reg.get<world::MapRef>(g).map == 1 && reg.all_of<world::SpawnRef>(g),
          "Guard, template Guard, map 1, from a spawn file");
    check(reg.get<world::MapRef>(bat).map == 2 && reg.get<world::Transform>(bat).pos == proto::Vec3{256, 0, 256},
          "Bat at map 2's spawn");
    w.spawn(1, 141, "Alice", 1024, 1024);
    // Their definitions (lua/npcs/): Health data, and the patrol machine for the two with a path.
    const auto health = [&](entt::entity e) { return w.value(e, "Health"); };
    check(reg.get<world::Npc>(g).npc == "Guard" && reg.get<world::Npc>(sentry).npc == "Sentry" && reg.get<world::Npc>(bat).npc == "Bat",
          "each spawn's NPC is made from its definition");
    check(health(g)["current"] == 50 && health(g)["max"] == 50 && health(g)["lives"] == 3 && health(sentry)["max"] == 80 &&
              health(bat)["current"] == 10 && health(bat)["regen"] == false,
          "the definitions' Health data, unlisted fields at their defaults: " + health(g).dump());
    check(w.value(g, "StateMachine")["machine"] == "patrol" && w.value(sentry, "StateMachine")["machine"] == "patrol" &&
              !lua::pool(reg, "StateMachine").contains(bat),
          "Guard and Sentry get the patrol machine from their definitions, the Bat none");
    std::set<std::string> states;
    float farthest = 0, maxStep = 0;
    proto::Vec3 last = reg.get<world::Transform>(g).pos;
    std::set<std::pair<int, int>> corners;
    // The Sentry (maps/hills/.server/spawns/0_1.json) patrols (150, 331) <-> (215, 331), through the mesa
    // (x 170..200, z 316..346) as the crow flies: on the hills' navmesh it walks round it, on the ground.
    float sentryEnd = 1e9f, sentryOffGround = 0, sentryInWall = 0;
    // And the crate fixture across that line at (160, 331) (maps/hills/objects/0_1.json, a 3 m box): round it too.
    map::Shape crate;
    crate.halfExtents = {1.5f, 1.5f, 1.5f};
    float sentryNearCrate = 1e9f;
    for (int i = 1; i <= 800; ++i) {
        w.world.tick(T0 + std::chrono::milliseconds(50 * i));
        const proto::Vec3 s = reg.get<world::Transform>(sentry).pos;
        sentryEnd = std::min(sentryEnd, std::hypot(s.x - 215, s.z - 331));
        sentryOffGround = std::max(sentryOffGround, std::fabs(s.y - *w.maps.ground(3, s.x, s.z)));
        sentryInWall = std::max(sentryInWall, std::min({s.x - 170, 200 - s.x, s.z - 316, 346 - s.z}));
        sentryNearCrate = std::min(sentryNearCrate, map::footprint_distance(crate, {160, 0, 331}, 0, s.x, s.z));
        const proto::Vec3 p = reg.get<world::Transform>(g).pos;
        farthest = std::max(farthest, std::hypot(p.x - 1024, p.z - 1024));
        maxStep = std::max(maxStep, std::hypot(p.x - last.x, p.z - last.z));
        if (std::abs(std::abs(p.x - 1024) - 10) < 1e-3f && std::abs(std::abs(p.z - 1024) - 10) < 1e-3f)
            corners.insert({int(std::lround(p.x)), int(std::lround(p.z))});
        last = p;
        states.insert(w.world.fsm().state(lua::EntityId{wire(g)}));
    }
    check(states == std::set<std::string>{"walking", "waiting"}, "the patrol machine walks and waits");
    check(corners == std::set<std::pair<int, int>>{{1034, 1034}, {1034, 1014}, {1014, 1014}, {1014, 1034}},
          "it visits the 4 points of gate_patrol (" + std::to_string(corners.size()) + ")");
    check(farthest > 10 && farthest < 15, "it patrols around the spawn point (farthest " + std::to_string(farthest) + " m)");
    check(maxStep <= 3 * 0.05f + 1e-3f, "at 3 m/s (largest step " + std::to_string(maxStep) + " m)");
    check(reg.get<world::Transform>(bat).pos == proto::Vec3{256, 0, 256}, "the Bat (no path) never moves");
    check(sentryEnd < 1 && sentryInWall < 0.5f && sentryOffGround < 0.01f,
          std::format("the Sentry reaches (215, 331) round the mesa ({:.2f} m off; at most {:.2f} m into its 3 m wall's "
                      "foot), on the ground ({:.3f} m off)", sentryEnd, sentryInWall, sentryOffGround));
    check(sentryNearCrate > 0.2f, std::format("the Sentry walks round the obstacle crate at (160, 331) (closest {:.2f} m to its edge)",
                                              sentryNearCrate));
    const auto spawns = w.sent.of<proto::Spawn>(1);
    check(std::ranges::any_of(spawns, [](const proto::Spawn& s) { return s.kind == proto::SpawnKind::npc && s.templateName == "Guard"; }),
          "the player at the spawn point gets the Guard's Spawn");
    {
        const auto& toA = w.sent.bySession[1];
        size_t spawnAt = SIZE_MAX, healthAt = SIZE_MAX;
        for (size_t i = 0; i < toA.size(); ++i) {
            if (const auto* m = std::get_if<proto::Spawn>(&toA[i]); m && m->entityId == wire(g) && spawnAt == SIZE_MAX) spawnAt = i;
            if (const auto* u = std::get_if<proto::ComponentUpdate>(&toA[i]);
                u && u->entityId == wire(g) && u->name == "Health" && healthAt == SIZE_MAX) {
                healthAt = i;
                check(Json::parse(u->json)["current"] == 50, "the Guard's Health from its definition: " + u->json);
            }
        }
        check(spawnAt != SIZE_MAX && healthAt != SIZE_MAX && spawnAt < healthAt,
              "the player gets the Guard's Spawn, then its Health (aoi) in the AOI-enter full set");
    }
    size_t moves = 0;
    for (const auto& m : w.sent.of<proto::EntityMove>(1)) moves += m.entityId == wire(g);
    check(moves > 400, "and its moves (" + std::to_string(moves) + ")");

    // The test hook: a "Respawntest..." character destroys the Guard; respawnSec 5 brings it back.
    const TimePoint t = T0 + std::chrono::milliseconds(50 * 801);
    w.world.tick(t);
    db::CharRow row{142, 1, "RespawntestAbc", 1, {1024, 0, 1024}, 0, ""};
    w.world.spawn(2, row, t);
    check(!reg.valid(g) && npc("Guard") == entt::null, "test hook: the Guard is destroyed on entering");
    w.world.tick(t + std::chrono::milliseconds(4950));
    check(npc("Guard") == entt::null, "test hook: not back after 4.95 s");
    w.world.tick(t + std::chrono::milliseconds(5000));
    const auto again = npc("Guard");
    check(again != entt::null && std::hypot(reg.get<world::Transform>(again).pos.x - 1024, reg.get<world::Transform>(again).pos.z - 1024) < 0.2f,
          "test hook: back at its spawn point after 5 s");
    for (const auto& s : w.world.script_status()) check(s.ok, "sample system enabled: " + s.path + " " + s.error);
    check(!said("FAIL") && log_index("\"level\":\"error\"") == SIZE_MAX, "no Lua errors");
}
#endif

// --- NPC spawns (docs/services/world/npcs.html#npc-spawns) ---

void checkSpawnParsing() {
    std::string err;
    const auto good = map::parse_spawns(Json::parse(R"({"version": 2,
        "paths": {"p": {"loop": false, "wait": 1.5, "points": [[1, 2, 3], [4, 5, 6]]}, "q": {"points": [[0, 0, 0]]}},
        "spawns": [{"id": "a", "npc": "Guard", "pos": [300, 1, 260], "heading": 2, "respawnSec": 30,
                    "path": "p"},
                   {"id": "b", "npc": "Bat", "pos": [256, 0, 256], "respawnSec": -1}]})"),
                                        {1, 1}, err);
    check(good && good->size() == 2, "spawn file parses: " + err);
    if (good && good->size() == 2) {
        const auto& a = (*good)[0];
        check(a.id == "a" && a.npc == "Guard" && a.file.empty() && a.pos == std::array<float, 3>{300, 1, 260} &&
                  a.heading == 2 && a.respawnSec == 30 && a.path && a.path->points.size() == 2 &&
                  a.path->points[1] == std::array<float, 3>{4, 5, 6} && a.path->wait == 1.5f && !a.path->loop,
              "every spawn field and its path");
        const auto& b = (*good)[1];
        check(b.npc == "Bat" && b.heading == 0 && b.respawnSec == -1 && !b.path, "heading defaults to 0; no path");
    }
    const auto q = map::parse_spawns(Json::parse(R"({"version":2,"paths":{"q":{"points":[[0,0,0]]}},
        "spawns":[{"id":"c","npc":"T","pos":[1,0,1],"respawnSec":0,"path":"q"}]})"), {0, 0}, err);
    check(q && (*q)[0].path->wait == 0 && (*q)[0].path->loop, "path wait defaults to 0, loop to true");
    check(map::parse_spawns(Json::parse(R"({"version":2,"spawns":[]})"), {0, 0}, err).has_value(), "no spawns is fine");
    check(map::parse_spawns(Json::parse(R"({"version":2,"spawns":[{"id":"e","npc":"T","pos":[0,0,0],"respawnSec":0}]})"),
                            {0, 0}, err).has_value(), "the tile's low corner is inside it");

    const std::string s = R"("npc":"T","pos":[10,0,10],"respawnSec":5)";
    const std::pair<std::string, const char*> bad[] = {
        {R"([1])", "must be a JSON object"},
        {R"({"spawns":[]})", "\"version\" must be 2"},
        {R"({"version":3,"spawns":[]})", "\"version\" must be 2"},
        {R"({"version":1,"spawns":[]})", "\"version\" 1 is no longer supported: use 2, with \"npc\""},
        {R"({"version":2})", "\"spawns\" must be a list"},
        {R"({"version":2,"paths":[],"spawns":[]})", "\"paths\" must be an object"},
        {R"({"version":2,"paths":{"p":{"points":[]}},"spawns":[]})", "path \"p\": \"points\" must be a non-empty list"},
        {R"({"version":2,"paths":{"p":{"points":[[1,2]]}},"spawns":[]})", "path \"p\": \"points\" must be"},
        {R"({"version":2,"paths":{"p":{"points":[[1,2,3]],"wait":-1}},"spawns":[]})", "path \"p\": \"wait\" must be a number >= 0"},
        {R"({"version":2,"paths":{"p":{"points":[[1,2,3]],"loop":1}},"spawns":[]})", "path \"p\": \"loop\" must be true or false"},
        {R"({"version":2,"spawns":[7]})", "spawns[0]: must be an object"},
        {R"({"version":2,"spawns":[{)" + s + "}]}", "spawns[0]: \"id\" must be a non-empty string"},
        {R"({"version":2,"spawns":[{"id":"",)" + s + "}]}", "spawns[0]: \"id\" must be a non-empty string"},
        {R"({"version":2,"spawns":[{"id":"a",)" + s + R"(},{"id":"a",)" + s + "}]}", "spawn \"a\": duplicate id"},
        {R"({"version":2,"spawns":[{"id":"a","pos":[10,0,10],"respawnSec":5}]})", "spawn \"a\": \"npc\" must be a non-empty string"},
        {R"({"version":2,"spawns":[{"id":"a","npc":"","pos":[10,0,10],"respawnSec":5}]})", "spawn \"a\": \"npc\" must be a non-empty string"},
        {R"({"version":2,"spawns":[{"id":"a","npc":3,"pos":[10,0,10],"respawnSec":5}]})", "spawn \"a\": \"npc\" must be a non-empty string"},
        {R"({"version":2,"spawns":[{"id":"a","npc":")" + std::string(256, 'x') + R"(","pos":[10,0,10],"respawnSec":5}]})",
         "\"npc\" must be a non-empty string of at most 255 bytes"},
        {R"({"version":2,"spawns":[{"id":"a","name":"Guard",)" + s + "}]}", "spawn \"a\": unknown key \"name\""},
        {R"({"version":2,"spawns":[{"id":"a","template":"Guard",)" + s + "}]}", "spawn \"a\": unknown key \"template\""},
        {R"({"version":2,"spawns":[{"id":"a","components":{},)" + s + "}]}", "spawn \"a\": unknown key \"components\""},
        {R"({"version":2,"spawns":[{"id":"a","npc":"T","pos":[10,0],"respawnSec":5}]})", "spawn \"a\": \"pos\" must be [x, y, z]"},
        {R"({"version":2,"spawns":[{"id":"a","npc":"T","pos":[256,0,10],"respawnSec":5}]})",
         "spawn \"a\": \"pos\" is outside tile 0_0"},
        {R"({"version":2,"spawns":[{"id":"a","npc":"T","pos":[-0.5,0,10],"respawnSec":5}]})", "\"pos\" is outside tile 0_0"},
        {R"({"version":2,"spawns":[{"id":"a","heading":"n",)" + s + "}]}", "spawn \"a\": \"heading\" must be a number"},
        {R"({"version":2,"spawns":[{"id":"a","npc":"T","pos":[10,0,10]}]})", "spawn \"a\": \"respawnSec\" must be a number"},
        {R"({"version":2,"spawns":[{"id":"a","npc":"T","pos":[10,0,10],"respawnSec":-2}]})", "\"respawnSec\" must be a number in [0, 31536000]"},
        {R"({"version":2,"spawns":[{"id":"a","npc":"T","pos":[10,0,10],"respawnSec":31536001}]})", "\"respawnSec\" must be"},
        {R"({"version":2,"spawns":[{"id":"a","path":"nope",)" + s + "}]}", "spawn \"a\": unknown path \"nope\""},
        {R"({"version":2,"spawns":[{"id":"a","path":3,)" + s + "}]}", "spawn \"a\": unknown path 3"},
    };
    for (const auto& [text, want] : bad) {
        err.clear();
        const bool rejected = !map::parse_spawns(Json::parse(text), {0, 0}, err);
        check(rejected && contains(err, want), "spawn file rejected: want [" + std::string(want) + "] got [" + err + "]");
    }
}

void checkSpawnLoading() {
    LuaRoot r;
    const auto one = [](const std::string& pos) {
        return R"({"version":2,"spawns":[{"id":"x","npc":"T","pos":[)" + pos + R"(],"respawnSec":5}]})";
    };
    r.write("a/map.json", R"({"version":1,"id":1,"tiles":[[0,0,1,1]],"spawn":{"pos":[10,0,10]}})");
    r.write("a/.server/spawns/0_0.json", one("10,0,10"));
    r.write("a/.server/spawns/-1_0.json", one("-10,0,10"));
    r.write("a/.server/spawns/1_0.json", one("300,0,10"));
    r.write("a/.server/spawns/1_1.json", one("10,0,10"));
    r.write("a/.server/spawns/0_1.json", R"({"version":2,"spawns":[{"id":"y","npc":"T","pos":[10,0,300],"respawnSec":5}]})");
    r.write("a/.server/spawns/notes.txt", "hi");
    r.write("a/.server/spawns/0_0_old.json", one("10,0,10"));
    r.write("b/map.json", R"({"version":1,"id":2,"tiles":[[0,0,0,0]],"spawn":{"pos":[10,0,10]}})");
    r.write("b/.server/spawns/0_0.json", one("10,0,10")); // same id, other map: fine
    std::vector<std::string> errors;
    const auto maps = world::Maps::load(r.root, errors);
    const std::set<std::string> got(errors.begin(), errors.end());
    const std::set<std::string> want = {
        "maps/a/.server/spawns/-1_0.json: tile is not on the map",
        "maps/a/.server/spawns/1_0.json: spawn \"x\": duplicate id on this map",
        "maps/a/.server/spawns/1_1.json: spawn \"x\": \"pos\" is outside tile 1_1",
        "maps/a/.server/spawns/notes.txt: file name must be <tx>_<tz>.json",
        "maps/a/.server/spawns/0_0_old.json: file name must be <tx>_<tz>.json",
    };
    std::string all;
    for (const auto& e : errors) all += e + "; ";
    check(got == want, "every bad spawn file reported: " + all);
    check(maps.info(1).spawns.size() == 2 && maps.info(1).spawns[0].id == "x" && maps.info(1).spawns[1].id == "y" &&
              maps.info(2).spawns.size() == 1,
          "the good spawns load, in file order");

    LuaRoot none;
    none.write("a/map.json", R"({"version":1,"id":1,"tiles":[[0,0,0,0]],"spawn":{"pos":[10,0,10]}})");
    errors.clear();
    check(world::Maps::load(none.root, errors).info(1).spawns.empty() && errors.empty(), "no .server/spawns: no spawns, no error");
}

// The definitions spawn_maps' spawns name: Guard (template GuardTpl) and Bat (template BatTpl).
void write_spawn_npcs(LuaRoot& r) {
    r.write("npcs/guard.lua", "return {name = 'Guard', template = 'GuardTpl'}");
    r.write("npcs/bat.lua", "return {name = 'Bat', template = 'BatTpl'}");
}

// test_maps with spawn "a" (respawn 30 s, a one-point path) and "b" (never respawns, no path) on map 1.
world::Maps spawn_maps() {
    world::Maps maps = test_maps();
    map::MapInfo start = maps.info(1);
    start.spawns = {
        {"a", "Guard", {1024, 0, 1024}, 0.5f, 30, map::PatrolPath{{{1030, 0, 1024}}, 1, false}, "spawns/a.json"},
        {"b", "Bat", {1030, 5000, 1030}, 0, -1, std::nullopt, "spawns/b.json"},
    };
    maps.add(start);
    return maps;
}

void checkRespawn() {
    LogCapture capture;
    LuaRoot r;
    write_spawn_npcs(r);
    TestWorld w({}, {}, spawn_maps());
    check(w.world.load_scripts(r.root).empty(), "respawn: definitions load");
    auto& reg = w.world.registry();
    const auto find = [&](const std::string& name) {
        for (auto [e, n] : reg.view<world::Npc>().each())
            if (n.name == name) return e;
        return entt::entity{entt::null};
    };
    check(reg.view<world::Npc>().empty(), "respawn: nothing before the first tick");
    w.world.tick(T0);
    const auto a = find("Guard"), b = find("Bat");
    check(a != entt::null && b != entt::null, "respawn: every spawn is created on the first tick");
    if (a == entt::null || b == entt::null) return;
    check(reg.get<world::Npc>(a).templateName == "GuardTpl" && reg.get<world::Transform>(a).pos == proto::Vec3{1024, 0, 1024} &&
              reg.get<world::Transform>(a).heading == 0.5f && reg.get<world::MapRef>(a).map == 1 &&
              reg.get<world::SpawnRef>(a).record == 0 && reg.get<world::SpawnRef>(b).record == 1,
          "respawn: name, template, pos, heading, map, record");
    check(reg.get<world::Transform>(b).pos.y == 0, "respawn: pos on the ground like kast.create");
    const TimePoint killed = T0 + std::chrono::seconds(10);
    w.world.tick(killed);
    check(reg.view<world::Npc>().size() == 2, "respawn: alive spawns are not created again");

    w.world.destroy_npc(a);
    w.world.destroy_npc(b);
    w.world.tick(killed + std::chrono::milliseconds(29950));
    check(reg.view<world::Npc>().empty(), "respawn: not back before respawnSec");
    w.world.tick(killed + std::chrono::seconds(30));
    const auto a2 = find("Guard");
    check(a2 != entt::null && reg.get<world::Transform>(a2).pos == proto::Vec3{1024, 0, 1024} && reg.get<world::SpawnRef>(a2).record == 0,
          "respawn: back at its spawn point respawnSec (30 s) after it was destroyed");
    w.world.tick(killed + std::chrono::hours(24 * 400));
    check(reg.view<world::Npc>().size() == 1 && find("Bat") == entt::null, "respawn: respawnSec -1 never comes back");

    // MAX_NPCS: 1 of the 2 due spawns, one warning; the other once there is room.
    world::WorldRules rules;
    rules.maxNpcs = 1;
    TestWorld capped({}, rules, spawn_maps());
    capped.world.load_scripts(r.root);
    capped.world.tick(T0);
    capped.world.tick(T0 + std::chrono::milliseconds(50));
    auto& creg = capped.world.registry();
    check(creg.view<world::Npc>().size() == 1 && said_at("spawns deferred: MAX_NPCS reached").size() == 1,
          "respawn: MAX_NPCS caps the spawner, warned once");
    capped.world.destroy_npc(creg.view<world::Npc>().front());
    capped.world.tick(T0 + std::chrono::milliseconds(100));
    check(creg.view<world::Npc>().size() == 1 && creg.get<world::Npc>(creg.view<world::Npc>().front()).name == "Bat",
          "respawn: the deferred spawn is created once there is room");
}

constexpr const char* kSpawnLuaSystem = R"(
local done = false
return { update = function()
    if done then return end
    done = true
    local seen = {}
    for e in kast.query("Spawn") do
        expect(kast.has(e, "Npc") and kast.has(e, "Spawn"), "a Spawn entity is an NPC")
        local s = kast.get(e, "Spawn")
        seen[s.id] = s
    end
    local a, b = seen.a, seen.b
    expect(a and b, "query Spawn finds both spawns")
    expect(#a.path == 1 and a.path[1].x == 1030 and a.path[1].y == 0 and a.path[1].z == 1024, "path points as {x, y, z}")
    expect(a.wait == 1 and a.loop == false, "wait and loop")
    expect(a.pos.x == 1024 and a.pos.y == 0 and a.pos.z == 1024 and a.heading == 0.5, "pos and heading as in the file")
    expect(b.pos.y == 5000 and b.heading == 0, "pos is the file's, not clamped onto the ground")
    expect(b.path == nil and b.wait == nil and b.loop == nil, "no path: no path, wait or loop")
    local plain = kast.create("Plain", {template = "T"})
    expect(not kast.has(plain, "Spawn"), "kast.create NPCs have no Spawn")
    local n = 0
    for _ in kast.query("Spawn") do n = n + 1 end
    expect(n == 2, "query Spawn skips them")
    raises("has no \"Spawn\"", function() kast.get(plain, "Spawn") end)
    for e in kast.query("Spawn") do
        raises("\"Spawn\" is read-only on the server", function() kast.set(e, "Spawn", {id = "x"}) end)
        raises("\"Spawn\" cannot be removed", function() kast.remove(e, "Spawn") end)
    end
    raises("\"Spawn\" is read-only on the server", function() kast.add(plain, "Spawn") end)
    kast.log("spawn lua done")
end }
)";

void checkSpawnLua() {
    LogCapture capture;
    LuaRoot r;
    r.write("systems/spawns.lua", std::string(kLuaExpect) + kSpawnLuaSystem);
    write_spawn_npcs(r);
    TestWorld w({}, {}, spawn_maps());
    check(w.world.load_scripts(r.root).empty(), "spawn lua: loads");
    w.world.tick(T0);
    check_lua_expectations("spawn lua", "spawn lua done");
    for (const auto& s : w.world.script_status()) check(s.ok, "spawn lua: system still enabled " + s.error);
}

// ClientEvent in (kast.world.send): "client.<name>" {entity, data} on the bus after this tick's update, in arrival
// order; past 10 a second per character the rest are dropped and counted (ClientEventLimit).
void checkClientEventsIn() {
    LogCapture capture;
    LuaRoot r;
    r.write("components/report.lua", kReport);
    r.write("systems/inbox.lua", R"(
local got = {}
return {
    init = function()
        kast.subscribe("client.combat.attack", function(e)
            got[#got + 1] = string.format("%d %s %s", e.entity, tostring(e.data.target), kast.get(e.entity, "Character").name)
        end)
    end,
    onEnter = function(e) kast.add(e, "Report") end,
    update = function(dt) for e in kast.query("Report") do kast.set(e, "Report", {lines = got}) end end,
})");
    TestWorld w(r.defs().defs);
    check(w.world.load_scripts(r.root).empty(), "client events: system loads");
    const auto alice = w.spawn(1, 31, "Alice", 1000, 1000);
    for (int i = 0; i < 15; ++i) w.world.push_client_event(alice, "combat.attack", Json{{"target", i}});
    w.world.tick(T0);
    w.world.tick(T0 + std::chrono::milliseconds(50)); // the report is written in the update after the drain
    Json lines = w.value(alice, "Report").value("lines", Json::array());
    const std::string id = std::to_string(wire(alice));
    check(lines.size() == 10 && lines[0] == id + " 0 Alice" && lines[9] == id + " 9 Alice",
          "client.<name> {entity = sender, data}, the first 10 a second, in order: " + lines.dump());
    auto& limit = w.world.registry().get<world::ClientEventLimit>(alice);
    check(limit.dropped == 5, "client events: the 5 over the limit are counted");
    for (int i = 20; i < 23; ++i) w.world.push_client_event(alice, "combat.attack", Json{{"target", i}});
    w.world.tick(T0 + std::chrono::seconds(1));
    w.world.tick(T0 + std::chrono::milliseconds(1050));
    lines = w.value(alice, "Report").value("lines", Json::array());
    check(lines.size() == 13 && lines[12] == id + " 22 Alice" && limit.dropped == 5,
          "client events: allowed again a second later: " + lines.dump());
    w.world.push_client_event(alice, "unheard", Json::object()); // nobody subscribed: dropped by the bus, no error
    w.world.tick(T0 + std::chrono::milliseconds(1100));
    w.world.despawn(alice);
    check(said_at("client events dropped").size() == 1, "client events: the dropped count is logged on despawn");
    for (const auto& s : w.world.script_status()) check(s.ok, "client events: system still enabled " + s.error);
}

// Move seq / epoch (protocol v6): a rejection sends one Correction{pos, the rejected seq, epoch + 1}; the rest of its
// batch and every later Move of the old epoch (in flight when the Correction went out) are dropped without another.
void checkMoveEpochs() {
    TestWorld w({});
    const auto alice = w.spawn(1, 41, "Alice", 1000, 1000);
    w.world.tick(T0);
    w.sent.clear();
    const auto& tf = w.world.registry().get<world::Transform>(alice);
    const auto& ms = w.world.registry().get<world::MoveState>(alice);
    const auto move = [&](float x, uint32_t seq, uint32_t epoch) { w.world.push_move(alice, {{x, 0, 1000}, 0, 0, seq, epoch}); };
    move(1001, 1, 0);
    w.world.tick(T0 + std::chrono::milliseconds(500));
    check(tf.pos.x == 1001 && w.sent.of<proto::Correction>(1).empty() && ms.epoch == 0, "epochs: a valid move, no Correction");
    move(1100, 2, 0); // a teleport
    move(1101, 3, 0); // sent right after it, same epoch
    w.world.tick(T0 + std::chrono::milliseconds(1000));
    auto corr = w.sent.of<proto::Correction>(1);
    check(corr.size() == 1 && corr[0].pos == proto::Vec3{1001, 0, 1000} && corr[0].ackSeq == 2 && corr[0].epoch == 1 &&
              ms.epoch == 1 && tf.pos.x == 1001,
          "epochs: a rejection -> one Correction{last accepted pos, ackSeq of the rejected move, epoch 1}; the rest of the batch dropped");
    w.sent.clear();
    move(1102, 4, 0); // in flight when the Correction went out
    move(1103, 5, 0);
    w.world.tick(T0 + std::chrono::milliseconds(1100));
    check(w.sent.of<proto::Correction>(1).empty() && tf.pos.x == 1001, "epochs: stale-epoch moves dropped silently, not applied");
    move(1003, 6, 1); // the client rebased: 1001 + the 2 m it walked since
    w.world.tick(T0 + std::chrono::milliseconds(1500));
    check(w.sent.of<proto::Correction>(1).empty() && tf.pos.x == 1003, "epochs: a move of the new epoch is accepted");
    move(1200, 7, 1);
    move(1004, 8, 0); // an older epoch than the current one in the same batch: ignored, not a second rejection
    w.world.tick(T0 + std::chrono::milliseconds(2000));
    corr = w.sent.of<proto::Correction>(1);
    check(corr.size() == 1 && corr[0].ackSeq == 7 && corr[0].epoch == 2 && corr[0].pos.x == 1003 && ms.epoch == 2,
          "epochs: the next rejection -> ackSeq 7, epoch 2");
}

// kast.setMovable: a locked character's moves are all refused with one Correction a tick; unlocked, they work again.
// kast.map.spawn(map): the map's spawn point.
void checkMovableAndMapSpawn() {
    LogCapture capture;
    LuaRoot r;
    r.write("systems/lock.lua", std::string(kLuaExpect) + R"(
local ticks = 0
return { update = function()
    ticks = ticks + 1
    local e
    for c in kast.query("Character") do e = c end
    if ticks == 1 then
        kast.setMovable(e, false)
        local npc = kast.create("Statue", {template = "T"})
        raises("kast.setMovable: entity " .. npc .. " is an NPC; only players send moves", function() kast.setMovable(npc, false) end)
        raises("kast.setMovable: movable must be a boolean, got nil", function() kast.setMovable(e) end)
        raises("invalid entity id 999999", function() kast.setMovable(999999, true) end)
        local s = kast.map.spawn(2)
        expect(s.position.x == 256 and s.position.y == 0 and s.position.z == 256 and s.heading == 1, "kast.map.spawn")
        raises("kast.map.spawn: ", function() kast.map.spawn(99) end)
    elseif ticks == 2 then
        kast.setMovable(e, true)
        kast.log("lock lua done")
    end
end })");
    TestWorld w({});
    check(w.world.load_scripts(r.root).empty(), "movable: loads");
    const auto alice = w.spawn(1, 41, "Alice", 1000, 1000);
    w.world.tick(T0); // locks
    w.sent.clear();
    w.move(alice, proto::Move{{1001, 0, 1000}, 0, 0});
    w.move(alice, proto::Move{{1002, 0, 1000}, 0, 0});
    w.world.tick(T0 + std::chrono::milliseconds(500)); // refused, then unlocks
    const auto& tf = w.world.registry().get<world::Transform>(alice);
    check(tf.pos.x == 1000 && w.sent.of<proto::Correction>(1).size() == 1, "movable: locked moves -> one Correction, no move");
    w.move(alice, proto::Move{{1001, 0, 1000}, 0, 0});
    w.world.tick(T0 + std::chrono::milliseconds(1000));
    check(tf.pos.x == 1001, "movable: unlocked moves are accepted again");
    check_lua_expectations("movable", "lock lua done");
    for (const auto& s : w.world.script_status()) check(s.ok, "movable: system still enabled " + s.error);
}

#ifdef KAST_STARTER_MMO
// The starter-mmo sample's combat (samples/starter-mmo/systems/combat.lua, classes.lua, regen.lua, npcs.lua, fsms/), on
// its own maps, with characters from before classes (Warriors: Strike, 12 damage, 140 Health): refused attacks, a hit
// and its cooldown, the NPC fighting back and returning home healed, an NPC killed and respawned, a character killed
// (no moves, no attacks) and revived at the map spawn with its class's Health, regeneration only out of combat.
void checkStarterMmoCombat() {
    LogCapture capture;
    const fs::path root = KAST_STARTER_MMO;
    const auto defs = lua::load_component_defs(root, world::kReservedComponentNames);
    check(defs.errors.empty(), "starter-mmo: definitions load");
    std::vector<std::string> mapErrors;
    auto maps = world::Maps::load(root / "maps", mapErrors);
    check(mapErrors.empty(), "starter-mmo: maps load " + (mapErrors.empty() ? "" : mapErrors[0]));
    world::WorldRules rules;
    rules.startMap = 10;
    TestWorld w(defs.defs, rules, std::move(maps));
    const auto errors = w.world.load_scripts(root);
    check(errors.empty(), "starter-mmo: systems load " + (errors.empty() ? "" : errors[0]));
    if (!errors.empty()) return;
    auto& reg = w.world.registry();
    TimePoint t = T0;
    w.world.tick(t);
    const auto npc = [&](const std::string& name) {
        for (auto [e, n] : reg.view<world::Npc>().each())
            if (n.name == name) return e;
        return entt::entity{entt::null};
    };
    const auto run = [&](double seconds) {
        for (int i = 0; i < static_cast<int>(std::lround(seconds * 20)); ++i) w.world.tick(t += std::chrono::milliseconds(50));
    };
    const auto health = [&](entt::entity e) { return w.value(e, "Health")["current"].get<double>(); };
    const auto attack = [&](entt::entity from, const nlohmann::json& target) {
        w.world.push_client_event(from, "combat.power", nlohmann::json{{"power", "strike"}, {"target", target}});
    };
    const auto events = [&](uint32_t sid, const std::string& name) {
        std::vector<Json> out;
        for (const auto& e : w.sent.of<proto::ServerEvent>(sid))
            if (e.name == name) out.push_back(Json::parse(e.json));
        return out;
    };
    const auto rejected = [&](uint32_t sid) {
        std::vector<std::string> out;
        for (const auto& e : events(sid, "combat.rejected")) out.push_back(e["reason"]);
        return out;
    };
    const auto state = [&](entt::entity e) { return w.world.fsm().state(lua::EntityId{wire(e)}); };
    const auto bat = npc("Bat"), guard = npc("Guard");
    check(bat != entt::null && guard != entt::null, "starter-mmo: the Bat and the Guard spawn");
    if (bat == entt::null || guard == entt::null) return;
    check(w.value(bat, "StateMachine")["machine"] == "creature" && state(bat) == "idle",
          "starter-mmo: the Bat stands idle (creature machine)");

    // Refused: out of range (8 m), not an NPC, no such entity, not a number.
    const auto alice = w.spawn(1, 501, "Alice", 128, 128, "", 11);
    const auto bob = w.spawn(2, 502, "Bob", 128, 122, R"({"Health":{"current":50,"max":100}})", 11);
    run(0.1);
    attack(alice, wire(bat));
    attack(alice, wire(bob));
    attack(alice, 999999);
    attack(alice, "x");
    run(0.1);
    check(health(bat) == 10 && rejected(1) == std::vector<std::string>{"range", "target", "target", "target"},
          "starter-mmo: out of range / a player / unknown / not a number refused: " + Json(rejected(1)).dump());
    // The same as a kast.world.request (requestId 7): kast.reply's Reply to the caster alone, no combat.rejected.
    w.sent.clear();
    w.world.push_client_event(alice, "combat.power", nlohmann::json{{"power", "strike"}, {"target", wire(bat)}}, 7);
    run(0.1);
    check(w.sent.of<proto::Reply>(1) == std::vector<proto::Reply>{{7, false, R"("range")"}} && rejected(1).empty() &&
              w.sent.of<proto::Reply>(2).empty() && health(bat) == 10,
          "starter-mmo: a refused request gets Reply{its id, rejected, the reason}, to the caster only");

    // Bob 2 m from the Bat: one hit kills it; both see the hit and the death; its corpse stays 60 s (Bob never loots
    // it), then goes, and the Bat is back respawnSec (30 s) after that. Bob asks with a request (requestId 8): the
    // same hit, plus Reply{8, ok, {power}} before the events.
    w.sent.clear();
    w.world.push_client_event(bob, "combat.power", nlohmann::json{{"power", "strike"}, {"target", wire(bat)}}, 8);
    w.world.tick(t += std::chrono::milliseconds(50));
    check(w.sent.of<proto::Reply>(2) == std::vector<proto::Reply>{{8, true, R"({"power":"strike"})"}} &&
              w.sent.of<proto::Reply>(1).empty() && [&] {
                  for (const auto& m : w.sent.bySession[2]) { // the first of the Reply and the ServerEvents
                      if (std::holds_alternative<proto::Reply>(m)) return true;
                      if (std::holds_alternative<proto::ServerEvent>(m)) return false;
                  }
                  return false;
              }(),
          "starter-mmo: a request used gets Reply{its id, ok, {power}} before its events, to the caster only");
    const double bobAfterHit = health(bob);
    const auto hits = events(1, "combat.hit");
    const auto used = events(2, "combat.power");
    check(hits.size() == 1 && hits[0]["attacker"] == wire(bob) && hits[0]["target"] == wire(bat) && hits[0]["damage"] == 12 &&
              hits[0]["attackerName"] == "Bob" && hits[0]["targetName"] == "Bat" && hits[0]["power"] == "Strike" &&
              used.size() == 1 && used[0]["caster"] == wire(bob) && used[0]["power"] == "strike" && used[0]["effect"] == "melee" &&
              used[0]["target"] == wire(bat),
          "starter-mmo: combat.power and combat.hit {attacker, target, damage, names, power} to everyone in view");
    check(events(1, "combat.death").size() == 1 && events(2, "combat.death")[0]["name"] == "Bat" && health(bat) == 0,
          "starter-mmo: the Bat at 0 Health dies (combat.death)");
    const TimePoint killed = t;
    w.world.tick(t += std::chrono::milliseconds(50));
    check(npc("Bat") == bat && state(bat) == "dead" && w.value(bat, "Corpse")["owner"] == wire(bob) &&
              w.sent.of<proto::Despawn>(1).empty(),
          "starter-mmo: the dead Bat stays as a corpse (dead, Corpse owner Bob)");
    attack(bob, wire(bat));
    run(0.1);
    check(rejected(2) == std::vector<std::string>{"target_dead"}, "starter-mmo: a corpse can't be attacked " + Json(rejected(2)).dump());
    run(4);
    check(health(bob) == bobAfterHit, "starter-mmo: no regeneration within 5 s of a hit");
    run(2);
    check(health(bob) > bobAfterHit, "starter-mmo: regeneration once 5 s out of combat");
    check(reg.get<world::Transform>(bat).pos == reg.get<world::Transform>(bat).pos && state(bat) == "dead",
          "starter-mmo: the corpse stays dead");
    run(53.5);
    check(npc("Bat") == bat, "starter-mmo: the corpse is still there before 60 s");
    run(1);
    check(npc("Bat") == entt::null && !w.sent.of<proto::Despawn>(1).empty() && events(2, "loot.closed").size() == 1,
          "starter-mmo: the unlooted corpse goes after 60 s (loot.closed to its owner)");
    const TimePoint gone = t;
    run(29);
    check(npc("Bat") == entt::null, "starter-mmo: not back before respawnSec (30 s) after the corpse went");
    run(1.5);
    const auto bat2 = npc("Bat");
    check(bat2 != entt::null && bat2 != bat && health(bat2) == 10 && t - gone >= std::chrono::seconds(30) &&
              t - killed >= std::chrono::seconds(90),
          "starter-mmo: the Bat respawns 30 s after its corpse went, with full Health");

    // Carol 1.2 m from the Guard: a hit, a second within the cooldown refused, a third after it.
    const proto::Vec3 g0 = reg.get<world::Transform>(guard).pos; // somewhere on its patrol by now
    const auto carol = w.spawn(3, 503, "Carol", g0.x + 1.2f, g0.z, "", 10);
    w.world.tick(t += std::chrono::milliseconds(50));
    w.sent.clear();
    attack(carol, wire(guard));
    w.world.tick(t += std::chrono::milliseconds(50));
    attack(carol, wire(guard));
    w.world.tick(t += std::chrono::milliseconds(50));
    check(health(guard) == 38 && rejected(3) == std::vector<std::string>{"cooldown"} && state(guard) == "combat",
          "starter-mmo: 12 damage, a second hit within 1.5 s refused, the Guard in combat: " + Json(rejected(3)).dump());
    run(1.5);
    attack(carol, wire(guard));
    run(0.1);
    check(health(guard) == 26, "starter-mmo: hit again after the cooldown");
    // The Guard hits back for 5 every 2 s: at once, then 2 s and 4 s later.
    run(2.6);
    std::vector<double> guardHits;
    for (const auto& h : events(3, "combat.hit"))
        if (h["attacker"] == wire(guard)) guardHits.push_back(h["damage"]);
    check(guardHits == std::vector<double>{5, 5, 5} && health(carol) == 125,
          "starter-mmo: the Guard hits back 5 every 2 s: " + Json(guardHits).dump() + " Carol " + std::to_string(health(carol)));
    // Carol leaves: the Guard returns to its spawn, heals to full and patrols again.
    w.world.despawn(carol);
    run(0.1);
    check(state(guard) == "returning", "starter-mmo: attacker gone -> the Guard is returning, not " + state(guard));
    run(5);
    check(health(guard) == 50 && (state(guard) == "walking" || state(guard) == "waiting"),
          "starter-mmo: back home the Guard is healed and patrols again (" + state(guard) + ")");

    // Dave, 3 Health, hits the Guard: it kills him. Dead: moves refused, attacks refused; revived 5 s later at the
    // map's spawn point with his class's full Health (a Warrior's 140).
    const proto::Vec3 g = reg.get<world::Transform>(guard).pos;
    const auto dave = w.spawn(4, 504, "Dave", g.x + 1.5f, g.z, R"({"Health":{"current":3,"max":100}})", 10);
    w.world.tick(t += std::chrono::milliseconds(50));
    w.sent.clear();
    attack(dave, wire(guard));
    run(0.2);
    const auto deaths = events(4, "combat.death");
    check(health(dave) == 0 && deaths.size() == 1 && deaths[0]["entity"] == wire(dave) && deaths[0]["name"] == "Dave",
          "starter-mmo: Dave at 0 Health dies (combat.death)");
    const proto::Vec3 deadAt = reg.get<world::Transform>(dave).pos;
    w.move(dave, proto::Move{{deadAt.x + 1, deadAt.y, deadAt.z}, 0, 0});
    attack(dave, wire(guard));
    run(0.1);
    check(reg.get<world::Transform>(dave).pos == deadAt && !w.sent.of<proto::Correction>(4).empty() &&
              rejected(4) == std::vector<std::string>{"dead"},
          "starter-mmo: dead -> move corrected, attack refused");
    run(2);
    check(state(guard) != "combat", "starter-mmo: the Guard drops a dead target");
    check(health(dave) == 0, "starter-mmo: no regeneration while dead");
    run(3);
    const proto::Vec3 dp = reg.get<world::Transform>(dave).pos;
    check(health(dave) == 140 && std::hypot(dp.x - 128, dp.z - 128) < 0.01f && events(4, "combat.revive").size() == 1 &&
              !w.sent.of<proto::MapChange>(4).empty(),
          "starter-mmo: revived 5 s later at the map spawn (128, 128) with a Warrior's full 140 Health");
    w.move(dave, proto::Move{{dp.x + 0.5f, *w.maps.ground(10, dp.x + 0.5f, dp.z), dp.z}, 0, 0});
    run(0.5);
    check(reg.get<world::Transform>(dave).pos.x == dp.x + 0.5f, "starter-mmo: moves accepted again after the revive");
    // Walking does not drain Health any more.
    const double walked = health(dave);
    for (int i = 1; i <= 20; ++i) {
        const float x = dp.x + 0.5f + 0.2f * static_cast<float>(i);
        w.move(dave, proto::Move{{x, *w.maps.ground(10, x, dp.z), dp.z}, 0, 0});
        w.world.tick(t += std::chrono::milliseconds(50));
    }
    check(health(dave) == walked, "starter-mmo: walking does not drain Health");

    // Leash: Dave hits the Guard and runs off at 8 m/s; chasing at 5 m/s it gives up 30 m from its spawn point and
    // goes home healed.
    const auto walkTo = [&](entt::entity e, float x, float z, float speed) {
        for (int i = 0; i < 2000; ++i) {
            const proto::Vec3 p = reg.get<world::Transform>(e).pos;
            const float dx = x - p.x, dz = z - p.z, d = std::hypot(dx, dz), step = speed * 0.05f;
            if (d < 0.01f) return;
            const float k = std::min(1.0f, step / d), nx = p.x + dx * k, nz = p.z + dz * k;
            w.move(e, proto::Move{{nx, *w.maps.ground(10, nx, nz), nz}, std::atan2(dx, dz), 0});
            w.world.tick(t += std::chrono::milliseconds(50));
        }
    };
    w.sent.clear();
    for (int tries = 0; tries < 10 && state(guard) != "combat"; ++tries) { // it walks on meanwhile: catch up
        const proto::Vec3 gp = reg.get<world::Transform>(guard).pos;
        walkTo(dave, gp.x + 1.5f, gp.z, 8);
        attack(dave, wire(guard));
        w.world.tick(t += std::chrono::milliseconds(50));
    }
    check(state(guard) == "combat" && health(guard) == 38, "starter-mmo: leash: the Guard fights Dave (" + state(guard) + ", " +
                                                               std::to_string(health(guard)) + ", " + Json(rejected(4)).dump() + ")");
    const proto::Vec3 d0 = reg.get<world::Transform>(dave).pos;
    float farthest = 0;
    bool returned = false;
    for (int leg = 0; leg < 2 && !returned; ++leg) { // 50 m south, then on if needed
        walkTo(dave, d0.x, d0.z - 50.0f * static_cast<float>(leg + 1), 8);
        const proto::Vec3 gp = reg.get<world::Transform>(guard).pos;
        farthest = std::max(farthest, std::hypot(gp.x - 134, gp.z - 122));
        returned = state(guard) == "returning";
    }
    check(returned && farthest <= 31.0f, "starter-mmo: leash: returning once 30 m from its spawn (" +
                                             std::to_string(farthest) + " m), not " + state(guard));
    run(8);
    check(health(guard) == 50 && (state(guard) == "walking" || state(guard) == "waiting"),
          "starter-mmo: leash: home, healed, patrolling (" + state(guard) + ")");
    for (const auto& st : w.world.script_status()) check(st.ok, "starter-mmo: system still enabled " + st.path + " " + st.error);
}
#endif

#ifdef KAST_STARTER_MMO_TEST_LUA
// The starter-mmo sample's bag and loot (samples/starter-mmo/systems/inventory.lua, components/inventory.lua,
// corpse.lua, items/, npcs/ loot), in a copy of its server Lua with the test-only cmake/starter_mmo_test_server.lua
// (test.seed, test.bag). Seed 1 makes the Bat drop 1 gold, a Bat Wing and Linen Cloth.
void checkStarterMmoLoot() {
    LogCapture capture;
    const fs::path sample = KAST_STARTER_MMO;
    LuaRoot r;
    for (const char* dir : {"components", "fsms", "items", "npcs", "systems"})
        fs::copy(sample / dir, r.root / dir, fs::copy_options::recursive);
    fs::copy_file(KAST_STARTER_MMO_TEST_LUA, r.root / "systems" / "zz_test.lua");
    const auto defs = lua::load_component_defs(r.root, world::kReservedComponentNames);
    std::vector<std::string> mapErrors;
    world::WorldRules rules;
    rules.startMap = 10;
    TestWorld w(defs.defs, rules, world::Maps::load(sample / "maps", mapErrors));
    const auto errors = w.world.load_scripts(r.root);
    check(defs.errors.empty() && errors.empty(), "starter-mmo loot: loads " + (errors.empty() ? std::string() : errors[0]));
    if (!errors.empty()) return;
    auto& reg = w.world.registry();
    TimePoint t = T0;
    w.world.tick(t);
    const auto run = [&](double seconds) {
        for (int i = 0; i < std::max(1, static_cast<int>(std::lround(seconds * 20))); ++i)
            w.world.tick(t += std::chrono::milliseconds(50));
    };
    const auto bat = [&] {
        for (auto [e, n] : reg.view<world::Npc>().each())
            if (n.name == "Bat") return e;
        return entt::entity{entt::null};
    };
    const auto send = [&](entt::entity from, const std::string& name, const Json& data) {
        w.world.push_client_event(from, name, nlohmann::json::parse(data.dump()));
        run(0.11); // under the 10 client events a second limit
    };
    const auto events = [&](uint32_t sid, const std::string& name) {
        std::vector<Json> out;
        for (const auto& e : w.sent.of<proto::ServerEvent>(sid))
            if (e.name == name) out.push_back(Json::parse(e.json));
        return out;
    };
    const auto rejected = [&](uint32_t sid) {
        std::vector<std::string> out;
        for (const auto& e : events(sid, "inventory.rejected")) out.push_back(e["reason"]);
        w.sent.clear();
        return out;
    };
    const auto bag = [&](entt::entity e) { // "item:count" per non-empty slot, by slot
        const Json v = w.value(e, "Inventory");
        std::vector<std::string> out;
        for (size_t i = 0; i < 16; ++i)
            out.push_back(v["items"][i].get<std::string>().empty() ? "" : v["items"][i].get<std::string>() + ":" +
                                                                                  std::to_string(v["counts"][i].get<int>()));
        while (!out.empty() && out.back().empty()) out.pop_back();
        return out;
    };
    using Bag = std::vector<std::string>;
    const auto setBag = [&](entt::entity e, const Bag& slots) {
        Json items = Json::array(), counts = Json::array();
        for (const std::string& s : slots) {
            const size_t colon = s.find(':');
            items.push_back(s.empty() ? "" : s.substr(0, colon));
            counts.push_back(s.empty() ? 0 : std::stoi(s.substr(colon + 1)));
        }
        send(e, "test.bag", Json{{"items", items}, {"counts", counts}});
    };
    const auto gold = [&](entt::entity e) { return w.value(e, "Wallet")["gold"].get<int>(); };
    const auto alive = [&](entt::entity e) { return reg.valid(e) && reg.all_of<world::Npc>(e); };

    // Every character gets an empty 16-slot bag; a saved bag with an unknown item and a bad count is cleaned on login.
    const auto ann = w.spawn(1, 601, "Ann", 128, 122, "", 11); // 2 m north of the Bat
    const auto ben = w.spawn(2, 602, "Ben", 128, 117, R"({"Inventory":{"items":["gone","linen_cloth"],"counts":[3,99]}})", 11);
    run(0.1);
    check(w.value(ann, "Inventory")["items"].size() == 16 && bag(ann).empty(), "starter-mmo loot: a new character has an empty 16-slot bag");
    check(bag(ben) == Bag{"", "linen_cloth:20"}, "starter-mmo loot: a saved bag drops unknown items and caps counts: " + Json(bag(ben)).dump());

    // Bag requests.
    setBag(ann, {"bat_wing:5", "linen_cloth:3", "", "bat_wing:18", "worn_shortsword:1"});
    send(ann, "inventory.move", {{"from", 1}, {"to", 3}, {"item", "bat_wing"}});
    check(bag(ann) == Bag{"", "linen_cloth:3", "bat_wing:5", "bat_wing:18", "worn_shortsword:1"}, "starter-mmo loot: move to an empty slot");
    send(ann, "inventory.move", {{"from", 2}, {"to", 3}, {"item", "linen_cloth"}});
    check(bag(ann) == Bag{"", "bat_wing:5", "linen_cloth:3", "bat_wing:18", "worn_shortsword:1"}, "starter-mmo loot: swap two items");
    send(ann, "inventory.move", {{"from", 2}, {"to", 4}, {"item", "bat_wing"}});
    check(bag(ann) == Bag{"", "bat_wing:3", "linen_cloth:3", "bat_wing:20", "worn_shortsword:1"},
          "starter-mmo loot: merge up to maxStack (20), the rest stays: " + Json(bag(ann)).dump());
    send(ann, "inventory.move", {{"from", 3}, {"to", 6}, {"item", "linen_cloth"}, {"split", true}});
    check(bag(ann) == Bag{"", "bat_wing:3", "linen_cloth:2", "bat_wing:20", "worn_shortsword:1", "linen_cloth:1"},
          "starter-mmo loot: split moves floor(n/2) to an empty slot");
    send(ann, "inventory.destroy", {{"slot", 6}, {"item", "linen_cloth"}});
    check(bag(ann) == Bag{"", "bat_wing:3", "linen_cloth:2", "bat_wing:20", "worn_shortsword:1"}, "starter-mmo loot: destroy");
    rejected(1);
    const Bag before = bag(ann);
    for (const Json& bad : {Json{{"from", 0}, {"to", 2}, {"item", "x"}}, Json{{"from", 2}, {"to", 17}, {"item", "bat_wing"}},
                            Json{{"from", 2}, {"to", 2}, {"item", "bat_wing"}}, Json{{"from", 1.5}, {"to", 2}, {"item", "bat_wing"}},
                            Json{{"from", "2"}, {"to", 3}}, Json{{"from", 1}, {"to", 2}, {"item", ""}},
                            Json{{"from", 2}, {"to", 3}, {"item", "linen_cloth"}},
                            Json{{"from", 5}, {"to", 7}, {"item", "worn_shortsword"}, {"split", true}},
                            Json{{"from", 2}, {"to", 3}, {"item", "bat_wing"}, {"split", true}},
                            Json{{"from", 2}, {"to", 4}, {"item", "bat_wing"}}, Json("nope")})
        send(ann, "inventory.move", bad);
    send(ann, "inventory.destroy", {{"slot", 1}, {"item", ""}});
    send(ann, "inventory.destroy", {{"slot", 2}, {"item", "linen_cloth"}});
    send(ann, "inventory.destroy", {{"slot", 99}});
    const auto reasons = rejected(1);
    check(bag(ann) == before &&
              reasons == std::vector<std::string>{"slot", "slot", "slot", "slot", "slot", "empty", "item", "split",
                                                      "split", "stack_full", "slot", "empty", "item", "slot"},
          "starter-mmo loot: malformed and illegal bag requests change nothing, each rejected with its reason: " +
              Json(reasons).dump() + " " + Json(bag(ann)).dump());

    // Ann kills the Bat (seed 1: 1 gold, a Bat Wing, Linen Cloth): only she may loot it, from within 5 m.
    send(ann, "test.seed", {{"seed", 1}});
    const auto b = bat();
    send(ann, "combat.power", {{"power", "strike"}, {"target", wire(b)}});
    run(0.1);
    check(w.value(b, "Corpse")["owner"] == wire(ann), "starter-mmo loot: the tagger owns the corpse");
    send(ben, "loot.open", {{"target", wire(b)}});
    send(ben, "loot.take_all", {{"target", wire(b)}});
    check(rejected(2) == std::vector<std::string>{"not_owner", "not_owner"} && events(2, "loot.contents").empty(),
          "starter-mmo loot: another player can't open or take the loot");
    const proto::Vec3 annAt = reg.get<world::Transform>(ann).pos;
    const auto moveTo = [&](entt::entity e, float x, float z) {
        for (int i = 0; i < 400; ++i) {
            const proto::Vec3 p = reg.get<world::Transform>(e).pos;
            const float dx = x - p.x, dz = z - p.z, d = std::hypot(dx, dz);
            if (d < 0.01f) return;
            const float k = std::min(1.0f, 0.4f / d), nx = p.x + dx * k, nz = p.z + dz * k;
            w.move(e, proto::Move{{nx, *w.maps.ground(11, nx, nz), nz}, 0, 0});
            w.world.tick(t += std::chrono::milliseconds(50));
        }
    };
    moveTo(ann, annAt.x, annAt.z + 6); // 8 m from the corpse
    send(ann, "loot.open", {{"target", wire(b)}});
    send(ann, "loot.open", {{"target", 424242}});
    send(ann, "loot.open", {{"target", "x"}});
    check(rejected(1) == std::vector<std::string>{"range", "no_loot", "no_loot"}, "starter-mmo loot: out of range / no corpse refused");
    moveTo(ann, annAt.x, annAt.z);
    send(ann, "loot.open", {{"target", wire(b)}});
    const auto opened = events(1, "loot.contents");
    check(opened.size() == 1 && opened[0]["gold"] == 1 &&
              nlohmann::json(opened[0]["items"]) == nlohmann::json::parse(R"([{"item":"bat_wing","count":1},{"item":"linen_cloth","count":1}])"),
          "starter-mmo loot: seed 1 rolls 1 gold, a Bat Wing and Linen Cloth: " + (opened.empty() ? "-" : opened[0].dump()));
    send(ann, "loot.take", {{"target", wire(b)}, {"index", 3}});
    send(ann, "loot.take", {{"target", wire(b)}, {"index", "x"}});
    check(rejected(1) == std::vector<std::string>{"index", "index"}, "starter-mmo loot: a bad index is refused");
    const int goldBefore = gold(ann);
    send(ann, "loot.take", {{"target", wire(b)}, {"index", "gold"}});
    check(gold(ann) == goldBefore + 1 && events(1, "loot.contents").back()["gold"] == 0, "starter-mmo loot: gold to the Wallet");
    send(ann, "loot.take", {{"target", wire(b)}, {"index", "gold"}});
    check(rejected(1) == std::vector<std::string>{"index"}, "starter-mmo loot: no gold left to take");

    // A full bag: the Bat Wing merges onto the 3 Bat Wings, the Linen Cloth merges onto the 2; then with no room left
    // the cloth stays in the corpse and Ann hears "full".
    setBag(ann, {"bat_wing:3", "linen_cloth:20", "linen_cloth:20", "linen_cloth:20", "linen_cloth:20", "linen_cloth:20",
                 "linen_cloth:20", "linen_cloth:20", "linen_cloth:20", "linen_cloth:20", "linen_cloth:20", "linen_cloth:20",
                 "linen_cloth:20", "linen_cloth:20", "linen_cloth:20", "linen_cloth:20"});
    send(ann, "loot.take_all", {{"target", wire(b)}});
    check(bag(ann)[0] == "bat_wing:4" && rejected(1) == std::vector<std::string>{"full"} && alive(b),
          "starter-mmo loot: the wing merges into its stack; the cloth doesn't fit: full, it stays");
    setBag(ann, {"bat_wing:4", "linen_cloth:19"});
    send(ann, "loot.take", {{"target", wire(b)}, {"index", 1}});
    check(bag(ann) == Bag{"bat_wing:4", "linen_cloth:20"} && events(1, "loot.closed").size() == 1,
          "starter-mmo loot: the last item merges onto a stack; the emptied corpse closes");
    run(0.1);
    check(!alive(b), "starter-mmo loot: the emptied corpse is gone");

    // The bag is saved with the character.
    w.world.despawn(ann);
    bool saved = false;
    for (const auto& save : w.sent.saves)
        if (save.charId == 601 && save.components)
            saved = saved || (contains(*save.components, "\"Inventory\"") && contains(*save.components, "linen_cloth"));
    check(saved, "starter-mmo loot: the bag is saved with the character");
    for (const auto& st : w.world.script_status()) check(st.ok, "starter-mmo loot: system still enabled " + st.path + " " + st.error);
}

// The starter-mmo sample's classes and powers (samples/starter-mmo/systems/classes.lua, combat.lua, npcs.lua,
// components/class.lua), in a copy of its server Lua with the test-only system (test.seed makes Evasion's rolls
// known). Dummies are NPCs with 1000 Health made here; with the "Guard" definition and a creature machine they fight
// back like the Guard (5 every 2 s).
void checkStarterMmoClasses() {
    LogCapture capture;
    const fs::path sample = KAST_STARTER_MMO;
    LuaRoot r;
    for (const char* dir : {"components", "fsms", "items", "npcs", "systems"})
        fs::copy(sample / dir, r.root / dir, fs::copy_options::recursive);
    fs::copy_file(KAST_STARTER_MMO_TEST_LUA, r.root / "systems" / "zz_test.lua");
    const auto defs = lua::load_component_defs(r.root, world::kReservedComponentNames);
    std::vector<std::string> mapErrors;
    world::WorldRules rules;
    rules.startMap = 10;
    TestWorld w(defs.defs, rules, world::Maps::load(sample / "maps", mapErrors));
    const auto errors = w.world.load_scripts(r.root);
    check(defs.errors.empty() && errors.empty(), "starter-mmo classes: loads " + (errors.empty() ? std::string() : errors[0]));
    if (!errors.empty()) return;
    auto& reg = w.world.registry();
    TimePoint t = T0;
    w.world.tick(t);
    const auto tick = [&] { w.world.tick(t += std::chrono::milliseconds(50)); };
    const auto run = [&](double seconds) {
        for (int i = 0; i < std::max(1, static_cast<int>(std::lround(seconds * 20))); ++i) tick();
    };
    const auto use = [&](entt::entity from, const Json& data) {
        w.world.push_client_event(from, "combat.power", nlohmann::json::parse(data.dump()));
        tick();
    };
    const auto power = [&](entt::entity from, const std::string& id, entt::entity target = entt::null) {
        Json data{{"power", id}};
        if (target != entt::null) data["target"] = wire(target);
        use(from, data);
    };
    const auto health = [&](entt::entity e) { return w.value(e, "Health")["current"].get<double>(); };
    const auto maxHealth = [&](entt::entity e) { return w.value(e, "Health")["max"].get<double>(); };
    const auto events = [&](uint32_t sid, const std::string& name) {
        std::vector<Json> out;
        for (const auto& e : w.sent.of<proto::ServerEvent>(sid))
            if (e.name == name) out.push_back(Json::parse(e.json));
        return out;
    };
    const auto rejected = [&](uint32_t sid) {
        std::vector<std::string> out;
        for (const auto& e : events(sid, "combat.rejected")) out.push_back(e["reason"]);
        return out;
    };
    // Damage of the hits on target seen by session sid, in order (attacker: only its hits).
    const auto hitsOn = [&](uint32_t sid, entt::entity target, entt::entity attacker = entt::null) {
        std::vector<double> out;
        for (const auto& h : events(sid, "combat.hit"))
            if (h["target"] == wire(target) && (attacker == entt::null || h["attacker"] == wire(attacker)))
                out.push_back(h["damage"]);
        return out;
    };
    const auto dummy = [&](float x, float z, float heading = 0, bool fights = false) {
        Json components{{"Health", {{"current", 1000}, {"max", 1000}, {"regen", true}}}};
        if (fights) components["StateMachine"] = {{"machine", "creature"}, {"initialState", ""}};
        return w.world.create_npc("Dummy", "Dummy", 10, proto::Vec3{x, 0, z}, heading, fights ? "Guard" : "",
                                  lua::Json::parse(components.dump()));
    };
    const auto pos = [&](entt::entity e) { return reg.get<world::Transform>(e).pos; };
    const auto as = [](const char* c) { return proto::CharOptions{{"class", c}}; };
    using Reasons = std::vector<std::string>;
    using Hits = std::vector<double>;

    // The creation hook: exactly one option, class, one of the four.
    check(w.world.validate_character("Ann", as("warrior")) && w.world.validate_character("Ann", as("mage")) &&
              w.world.validate_character("Ann", as("priest")) && w.world.validate_character("Ann", as("rogue")),
          "starter-mmo classes: each of the four classes is accepted");
    check(!w.world.validate_character("Ann", {}) && !w.world.validate_character("Ann", as("paladin")) &&
              !w.world.validate_character("Ann", as("Mage")) &&
              !w.world.validate_character("Ann", {{"class", "mage"}, {"hair", "red"}}),
          "starter-mmo classes: no class, an unknown class and an extra option are refused");

    // Entering: Class from the creation options, Health from the class; one from before classes becomes a Warrior.
    const auto war = w.spawn(1, 701, "War", 60, 60, "", 10, as("warrior"));
    const auto mag = w.spawn(2, 702, "Mag", 60, 80, "", 10, as("mage"));
    const auto pri = w.spawn(3, 703, "Pri", 60, 100, R"({"Health":{"current":70,"max":90}})", 10, as("priest"));
    const auto rog = w.spawn(4, 704, "Rog", 60, 120, "", 10, as("rogue"));
    const auto old = w.spawn(5, 705, "Old", 60, 140, R"({"Health":{"current":100,"max":100}})", 10);
    run(0.1);
    const auto classOf = [&](entt::entity e) { return w.value(e, "Class")["id"].get<std::string>(); };
    check(classOf(war) == "warrior" && classOf(mag) == "mage" && classOf(pri) == "priest" && classOf(rog) == "rogue" &&
              classOf(old) == "warrior",
          "starter-mmo classes: Class from the creation options; none -> warrior");
    check(health(war) == 140 && maxHealth(war) == 140 && health(mag) == 80 && maxHealth(mag) == 80 && health(pri) == 70 &&
              maxHealth(pri) == 90 && health(rog) == 110 && maxHealth(rog) == 110 && health(old) == 100 && maxHealth(old) == 140,
          "starter-mmo classes: max Health 140 / 80 / 90 / 110; new characters full, saved Health kept");
    const auto bar = events(2, "class.powers");
    check(bar.size() == 1 && bar[0]["class"] == "mage" && bar[0]["gcd"] == 1 && bar[0]["powers"].size() == 3 &&
              bar[0]["powers"][0]["id"] == "firebolt" && bar[0]["powers"][0]["key"] == 1 && bar[0]["powers"][0]["cooldown"] == 2 &&
              bar[0]["powers"][1]["id"] == "frost_nova" && bar[0]["powers"][1]["name"] == "Frost Nova" &&
              bar[0]["powers"][2]["id"] == "arcane_blast" && bar[0]["powers"][2]["effect"] == "bolt",
          "starter-mmo classes: the owner gets class.powers (its 3 powers, keys 1-3, cooldowns, the 1 s GCD): " +
              (bar.empty() ? std::string() : bar[0].dump()));
    const std::array owners{war, mag, pri, rog, old};
    bool ownerOnly = true;
    for (uint32_t sid = 1; sid <= 5; ++sid)
        for (const auto& u : w.sent.of<proto::ComponentUpdate>(sid))
            if (u.name == "Class" && u.entityId != wire(owners[sid - 1])) ownerOnly = false;
    check(ownerOnly, "starter-mmo classes: Class is replicated to its owner only");
    w.world.despawn(old);
    const std::string saved = w.sent.saves.empty() || !w.sent.saves.back().components ? "{}" : *w.sent.saves.back().components;
    check(Json::parse(saved).value("Class", Json::object()).value("id", "") == "warrior",
          "starter-mmo classes: the Warrior a character from before classes became is saved: " + saved);
    const auto relogged = w.spawn(5, 705, "Old", 60, 140, saved, 10);
    run(0.1);
    check(classOf(relogged) == "warrior" && maxHealth(relogged) == 140, "starter-mmo classes: ... and kept after a relog");

    // Malformed and refused requests change nothing (the first valid power afterwards still works).
    const auto d1 = dummy(60, 62.5f);
    const auto tooFar = dummy(60, 63.5f); // 3.5 m
    run(0.1);
    w.sent.clear();
    power(war, "firebolt", d1);
    power(war, "fireball", d1);
    use(war, Json{{"power", 5}});
    w.world.push_client_event(war, "combat.power", nlohmann::json::object());
    tick();
    power(war, "strike");
    use(war, Json{{"power", "strike"}, {"target", "x"}});
    power(war, "strike", pri);
    power(war, "strike", tooFar);
    check(rejected(1) == Reasons{"wrong_class", "unknown_power", "unknown_power", "unknown_power", "target", "target", "target", "range"} &&
              health(d1) == 1000 && health(tooFar) == 1000 && events(1, "combat.power").empty(),
          "starter-mmo classes: wrong class, unknown, malformed, no / bad / player target, out of range refused, nothing used: " +
              Json(rejected(1)).dump());
    run(1.1); // under the 10 client events a second limit

    // Warrior: Strike 12 and Heroic Slam 25 in melee; the 1 s global cooldown, then each power's own.
    w.sent.clear();
    power(war, "strike", d1);       // t 0
    power(war, "heroic_slam", d1);  // within the GCD
    run(1.0);
    power(war, "strike", d1);       // after the GCD, within Strike's 1.5 s
    power(war, "heroic_slam", d1);
    run(1.5);
    power(war, "strike", d1);
    check(hitsOn(1, d1) == Hits{12, 25, 12} && rejected(1) == Reasons{"gcd", "cooldown"},
          "starter-mmo classes: Strike 12, Heroic Slam 25, GCD and cooldown refused: " + Json(hitsOn(1, d1)).dump() + " " +
              Json(rejected(1)).dump());

    // Mage: Firebolt 14 and Arcane Blast 30 at 20 m, Firebolt refused at 26 m; Frost Nova roots NPCs within 8 m.
    const auto d3 = dummy(80, 80), d4 = dummy(86, 80), inNova = dummy(67.5f, 80), outOfNova = dummy(68.5f, 80);
    run(0.1);
    w.sent.clear();
    power(mag, "firebolt", d3);
    run(1.05);
    power(mag, "arcane_blast", d3);
    run(1.05);
    power(mag, "firebolt", d4);
    const auto bolts = events(2, "combat.power");
    check(hitsOn(2, d3, mag) == Hits{14, 30} && rejected(2) == Reasons{"range"} && bolts.size() == 2 &&
              bolts[0]["effect"] == "bolt" && bolts[0]["target"] == wire(d3) && health(d4) == 1000,
          "starter-mmo classes: Firebolt 14, Arcane Blast 30 at 20 m; 26 m out of range: " + Json(rejected(2)).dump());
    size_t seenByOthers = 0;
    for (const auto& e : events(1, "combat.power")) seenByOthers += e["caster"] == wire(mag) && e["effect"] == "bolt" ? 1 : 0;
    check(seenByOthers == 2, "starter-mmo classes: another player in view gets the caster's combat.power (to draw its effect)");
    run(1.05);
    w.sent.clear();
    power(mag, "frost_nova");
    const auto nova = events(2, "combat.power");
    check(nova.size() == 1 && nova[0]["effect"] == "nova" && nova[0]["rooted"] == Json::array({wire(inNova)}) &&
              health(inNova) == 1000 && health(outOfNova) == 1000,
          "starter-mmo classes: Frost Nova roots the NPC at 7.5 m, not the one at 8.5 m, no damage: " +
              (nova.empty() ? std::string() : nova[0].dump()));

    // A rooted NPC that fights: it can't chase for 4 s, then does; one in reach still hits.
    const auto mag2 = w.spawn(6, 706, "Magtwo", 160, 60, "", 10, as("mage"));
    const auto chaser = dummy(160, 66, 0, true); // 6 m away
    const auto biter = dummy(162, 60, 0, true);  // 2 m away
    run(0.1);
    w.sent.clear();
    power(mag2, "firebolt", biter);
    run(1.05);
    power(mag2, "frost_nova");
    const proto::Vec3 rootedAt = pos(chaser);
    w.sent.clear();
    run(1.05);
    power(mag2, "arcane_blast", chaser); // it would chase now
    run(1.5);
    check(pos(chaser) == rootedAt && w.world.fsm().state(lua::EntityId{wire(chaser)}) == "combat",
          "starter-mmo classes: the rooted NPC is in combat but stays put");
    check(!hitsOn(6, mag2, biter).empty(), "starter-mmo classes: a rooted NPC in reach still hits");
    run(2.5); // the root (4 s from the nova) is over
    check(std::hypot(pos(chaser).x - rootedAt.x, pos(chaser).z - rootedAt.z) > 1,
          "starter-mmo classes: after 4 s the NPC chases again");

    // The Meadow's patrolling Guard, rooted by a Frost Nova next to it, stops walking for 4 s.
    entt::entity guard = entt::null;
    for (auto [e, npcRef] : reg.view<world::Npc>().each())
        if (npcRef.name == "Guard") guard = e;
    const proto::Vec3 gp = pos(guard);
    const auto mag4 = w.spawn(22, 722, "Magfour", gp.x + 2, gp.z, "", 10, as("mage"));
    run(0.1);
    power(mag4, "frost_nova");
    tick(); // npcs.lua hears "npc.rooted" after this tick's walk
    const proto::Vec3 patrolAt = pos(guard);
    run(3.7);
    const proto::Vec3 stillAt = pos(guard);
    run(3);
    const std::string patrolState = w.world.fsm().state(lua::EntityId{wire(guard)});
    check(patrolState != "combat" && stillAt == patrolAt && (pos(guard).x != patrolAt.x || pos(guard).z != patrolAt.z),
          std::format("starter-mmo classes: a rooted patrol stands for 4 s, then walks on ({}; {},{} -> {},{} -> {},{})",
                      patrolState, patrolAt.x, patrolAt.z, stillAt.x, stillAt.z, pos(guard).x, pos(guard).z));
    w.world.despawn(mag4);

    // Priest: Smite 10 at 20 m (which holds off regeneration); Heal +40, capped at max, then its 10 s cooldown; Power
    // Shield expires after 10 s.
    const auto d7 = dummy(80, 100);
    const auto low = w.spawn(23, 723, "Prilow", 100, 110, R"({"Health":{"current":30,"max":90}})", 10, as("priest"));
    const auto high = w.spawn(24, 724, "Prihigh", 100, 100, R"({"Health":{"current":70,"max":90}})", 10, as("priest"));
    run(0.1);
    w.sent.clear();
    power(pri, "smite", d7);
    power(low, "smite", d7);
    power(high, "smite", d7);
    run(1.05);
    const double lowBefore = health(low), highBefore = health(high);
    power(low, "heal");
    power(high, "heal");
    double highAmount = -1;
    for (const auto& e : events(24, "combat.power"))
        if (e["caster"] == wire(high) && e["effect"] == "heal") highAmount = e["amount"];
    check(hitsOn(3, d7) == Hits{10, 10, 10} && health(low) == lowBefore + 40 && health(high) == 90 &&
              highAmount == 90 - highBefore && highAmount < 40,
          std::format("starter-mmo classes: Smite 10; Heal +40 ({} -> {}), capped at max ({} -> 90)", lowBefore, health(low),
                      highBefore));
    run(1.05);
    power(low, "heal");
    check(rejected(23) == Reasons{"cooldown"}, "starter-mmo classes: Heal's 10 s cooldown");
    run(1.05);
    w.sent.clear();
    power(pri, "power_shield");
    run(9.8);
    check(events(3, "combat.fade").empty(), "starter-mmo classes: Power Shield holds for 10 s");
    run(0.3);
    const auto faded = events(3, "combat.fade");
    check(faded.size() == 1 && faded[0]["power"] == "power_shield" && faded[0]["name"] == "Power Shield",
          "starter-mmo classes: ... then fades (combat.fade)");

    // Power Shield under two fighting dummies: it soaks exactly 30, breaks, and later hits land in full.
    const auto pri2 = w.spawn(7, 707, "Pritwo", 160, 160, "", 10, as("priest"));
    const auto ga = dummy(162, 160, 0, true), gb = dummy(158, 160, 0, true);
    run(0.1);
    w.sent.clear();
    power(pri2, "power_shield");
    run(1.05);
    power(pri2, "smite", ga);
    run(1.55);
    power(pri2, "smite", gb);
    run(7);
    double absorbed = 0;
    size_t brokeAt = 0, n = 0;
    bool fullAfter = true;
    for (const auto& h : events(7, "combat.hit")) {
        if (h["target"] != wire(pri2)) continue;
        ++n;
        if (h.contains("absorbed")) {
            absorbed += h["absorbed"].get<double>();
            if (h["absorbedBy"] != "Power Shield") fullAfter = false;
            if (absorbed >= 30 && !brokeAt) brokeAt = n;
        } else if (!brokeAt || h["damage"] != 5) {
            fullAfter = false;
        }
    }
    check(absorbed == 30 && brokeAt == 6 && n > 6 && fullAfter && events(7, "combat.fade").size() == 1 &&
              health(pri2) == 90 - 5.0 * static_cast<double>(n - 6),
          "starter-mmo classes: Power Shield absorbs 30 (6 hits of 5), breaks, then hits land: absorbed " +
              std::to_string(absorbed) + " in " + std::to_string(brokeAt) + " of " + std::to_string(n) + " hits");

    // Warrior: Shield Wall halves NPC damage (5 -> 2, rounded down) for 6 s.
    const auto war2 = w.spawn(8, 708, "Wartwo", 200, 60, "", 10, as("warrior"));
    const auto gw = dummy(202, 60, 0, true);
    run(0.1);
    w.sent.clear();
    power(war2, "shield_wall");
    run(1.05);
    power(war2, "strike", gw); // it hits back at once, then every 2 s
    run(4.5);
    const Hits walled = hitsOn(8, war2, gw);
    run(4);
    const Hits all = hitsOn(8, war2, gw);
    check(walled == Hits{2, 2, 2} && all.size() > walled.size() && all.back() == 5 && events(8, "combat.fade").size() == 1,
          "starter-mmo classes: Shield Wall: 2 per hit for 6 s, then 5: " + Json(all).dump());

    // Rogue: Sinister Strike 11; Backstab 30 only from behind (the dummies face +z: z below one is behind it).
    const auto front = dummy(60, 118), back = dummy(60, 122); // Rog stands at (60, 120)
    run(0.1);
    w.sent.clear();
    power(rog, "backstab", front);
    power(rog, "backstab", back);
    run(1.05);
    power(rog, "sinister_strike", front);
    check(hitsOn(4, back) == Hits{30} && hitsOn(4, front) == Hits{11} && rejected(4) == Reasons{"not_behind"},
          "starter-mmo classes: Backstab 30 from behind, refused in front; Sinister Strike 11: " + Json(rejected(4)).dump());

    // Evasion: five rogues, each with a fighting dummy; about half of the hits within its 8 s miss (seeded), none after.
    w.world.push_client_event(rog, "test.seed", nlohmann::json{{"seed", 7}});
    run(0.1);
    std::vector<entt::entity> rogues, biters;
    for (int i = 0; i < 5; ++i) {
        const float x = 100 + 10.0f * static_cast<float>(i);
        rogues.push_back(w.spawn(10 + i, 710 + i, std::string("Rogue") + static_cast<char>('a' + i), x, 200, "", 10, as("rogue")));
        biters.push_back(dummy(x, 202, 0, true));
    }
    run(0.1);
    w.sent.clear();
    for (auto e : rogues) power(e, "evasion");
    run(1.0);
    for (int i = 0; i < 5; ++i) power(rogues[i], "sinister_strike", biters[i]);
    run(6.4); // the Evasions end 8 s after they were cast
    size_t dodged = 0, landed = 0;
    for (uint32_t i = 0; i < 5; ++i) {
        for (const auto& d : events(10 + i, "combat.dodge")) dodged += d["target"] == wire(rogues[i]) ? 1 : 0;
        landed += hitsOn(10 + i, rogues[i], biters[i]).size();
    }
    check(dodged + landed >= 15 && dodged >= 5 && landed >= 5,
          "starter-mmo classes: Evasion dodges about half the hits: " + std::to_string(dodged) + " dodged, " +
              std::to_string(landed) + " landed");
    run(1);
    w.sent.clear();
    run(6);
    size_t later = 0;
    for (uint32_t i = 0; i < 5; ++i) later += events(10 + i, "combat.dodge").size();
    check(later == 0 && !hitsOn(10, rogues[0], biters[0]).empty(), "starter-mmo classes: no dodges once Evasion fades");

    // Tagging and loot with any class's power: a Mage's Firebolt kills the Cavern's Bat; its corpse is the Mage's.
    const auto mag3 = w.spawn(20, 720, "Magthree", 128, 140, "", 11, as("mage")); // 20 m from the Bat
    run(0.1);
    entt::entity bat = entt::null;
    for (auto [e, npcRef] : reg.view<world::Npc>().each())
        if (npcRef.name == "Bat") bat = e;
    power(mag3, "firebolt", bat);
    run(0.1);
    check(bat != entt::null && health(bat) == 0 && w.value(bat, "Corpse").value("owner", 0u) == wire(mag3),
          "starter-mmo classes: a Mage's Firebolt kills the Bat, tagged: the corpse is the Mage's");

    // Dead: no powers.
    const auto frail = w.spawn(21, 721, "Frail", 240, 60, R"({"Health":{"current":4,"max":80}})", 10, as("mage"));
    const auto killer = dummy(242, 60, 0, true);
    run(0.1);
    power(frail, "firebolt", killer);
    run(0.2);
    w.sent.clear();
    power(frail, "frost_nova");
    check(health(frail) == 0 && rejected(21) == Reasons{"dead"}, "starter-mmo classes: a dead caster is refused");
    run(5.1);
    check(health(frail) == 80, "starter-mmo classes: revived with the class's 80 Health");
    for (const auto& st : w.world.script_status()) check(st.ok, "starter-mmo classes: system still enabled " + st.path + " " + st.error);
}

// The starter-mmo sample's equipment (samples/starter-mmo/components/equipment.lua, systems/inventory.lua's equip
// rules and GEAR, combat.lua's Armor and Damage, classes.lua's Stamina), in a copy of its server Lua with the test-only
// system (test.bag). Gear: Leather Cap head 1 armor 1 stamina, Linen Tunic chest 2/2, Patched Trousers legs 1/1, Worn
// Boots feet 1/1, Cloth Gloves hands 1/1, Worn Shortsword main hand 3 damage, Wooden Shield off hand 2/1.
void checkStarterMmoEquipment() {
    LogCapture capture;
    const fs::path sample = KAST_STARTER_MMO;
    LuaRoot r;
    for (const char* dir : {"components", "fsms", "items", "npcs", "systems"})
        fs::copy(sample / dir, r.root / dir, fs::copy_options::recursive);
    fs::copy_file(KAST_STARTER_MMO_TEST_LUA, r.root / "systems" / "zz_test.lua");
    const auto defs = lua::load_component_defs(r.root, world::kReservedComponentNames);
    std::vector<std::string> mapErrors;
    world::WorldRules rules;
    rules.startMap = 10;
    TestWorld w(defs.defs, rules, world::Maps::load(sample / "maps", mapErrors));
    const auto errors = w.world.load_scripts(r.root);
    check(defs.errors.empty() && errors.empty(), "starter-mmo equipment: loads " + (errors.empty() ? std::string() : errors[0]));
    if (!errors.empty()) return;
    TimePoint t = T0;
    w.world.tick(t);
    const auto tick = [&] { w.world.tick(t += std::chrono::milliseconds(50)); };
    const auto run = [&](double seconds) {
        for (int i = 0; i < std::max(1, static_cast<int>(std::lround(seconds * 20))); ++i) tick();
    };
    const auto send = [&](entt::entity from, const std::string& name, const Json& data) {
        w.world.push_client_event(from, name, nlohmann::json::parse(data.dump()));
        run(0.11); // under the 10 client events a second limit
    };
    const auto events = [&](uint32_t sid, const std::string& name) {
        std::vector<Json> out;
        for (const auto& e : w.sent.of<proto::ServerEvent>(sid))
            if (e.name == name) out.push_back(Json::parse(e.json));
        return out;
    };
    using Reasons = std::vector<std::string>;
    const auto rejected = [&](uint32_t sid) {
        Reasons out;
        for (const auto& e : events(sid, "inventory.rejected")) out.push_back(e["reason"]);
        w.sent.clear();
        return out;
    };
    using Slots = std::vector<std::string>;
    const auto bag = [&](entt::entity e) { // "item:count" per non-empty slot, by slot
        const Json v = w.value(e, "Inventory");
        Slots out;
        for (size_t i = 0; i < 16; ++i)
            out.push_back(v["items"][i].get<std::string>().empty() ? "" : v["items"][i].get<std::string>() + ":" +
                                                                                  std::to_string(v["counts"][i].get<int>()));
        while (!out.empty() && out.back().empty()) out.pop_back();
        return out;
    };
    const auto gear = [&](entt::entity e) { // item id per slot, trailing empties dropped
        Slots out = w.value(e, "Equipment")["items"].get<Slots>();
        while (!out.empty() && out.back().empty()) out.pop_back();
        return out;
    };
    const auto setBag = [&](entt::entity e, const Slots& slots) {
        Json items = Json::array(), counts = Json::array();
        for (const std::string& s : slots) {
            const size_t colon = s.find(':');
            items.push_back(s.empty() ? "" : s.substr(0, colon));
            counts.push_back(s.empty() ? 0 : std::stoi(s.substr(colon + 1)));
        }
        send(e, "test.bag", Json{{"items", items}, {"counts", counts}});
    };
    const auto equip = [&](entt::entity e, int from, int slot, const std::string& item) {
        send(e, "equipment.equip", {{"from", from}, {"slot", slot}, {"item", item}});
    };
    const auto unequip = [&](entt::entity e, int slot, int to, const std::string& item) { // to 0: none
        Json data{{"slot", slot}, {"item", item}};
        if (to) data["to"] = to;
        send(e, "equipment.unequip", data);
    };
    const auto health = [&](entt::entity e) { return w.value(e, "Health")["current"].get<double>(); };
    const auto maxHealth = [&](entt::entity e) { return w.value(e, "Health")["max"].get<double>(); };
    const auto hitsOn = [&](uint32_t sid, entt::entity target) {
        std::vector<double> out;
        for (const auto& h : events(sid, "combat.hit"))
            if (h["target"] == wire(target)) out.push_back(h["damage"]);
        return out;
    };
    const auto dummy = [&](float x, float z, bool fights = false) {
        Json components{{"Health", {{"current", 1000}, {"max", 1000}, {"regen", true}}}};
        if (fights) components["StateMachine"] = {{"machine", "creature"}, {"initialState", ""}};
        return w.world.create_npc("Dummy", "Dummy", 10, proto::Vec3{x, 0, z}, 0, fights ? "Guard" : "",
                                  lua::Json::parse(components.dump()));
    };
    const auto as = [](const char* c) { return proto::CharOptions{{"class", c}}; };
    using Hits = std::vector<double>;

    // Items: the sword and six new gear items, one per other slot, all maxStack 1 with an icon; four in the Guard's
    // loot, two in the Bat's.
    const std::array<const char*, 7> gearIds{"leather_cap", "linen_tunic", "patched_trousers", "worn_boots",
                                             "cloth_gloves", "worn_shortsword", "wooden_shield"};
    const auto text = [](const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    const std::string loot = text(sample / "npcs" / "guard.lua") + text(sample / "npcs" / "bat.lua");
    bool itemsOk = true;
    for (const char* id : gearIds)
        itemsOk = itemsOk && contains(text(sample / "items" / (std::string(id) + ".lua")), "maxStack = 1,") &&
                  fs::exists(sample / "textures" / "icons" / (std::string(id) + ".png")) &&
                  contains(loot, std::string("\"") + id + "\"");
    check(itemsOk, "starter-mmo equipment: 7 gear items, maxStack 1, icons, all in NPC loot");

    // Entering: an empty 7-slot Equipment, the gear table for the client; a saved one drops unknown and misplaced items.
    const auto ann = w.spawn(1, 801, "Ann", 60, 60, "", 10, as("warrior"));
    const auto ben = w.spawn(2, 802, "Ben", 60, 80,
                             R"({"Equipment":{"items":["gone","worn_shortsword","linen_tunic","","","worn_shortsword",""]}})",
                             10, as("warrior"));
    run(0.1);
    const auto table = events(1, "equipment.gear");
    check(w.value(ann, "Equipment")["items"] == Json(Slots(7, "")) && table.size() == 1 &&
              table[0]["slots"] == Json::parse(R"(["head","chest","legs","feet","hands","main_hand","off_hand"])") &&
              table[0]["names"][5] == "Main Hand" && table[0]["gear"]["worn_shortsword"]["slot"] == "main_hand" &&
              table[0]["gear"]["worn_shortsword"]["damage"] == 3 && table[0]["gear"]["linen_tunic"]["armor"] == 2,
          "starter-mmo equipment: a new character has 7 empty slots and gets equipment.gear: " +
              (table.empty() ? std::string("-") : table[0].dump()));
    check(gear(ben) == Slots{"", "", "", "", "", "worn_shortsword"},
          "starter-mmo equipment: a saved Equipment drops an unknown item and items in the wrong slot: " + Json(gear(ben)).dump());
    bool ownerOnly = true;
    for (uint32_t sid = 1; sid <= 2; ++sid)
        for (const auto& u : w.sent.of<proto::ComponentUpdate>(sid))
            if (u.name == "Equipment" && u.entityId != wire(sid == 1 ? ann : ben)) ownerOnly = false;
    check(ownerOnly, "starter-mmo equipment: Equipment is replicated to its owner only");

    // Equip and unequip, with both swaps.
    setBag(ann, {"worn_shortsword:1", "linen_tunic:1", "worn_shortsword:1", "bat_wing:5"});
    rejected(1);
    equip(ann, 1, 6, "worn_shortsword");
    check(gear(ann) == Slots{"", "", "", "", "", "worn_shortsword"} && bag(ann) == Slots{"", "linen_tunic:1", "worn_shortsword:1", "bat_wing:5"},
          "starter-mmo equipment: equip from the bag into its slot");
    equip(ann, 2, 2, "linen_tunic");
    equip(ann, 3, 6, "worn_shortsword"); // onto the equipped sword: the old one goes to bag slot 3
    check(gear(ann) == Slots{"", "linen_tunic", "", "", "", "worn_shortsword"} &&
              bag(ann) == Slots{"", "", "worn_shortsword:1", "bat_wing:5"} && rejected(1).empty(),
          "starter-mmo equipment: equip onto an occupied slot swaps the old item into the bag slot: " + Json(bag(ann)).dump());
    unequip(ann, 2, 8, "linen_tunic");
    check(gear(ann) == Slots{"", "", "", "", "", "worn_shortsword"} &&
              bag(ann) == Slots{"", "", "worn_shortsword:1", "bat_wing:5", "", "", "", "linen_tunic:1"},
          "starter-mmo equipment: unequip onto an empty bag slot");
    unequip(ann, 6, 3, "worn_shortsword"); // onto the other sword: swapped
    check(gear(ann) == Slots{"", "", "", "", "", "worn_shortsword"} &&
              bag(ann) == Slots{"", "", "worn_shortsword:1", "bat_wing:5", "", "", "", "linen_tunic:1"} && rejected(1).empty(),
          "starter-mmo equipment: unequip onto same-slot gear in the bag swaps them");
    unequip(ann, 6, 0, "worn_shortsword");
    check(gear(ann).empty() && bag(ann) == Slots{"worn_shortsword:1", "", "worn_shortsword:1", "bat_wing:5", "", "", "", "linen_tunic:1"},
          "starter-mmo equipment: unequip without a bag slot goes to the first empty one: " + Json(bag(ann)).dump());

    // Refusals change nothing.
    equip(ann, 1, 6, "worn_shortsword");
    rejected(1);
    const Slots bagBefore = bag(ann), gearBefore = gear(ann);
    equip(ann, 8, 1, "linen_tunic");      // a chest item onto the head
    equip(ann, 4, 6, "bat_wing");         // not gear
    unequip(ann, 6, 8, "worn_shortsword"); // onto the tunic: other gear
    unequip(ann, 6, 4, "worn_shortsword"); // onto the Bat Wings
    const Reasons placed = rejected(1);
    check(placed == Reasons{"wrong_slot", "wrong_slot", "occupied", "occupied"} && bag(ann) == bagBefore && gear(ann) == gearBefore,
          "starter-mmo equipment: wrong slot and an occupied bag slot refused: " + Json(placed).dump());
    for (const Json& bad : {Json{{"from", 0}, {"slot", 1}, {"item", "x"}}, Json{{"from", 17}, {"slot", 1}, {"item", "x"}},
                            Json{{"from", 8}, {"slot", 0}, {"item", "linen_tunic"}}, Json{{"from", 8}, {"slot", 8}, {"item", "linen_tunic"}},
                            Json{{"from", 8}, {"slot", 1.5}, {"item", "linen_tunic"}}, Json{{"from", "8"}, {"slot", 2}},
                            Json("nope"), Json{{"from", 2}, {"slot", 2}, {"item", ""}},
                            Json{{"from", 8}, {"slot", 2}, {"item", "leather_cap"}}})
        send(ann, "equipment.equip", bad);
    for (const Json& bad : {Json{{"slot", 9}, {"item", "x"}}, Json{{"slot", 6}, {"to", 17}, {"item", "worn_shortsword"}},
                            Json{{"slot", 6}, {"to", "x"}, {"item", "worn_shortsword"}}, Json(5),
                            Json{{"slot", 1}, {"item", ""}}, Json{{"slot", 6}, {"item", "leather_cap"}}})
        send(ann, "equipment.unequip", bad);
    const Reasons bad = rejected(1);
    check(bad == Reasons{"slot", "slot", "slot", "slot", "slot", "slot", "slot", "empty", "item", "slot", "slot", "slot", "slot",
                         "empty", "item"} &&
              bag(ann) == bagBefore && gear(ann) == gearBefore,
          "starter-mmo equipment: bad indices, empty slots and a mismatched item refused: " + Json(bad).dump());
    Slots full(16, "linen_cloth:20");
    full[3] = "worn_shortsword:1";
    full[7] = "linen_tunic:1";
    setBag(ann, full);
    rejected(1);
    unequip(ann, 6, 0, "worn_shortsword");  // no empty slot
    unequip(ann, 6, 1, "worn_shortsword");  // onto Linen Cloth in a full bag
    unequip(ann, 6, 8, "worn_shortsword");  // onto the tunic in a full bag
    unequip(ann, 6, 4, "worn_shortsword");  // the swap still works in a full bag
    check(rejected(1) == Reasons{"full", "full", "full"} && gear(ann) == Slots{"", "", "", "", "", "worn_shortsword"} &&
              bag(ann) == full,
          "starter-mmo equipment: a full bag refuses an unequip (full), but a same-slot swap still works");

    // Dead or within 5 s of a hit: refused; 5 s after the last hit: allowed.
    setBag(ann, {"leather_cap:1"});
    const auto d1 = dummy(60, 62.5f);
    run(0.1);
    send(ann, "combat.power", {{"power", "strike"}, {"target", wire(d1)}});
    run(4.5);
    equip(ann, 1, 1, "leather_cap");
    unequip(ann, 6, 0, "worn_shortsword");
    check(rejected(1) == Reasons{"combat", "combat"} && bag(ann) == Slots{"leather_cap:1"},
          "starter-mmo equipment: refused within 5 s of a hit");
    run(0.5);
    equip(ann, 1, 1, "leather_cap");
    check(gear(ann) == Slots{"leather_cap", "", "", "", "", "worn_shortsword"}, "starter-mmo equipment: allowed 5 s after the hit");
    const auto dead = w.spawn(3, 803, "Dead", 60, 100, R"({"Health":{"current":0,"max":140},"Inventory":{"items":["leather_cap"],"counts":[1]}})",
                              10, as("warrior"));
    run(0.1);
    equip(dead, 1, 1, "leather_cap");
    check(rejected(3) == Reasons{"dead"} && gear(dead).empty(), "starter-mmo equipment: refused while dead");

    // Damage: +3 on every damage power, melee and ranged; heals unchanged.
    const auto d2 = dummy(80, 62.5f), d3 = dummy(100, 80);
    const auto mag = w.spawn(4, 804, "Mag", 100, 60, R"({"Equipment":{"items":["","","","","","worn_shortsword",""]}})", 10, as("mage"));
    const auto war = w.spawn(5, 805, "War", 80, 60, R"({"Equipment":{"items":["","","","","","worn_shortsword",""]}})", 10, as("warrior"));
    const auto pri = w.spawn(6, 806, "Pri", 120, 60,
                             R"({"Health":{"current":30,"max":90},"Equipment":{"items":["","","","","","worn_shortsword",""]}})", 10,
                             as("priest"));
    run(0.1);
    w.sent.clear();
    send(war, "combat.power", {{"power", "strike"}, {"target", wire(d2)}});
    run(1.0);
    send(war, "combat.power", {{"power", "heroic_slam"}, {"target", wire(d2)}});
    send(mag, "combat.power", {{"power", "firebolt"}, {"target", wire(d3)}});
    send(pri, "combat.power", {{"power", "heal"}});
    double healed = -1;
    for (const auto& e : events(6, "combat.power"))
        if (e["effect"] == "heal") healed = e["amount"];
    check(hitsOn(5, d2) == Hits{15, 28} && hitsOn(4, d3) == Hits{17} && healed == 40,
          std::format("starter-mmo equipment: the sword's +3 Damage: Strike 15, Heroic Slam 28, Firebolt 17; Heal still 40 ({}): ",
                      healed) + Json(hitsOn(5, d2)).dump() + Json(hitsOn(4, d3)).dump());

    // Armor: each point takes 1 off an NPC's hit (the Guard's 5), never below 1, before Shield Wall and Power Shield.
    const auto armored = [&](uint32_t sid, const char* name, float x, const char* cls, const char* items) {
        return w.spawn(sid, 810 + sid, name, x, 160, std::string(R"({"Equipment":{"items":)") + items + "}}", 10, as(cls));
    };
    const auto a2 = armored(10, "Armtwo", 60, "warrior", R"(["","linen_tunic","","","","",""])");                // armor 2
    const auto a8 = armored(11, "Armall", 80, "warrior", R"(["leather_cap","linen_tunic","patched_trousers","worn_boots","cloth_gloves","","wooden_shield"])");
    const auto aw = armored(12, "Armwall", 100, "warrior", R"(["leather_cap","","","","","",""])");              // armor 1
    const auto ap = armored(13, "Armpri", 120, "priest", R"(["","linen_tunic","","","","",""])");               // armor 2
    std::array<entt::entity, 4> biters{};
    for (int i = 0; i < 4; ++i) biters[i] = dummy(60 + 20.0f * static_cast<float>(i), 162, true);
    run(0.1);
    check(maxHealth(a8) == 140 + 5 * 7, "starter-mmo equipment: the full set's 7 Stamina: max Health 175");
    w.sent.clear();
    send(aw, "combat.power", {{"power", "shield_wall"}});
    send(ap, "combat.power", {{"power", "power_shield"}});
    run(1.0);
    send(a2, "combat.power", {{"power", "strike"}, {"target", wire(biters[0])}});
    send(a8, "combat.power", {{"power", "strike"}, {"target", wire(biters[1])}});
    send(aw, "combat.power", {{"power", "strike"}, {"target", wire(biters[2])}});
    send(ap, "combat.power", {{"power", "smite"}, {"target", wire(biters[3])}});
    run(2.5);
    double firstAbsorbed = -1;
    for (const auto& h : events(13, "combat.hit"))
        if (h["target"] == wire(ap) && firstAbsorbed < 0) firstAbsorbed = h.value("absorbed", -1.0);
    check(hitsOn(10, a2).size() >= 2 && hitsOn(10, a2)[0] == 3 && hitsOn(11, a8)[0] == 1 && hitsOn(12, aw)[0] == 2 &&
              firstAbsorbed == 3,
          std::format("starter-mmo equipment: Armor 2: 3, Armor 8: 1 (min), Armor 1 then Shield Wall: 2, Armor 2 then Power "
                      "Shield absorbs 3 ({}): ", firstAbsorbed) +
              Json(hitsOn(10, a2)).dump() + Json(hitsOn(11, a8)).dump() + Json(hitsOn(12, aw)).dump());

    // Stamina: +5 max Health a point. Equipping raises the max only; unequipping clamps the current Health; login and
    // revive use the class's base plus the gear.
    const auto sta = w.spawn(7, 807, "Sta", 160, 60,
                             R"({"Health":{"current":155,"max":155},"Equipment":{"items":["leather_cap","linen_tunic","","","","",""]},)"
                             R"("Inventory":{"items":["worn_boots"],"counts":[1]}})",
                             10, as("warrior"));
    run(0.1);
    const bool login = health(sta) == 155 && maxHealth(sta) == 155;
    equip(sta, 1, 4, "worn_boots");
    const bool raised = health(sta) == 155 && maxHealth(sta) == 160;
    unequip(sta, 2, 0, "linen_tunic");
    check(login && raised && health(sta) == 150 && maxHealth(sta) == 150,
          std::format("starter-mmo equipment: login keeps 155 / 155 ({}), boots raise only the max ({}), the tunic off clamps "
                      "to 150 / 150 ({} / {})", login, raised, health(sta), maxHealth(sta)));
    const auto fallen = w.spawn(8, 808, "Fallen", 180, 60,
                                R"({"Health":{"current":0,"max":150},"Equipment":{"items":["","linen_tunic","","","","",""]}})", 10,
                                as("warrior"));
    run(0.1);
    check(health(fallen) == 0 && maxHealth(fallen) == 150, "starter-mmo equipment: logged in dead, max Health with gear");
    run(5.1);
    check(health(fallen) == 150, "starter-mmo equipment: revived at the class's 140 + the tunic's 10");

    // Saved with the character; a relog has the same gear, max Health and Damage.
    w.world.despawn(war);
    std::string saved = "{}";
    for (const auto& s : w.sent.saves)
        if (s.charId == 805 && s.components) saved = *s.components;
    check(Json::parse(saved).value("Equipment", Json::object()).value("items", Json::array()) ==
              Json::parse(R"(["","","","","","worn_shortsword",""])"),
          "starter-mmo equipment: Equipment is saved with the character: " + saved);
    const auto relogged = w.spawn(5, 805, "War", 80, 60, saved, 10, as("warrior"));
    run(1.1);
    w.sent.clear();
    send(relogged, "combat.power", {{"power", "strike"}, {"target", wire(d2)}});
    check(gear(relogged) == Slots{"", "", "", "", "", "worn_shortsword"} && hitsOn(5, d2) == Hits{15},
          "starter-mmo equipment: after a relog the same gear, and Strike 15");
    for (const auto& st : w.world.script_status()) check(st.ok, "starter-mmo equipment: system still enabled " + st.path + " " + st.error);
}
#endif

// --- maps (docs/services/world/maps.html) ---

void checkTileMath() {
    using map::tile_of;
    check(tile_of(255.999f, 0) == map::TileKey{0, 0}, "255.999 is tile 0");
    check(tile_of(256, 256) == map::TileKey{1, 1}, "256 is tile 1");
    check(tile_of(-0.001f, -256) == map::TileKey{-1, -1}, "-0.001 and -256 are tile -1");
    check(tile_of(-256.001f, 0) == map::TileKey{-2, 0}, "-256.001 is tile -2");
    map::MapInfo m;
    m.tiles = {{0, 0, 1, 0}, {5, 5, 5, 5}};
    check(m.has_tile({1, 0}) && m.has_tile({5, 5}) && !m.has_tile({2, 0}) && !m.has_tile({0, 1}), "union of rects");
    check(m.contains(511.9f, 10) && !m.contains(512, 10) && !m.contains(-0.1f, 10), "far edge is the next tile");
    check(m.clamp(1300, 1300) == std::array<float, 2>{1300, 1300}, "clamp keeps an on-map point");
    check(m.clamp(600, 10)[0] < 512 && m.clamp(600, 10)[0] > 511.99f && m.clamp(600, 10)[1] == 10,
          "clamp pulls an off-map point to the nearest declared tile");
    const auto c = m.clamp(1600, 1600);
    check(c[0] < 1536 && c[1] < 1536 && m.contains(c[0], c[1]), "clamp lands on the nearest rect");
}

// map/heights.h links into the server too (stb's PNG decoder included); the engine's self-check covers it in depth.
void checkHeights() {
    std::string err;
    map::MapInfo info;
    info.heightRange = {{-10, 90}};
    std::string r16(size_t(map::kTileSamples) * map::kTileSamples * 2, '\0');
    const auto flat = map::decode_heights(r16, ".r16", err);
    check(flat && flat->size() == size_t(map::kTileSamples) * map::kTileSamples &&
              map::ground(*flat, info, 100.5f, 3.25f) == -10, "heights: a zero .r16 is flat at heightMin");
    check(!map::decode_heights(r16.substr(1), ".r16", err) && err.find("need 132098") != std::string::npos,
          "heights: a wrong-size .r16 is refused");
    check(!map::decode_heights("not a png", ".png", err) && err.rfind("png:", 0) == 0, "heights: a bad PNG is refused");
}

void checkManifests() {
    std::string err;
    const auto good = Json::parse(R"({"version": 1, "id": 7, "name": "Eastlands", "tiles": [[0,0,7,7], [-2,0,-1,0]],
        "heightRange": [-100, 500], "spawn": {"pos": [1024, 0, 1024], "heading": 0.5},
        "material": "materials/terrain_grass.mat",
        "nav": {"agentRadius": 0.4, "agentHeight": 1.8, "maxClimb": 0.5, "maxSlopeDeg": 45, "cellSize": 0.5}})");
    const auto m = map::parse_manifest(good, err);
    check(m && m->id == 7 && m->name == "Eastlands" && m->tiles.size() == 2 && m->spawn[0] == 1024 &&
              m->spawnHeading == 0.5f && m->heightRange && (*m->heightRange)[1] == 500 &&
              m->material == "materials/terrain_grass.mat" && m->nav && m->nav->maxSlopeDeg == 45,
          "full manifest parses: " + err);
    check(m && m->has_tile({-2, 0}) && !m->has_tile({-3, 0}), "negative tiles");
    const auto minimal =
        map::parse_manifest(Json::parse(R"({"version":1,"id":2,"tiles":[[0,0,0,0]],"spawn":{"pos":[1,0,1]}})"), err);
    check(minimal && !minimal->heightRange && !minimal->nav && minimal->material.empty(), "heightRange/material/nav optional");

    const std::pair<const char*, const char*> bad[] = {
        {R"([1])", "must be a JSON object"},
        {R"({"id":1,"tiles":[[0,0,0,0]],"spawn":{"pos":[1,0,1]}})", "\"version\" must be 1"},
        {R"({"version":2,"id":1,"tiles":[[0,0,0,0]],"spawn":{"pos":[1,0,1]}})", "\"version\" must be 1"},
        {R"({"version":1,"tiles":[[0,0,0,0]],"spawn":{"pos":[1,0,1]}})", "\"id\" must be an integer in [0, 65535]"},
        {R"({"version":1,"id":65536,"tiles":[[0,0,0,0]],"spawn":{"pos":[1,0,1]}})", "\"id\" must be an integer"},
        {R"({"version":1,"id":-1,"tiles":[[0,0,0,0]],"spawn":{"pos":[1,0,1]}})", "\"id\" must be an integer"},
        {R"({"version":1,"id":1,"tiles":[],"spawn":{"pos":[1,0,1]}})", "\"tiles\" must be a non-empty list"},
        {R"({"version":1,"id":1,"tiles":[[0,0,0]],"spawn":{"pos":[1,0,1]}})", "tiles[0]: must be [x0, z0, x1, z1] integers"},
        {R"({"version":1,"id":1,"tiles":[[0,0,0,0],[0,0,0.5,0]],"spawn":{"pos":[1,0,1]}})", "tiles[1]: must be"},
        {R"({"version":1,"id":1,"tiles":[[3,0,2,0]],"spawn":{"pos":[1,0,1]}})", "tiles[0]: x0 <= x1 and z0 <= z1 required"},
        {R"({"version":1,"id":1,"tiles":[[0,0,0,0]]})", "\"spawn\" must be {\"pos\": [x, y, z]"},
        {R"({"version":1,"id":1,"tiles":[[0,0,0,0]],"spawn":{"pos":[1,0]}})", "\"spawn\" must be"},
        {R"({"version":1,"id":1,"tiles":[[0,0,0,0]],"spawn":{"pos":[1,0,1],"heading":"n"}})", "\"spawn.heading\" must be a number"},
        {R"({"version":1,"id":1,"tiles":[[0,0,0,0]],"spawn":{"pos":[300,0,1]}})", "\"spawn.pos\" is not on a declared tile"},
        {R"({"version":1,"id":1,"tiles":[[0,0,0,0]],"spawn":{"pos":[1,0,1]},"heightRange":[5,1]})", "\"heightRange\" must be [min, max]"},
        {R"({"version":1,"id":1,"tiles":[[0,0,0,0]],"spawn":{"pos":[1,0,1]},"material":3})", "\"material\" must be a string"},
        {R"({"version":1,"id":1,"tiles":[[0,0,0,0]],"spawn":{"pos":[1,0,1]},"nav":{"cellSize":-1}})", "\"nav.cellSize\" must be a number >= 0"},
    };
    for (const auto& [text, want] : bad) {
        err.clear();
        const bool refused = !map::parse_manifest(Json::parse(text), err);
        check(refused && contains(err, want), std::string("manifest refused: ") + want + " (got: " + err + ")");
    }

    // Maps::load: every folder's map.json; one line per problem, naming the file.
    LuaRoot r;
    r.write("start/map.json", R"({"version":1,"id":1,"name":"start","tiles":[[0,0,7,7]],"spawn":{"pos":[1024,0,1024]}})");
    r.write("cave/map.json", R"({"version":1,"id":2,"name":"cave","tiles":[[0,0,1,1]],"spawn":{"pos":[256,0,256]}})");
    r.write(".server/map.json", "not even json"); // dot-folders are server data, never maps
    std::vector<std::string> errors;
    const auto ok = world::Maps::load(r.root, errors);
    check(errors.empty() && ok.all().size() == 2 && ok.info(2).name == "cave" && ok.has_tile(1, 2047, 0) &&
              !ok.has_tile(1, 2048, 0) && !ok.has_tile(3, 0, 0) && ok.owns(2, {1, 1}) && !ok.owns(2, {2, 1}),
          "two maps load; dot-folder skipped");
    r.write("dup/map.json", R"({"version":1,"id":2,"tiles":[[0,0,0,0]],"spawn":{"pos":[1,0,1]}})");
    r.write("junk/map.json", "{not json");
    r.write("rect/map.json", R"({"version":1,"id":3,"tiles":[[1,0,0,0]],"spawn":{"pos":[1,0,1]}})");
    r.write("nospawn/map.json", R"({"version":1,"id":4,"tiles":[[0,0,0,0]]})");
    fs::create_directories(r.root / "empty");
    errors.clear();
    world::Maps::load(r.root, errors);
    const std::set<std::string> got(errors.begin(), errors.end());
    const std::set<std::string> want = {
        "maps/dup/map.json: id 2 already used by maps/cave",
        "maps/empty/map.json: missing",
        "maps/junk/map.json: not valid JSON",
        "maps/nospawn/map.json: \"spawn\" must be {\"pos\": [x, y, z], \"heading\": h}",
        "maps/rect/map.json: tiles[0]: x0 <= x1 and z0 <= z1 required",
    };
    std::string all;
    for (const auto& e : errors) all += e + "; ";
    check(got == want, "every bad map reported: " + all);
    errors.clear();
    world::Maps::load(r.root / "nowhere", errors);
    check(errors.size() == 1 && contains(errors[0], "nowhere"), "missing MAP_ROOT refused");
    LuaRoot none;
    errors.clear();
    world::Maps::load(none.root, errors);
    check(errors.size() == 1 && contains(errors[0], ": no maps"), "no maps refused");
}

// Index of the first message of type T on sid, or SIZE_MAX.
template <typename T>
size_t first_of(const Sent& sent, uint32_t sid) {
    const auto it = sent.bySession.find(sid);
    if (it == sent.bySession.end()) return SIZE_MAX;
    for (size_t i = 0; i < it->second.size(); ++i)
        if (std::holds_alternative<T>(it->second[i])) return i;
    return SIZE_MAX;
}

void checkMapWorld() {
    LogCapture capture;
    TestWorld w({});
    // spawn: saved map kept, unknown map / off-map position -> START_MAP spawn, 0 (before migration 003) -> START_MAP
    w.spawn(1, 1, "Onstart", 1000, 1000);
    w.spawn(2, 2, "Incave", 300, 300, "", 2);
    w.spawn(3, 3, "Lost", 1000, 1000, "", 99);
    w.spawn(4, 4, "Offcave", 1000, 1000, "", 2);
    w.spawn(5, 5, "Legacy", 900, 900, "", 0);
    const auto ok = [&](uint32_t sid) { return w.sent.of<proto::EnterWorldOk>(sid).at(0); };
    check(ok(1).map == 1 && ok(1).pos == proto::Vec3{1000, 0, 1000}, "EnterWorldOk map 1 at the saved position");
    check(ok(2).map == 2 && ok(2).pos == proto::Vec3{300, 0, 300}, "saved map 2 kept");
    check(ok(3).map == 1 && ok(3).pos == proto::Vec3{1024, 0, 1024} && ok(3).heading == 0, "unknown map 99 -> START_MAP spawn");
    check(ok(4).map == 1 && ok(4).pos == proto::Vec3{1024, 0, 1024}, "position off its map -> START_MAP spawn");
    check(ok(5).map == 1 && ok(5).pos == proto::Vec3{900, 0, 900}, "map 0 (pre-maps row) reads as START_MAP, position kept");
    check(logged("saved position not on a loaded map"), "fallback logged");
    const auto a = entt::entity{ok(1).entityId}, cave = entt::entity{ok(2).entityId};
    for (uint32_t sid = 3; sid <= 5; ++sid) w.world.despawn(entt::entity{ok(sid).entityId});
    w.sent.clear();

    // Same x/z, different maps: never see each other. A third player next to the first on map 1 does.
    w.spawn(6, 6, "Twin", 300, 300); // map 1 at the cave player's x/z
    const auto near = w.spawn(7, 7, "Near", 1010, 1000);
    w.world.tick(T0);
    const auto spawnsOf = [&](uint32_t sid) {
        std::set<uint32_t> ids;
        for (const auto& s : w.sent.of<proto::Spawn>(sid)) ids.insert(s.entityId);
        return ids;
    };
    check(spawnsOf(2).empty() && spawnsOf(6).empty(), "same x/z on different maps: no Spawn either way");
    check(spawnsOf(1) == std::set<uint32_t>{wire(near)} && spawnsOf(7) == std::set<uint32_t>{wire(a)}, "same map in range: seen");

    // Move onto an undeclared tile: Correction, not applied. A move on the map is.
    w.sent.clear();
    w.move(near, {{1015, 0, 1000}, 0, 0});
    w.world.tick(T0 + std::chrono::seconds(1));
    check(w.sent.of<proto::Correction>(7).empty() && w.world.registry().get<world::Transform>(near).pos.x == 1015, "on-map move accepted");
    w.sent.clear();
    const auto edger = w.spawn(8, 8, "Edger", 2045, 500);
    w.move(edger, {{2050, 0, 500}, 0, 0});
    w.world.tick(T0 + std::chrono::seconds(3));
    const auto corr = w.sent.of<proto::Correction>(8);
    check(corr.size() == 1 && corr[0].pos == proto::Vec3{2045, 0, 500}, "move onto undeclared tile 8 -> Correction");
    w.sent.clear();
    w.move(edger, {{2048, 0, 500}, 0, 0});
    w.world.tick(T0 + std::chrono::seconds(4));
    check(w.sent.of<proto::Correction>(8).size() == 1, "x = 2048 exactly is tile 8: Correction");
    w.world.despawn(edger);

    // Transfer A from map 1 (sees Near) to map 2 next to the cave player.
    w.sent.clear();
    check(w.world.transfer(a, {2, {310, 9000, 300}, 1.5f}), "transfer accepted");
    const auto mc = w.sent.of<proto::MapChange>(1);
    check(mc.size() == 1 && mc[0].map == 2 && mc[0].pos == proto::Vec3{310, 0, 300} && mc[0].heading == 1.5f,
          "MapChange{map, pos (y onto the ground), heading} sent at once");
    check(w.world.registry().get<world::MapRef>(a).map == 2 && w.world.registry().all_of<world::Dirty>(a), "MapRef changed, Dirty");
    w.world.tick(T0 + std::chrono::seconds(5));
    const size_t change = first_of<proto::MapChange>(w.sent, 1), despawn = first_of<proto::Despawn>(w.sent, 1),
                 spawn = first_of<proto::Spawn>(w.sent, 1);
    check(change == 0 && despawn != SIZE_MAX && spawn != SIZE_MAX && change < despawn && change < spawn,
          "mover: MapChange before the Despawn of old neighbours and the Spawn of new ones");
    check(w.sent.of<proto::Despawn>(1).size() == 1 && w.sent.of<proto::Despawn>(1)[0].entityId == wire(near), "mover: Despawn Near");
    check(w.sent.of<proto::Spawn>(1).size() == 1 && w.sent.of<proto::Spawn>(1)[0].entityId == wire(cave), "mover: Spawn the cave player");
    check(w.sent.of<proto::Despawn>(7).size() == 1 && w.sent.of<proto::Despawn>(7)[0].entityId == wire(a), "old neighbour: Despawn mover");
    check(w.sent.of<proto::EntityMove>(7).empty(), "old neighbour never sees where it went");
    check(w.sent.of<proto::Spawn>(2).size() == 1 && w.sent.of<proto::Spawn>(2)[0].pos == proto::Vec3{310, 0, 300},
          "new neighbour: Spawn at the destination");
    check(spawnsOf(6).empty(), "the map-1 player at the same x/z still sees nothing");

    // A move from before the client got MapChange (old-map coordinates, the old epoch): dropped, no Correction; one of
    // MapChange's epoch from there is still validated (Correction to the new position).
    check(mc[0].epoch == 1, "MapChange carries the session's new epoch");
    w.sent.clear();
    w.world.push_move(a, {{1001, 0, 1000}, 0, 0, 1, 0}); // sent with the epoch before MapChange
    w.world.tick(T0 + std::chrono::seconds(6));
    check(w.sent.of<proto::Correction>(1).empty() &&
              w.world.registry().get<world::Transform>(a).pos == proto::Vec3{310, 0, 300},
          "stale old-map move (old epoch) dropped without a Correction");
    w.move(a, {{1001, 0, 1000}, 0, 0, 2});
    w.world.tick(T0 + std::chrono::milliseconds(6100));
    check(w.sent.of<proto::Correction>(1).size() == 1 && w.sent.of<proto::Correction>(1)[0].pos == proto::Vec3{310, 0, 300},
          "an old-map move of the new epoch is corrected");

    // Teleport within the map, next to the same neighbour: both get Despawn now and Spawn again on the next tick, so a
    // client that dropped its remotes on MapChange gets them back.
    const auto p = w.spawn(9, 9, "Porter", 1500, 1500), q = w.spawn(10, 10, "Stayer", 1510, 1500);
    w.world.tick(T0 + std::chrono::seconds(7));
    w.sent.clear();
    check(w.world.transfer(p, {1, {1505, 0, 1500}, 0}), "teleport within the map");
    check(first_of<proto::MapChange>(w.sent, 9) == 0 && w.sent.of<proto::Despawn>(9).size() == 1 &&
              w.sent.of<proto::Despawn>(10).size() == 1 && w.sent.of<proto::Despawn>(10)[0].entityId == wire(p),
          "teleport: MapChange then Despawn Stayer; Stayer gets Despawn Porter");
    w.world.tick(T0 + std::chrono::seconds(8));
    check(spawnsOf(9) == std::set<uint32_t>{wire(q)} && w.sent.of<proto::Spawn>(10).size() == 1 &&
              w.sent.of<proto::Spawn>(10)[0].pos == proto::Vec3{1505, 0, 1500},
          "teleport: both Spawn again on the next tick, at the new position");
    w.world.despawn(p);
    w.world.despawn(q);

    // Refused: not owned (undeclared tile, unknown map); nothing changes, nothing sent.
    w.sent.clear();
    check(!w.world.transfer(a, {2, {600, 0, 100}, 0}) && !w.world.transfer(a, {9, {1, 0, 1}, 0}), "unowned destination refused");
    check(w.sent.bySession.empty() && w.world.registry().get<world::MapRef>(a).map == 2, "refusal changes nothing");
    check(logged("transfer refused"), "refusal logged");

    // Save carries the map.
    w.world.despawn(a);
    check(!w.sent.saves.empty() && w.sent.saves.back().charId == 1 && w.sent.saves.back().mapId == 2, "save includes map");

    // NPCs: created on a map, transferred like players.
    w.sent.clear();
    const auto npc = w.world.create_npc("Bat", "Bat", uint16_t{2}, proto::Vec3{290, 0, 300}, 0);
    w.world.tick(T0 + std::chrono::seconds(9));
    check(spawnsOf(2).contains(wire(npc)) && !spawnsOf(6).contains(wire(npc)), "NPC on map 2 seen only there");
    w.sent.clear();
    check(w.world.transfer(npc, {1, {305, 0, 300}, 0}), "NPC transfer");
    w.world.tick(T0 + std::chrono::seconds(10));
    check(w.sent.of<proto::Despawn>(2).size() == 1 && spawnsOf(6).contains(wire(npc)), "NPC: Despawn on map 2, Spawn on map 1");
    check(w.sent.of<proto::MapChange>(2).empty() && w.sent.of<proto::MapChange>(6).empty(), "an NPC transfer sends no MapChange");
}

// kast.transfer and Transform.map from Lua (players and NPCs), with every argument check.
constexpr const char* kMapLuaSystem = R"lua(
local done = false
return { update = function()
    if done then return end
    done = true
    local p
    for e in kast.query("Character") do p = e end
    expect(kast.get(p, "Transform").map == 1, "Transform.map: player on map 1")
    local g = kast.create("Bat", {template = "Bat"})
    expect(kast.get(g, "Transform").map == 1, "kast.create defaults to START_MAP")
    local c = kast.create("Cave", {template = "Bat", map = 2})
    local tf = kast.get(c, "Transform")
    expect(tf.map == 2 and tf.position.x == 256 and tf.position.z == 256, "create map = 2: at its spawn")
    local far = kast.create("Far", {template = "Bat", map = 2, position = {x = 5000, y = 0, z = 100}})
    expect(kast.get(far, "Transform").position.x < 512, "create clamps onto its map")
    raises('kast.create: "map" must be the id of a loaded map, got 9', function() kast.create("X", {template = "T", map = 9}) end)
    raises('kast.create: "map" must be the id of a loaded map, got 1.5', function() kast.create("X", {template = "T", map = 1.5}) end)

    expect(kast.transfer(p, 2, {x = 300, y = 0, z = 310}, 0.25) == true, "kast.transfer player returns true")
    tf = kast.get(p, "Transform")
    expect(tf.map == 2 and tf.position.x == 300 and tf.position.z == 310 and tf.heading == 0.25, "player moved to map 2")
    expect(kast.transfer(g, 2, {x = 100, y = 0, z = 100}) and kast.get(g, "Transform").map == 2, "kast.transfer NPC")
    expect(kast.get(g, "Transform").heading == 0, "heading defaults to the current one")
    kast.set(g, "Transform", {position = {x = 4000, y = 0, z = 100}})
    expect(kast.get(g, "Transform").position.x < 512, "NPC Transform set clamps onto its new map")
    raises('"Transform": "map" is read-only; use kast.transfer', function() kast.set(g, "Transform", {map = 1}) end)
    raises('kast.transfer: "map" must be the id of a loaded map, got 9', function() kast.transfer(p, 9, {x = 1, y = 0, z = 1}) end)
    raises('kast.transfer: "map" must be the id of a loaded map, got nil', function() kast.transfer(p) end)
    raises('kast.transfer: position (600, 1) is not on a declared tile of map 2', function() kast.transfer(p, 2, {x = 600, y = 0, z = 1}) end)
    raises('kast.transfer: "position" must be a table', function() kast.transfer(p, 2, 5) end)
    raises('kast.transfer: "heading" must be a finite number', function() kast.transfer(p, 2, {x = 1, y = 0, z = 1}, "n") end)
    raises("kast.transfer: invalid entity id 999999", function() kast.transfer(999999, 1, {x = 1, y = 0, z = 1}) end)
    expect(kast.get(p, "Transform").map == 2 and kast.get(p, "Transform").position.x == 300, "failed transfers changed nothing")
    kast.log("map lua done")
end })lua";

void checkMapLua() {
    LogCapture capture;
    LuaRoot r;
    r.write("systems/maps.lua", std::string(kLuaExpect) + kMapLuaSystem);
    TestWorld w({});
    check(w.world.load_scripts(r.root).empty(), "map lua: loads");
    w.spawn(1, 1, "Alice", 1000, 1000);
    w.sent.clear();
    w.world.tick(T0);
    check_lua_expectations("map lua", "map lua done");
    for (const auto& s : w.world.script_status()) check(s.ok, "map lua: system still enabled " + s.error);
    const auto mc = w.sent.of<proto::MapChange>(1);
    check(mc.size() == 1 && mc[0].map == 2 && mc[0].pos == proto::Vec3{300, 0, 310}, "kast.transfer sends the player MapChange");
}

// --- areas (docs/services/world/areas.html) ---

std::optional<map::Areas> areas_of(const std::string& text, std::string& err) {
    err.clear();
    return map::parse_areas(nlohmann::json::parse(text), err);
}

std::string joined(const std::vector<std::string>& items) {
    std::string out;
    for (const auto& item : items) out += (out.empty() ? "" : "; ") + item;
    return out;
}

std::string area_id(const map::Areas& a, float x, float y, float z) {
    const int16_t i = map::area_at(a, x, y, z);
    return i < 0 ? "-" : a.list[i].id;
}

void checkAreaShapes() {
    std::string err;
    // world: 3x3 tiles. town: an L-shaped polygon, only for 0 <= y <= 10. tower: a rect inside town's corner.
    const auto a = areas_of(R"({"version": 1, "areas": [
        {"id": "world", "rect": [0, 0, 768, 768], "flags": {"pvp": false, "noMount": true, "zone": 1}, "music": "w.ogg"},
        {"id": "town", "name": "Town", "parent": "world", "priority": 5, "y": [0, 10], "flags": {"pvp": true},
         "polygon": [[10, 10], [60, 10], [60, 30], [30, 30], [30, 60], [10, 60]]},
        {"id": "tower", "parent": "town", "priority": 9, "rect": [12, 12, 20, 20], "flags": {"zone": 3}},
        {"id": "east", "rect": [700, 0, 800, 10], "priority": 1}
    ]})", err);
    check(a.has_value(), "areas parse: " + err);
    if (!a) return;
    check(area_id(*a, 500, 0, 500) == "world" && area_id(*a, 1, 0, 1) == "world", "rect: inside");
    check(area_id(*a, 800.5f, 0, 5) == "-" && area_id(*a, -1, 0, 5) == "-" && area_id(*a, 5, 0, 900) == "-", "outside all: none");
    check(area_id(*a, 750, 0, 5) == "east", "higher priority wins where areas overlap");
    check(area_id(*a, 20, 5, 50) == "town" && area_id(*a, 50, 5, 20) == "town", "polygon: both arms of the L");
    check(area_id(*a, 50, 5, 50) == "world", "polygon: the L's notch is outside (inside its bounds)");
    check(area_id(*a, 20, 10.5f, 50) == "world" && area_id(*a, 20, -0.5f, 50) == "world" && area_id(*a, 20, 0, 50) == "town",
          "y range: above and below fall through to the parent, the bound itself is inside");
    check(area_id(*a, 15, 100, 15) == "tower", "priority: tower over town over world (no y range on tower)");
    const auto& tower = a->list[2];
    check(tower.flags == nlohmann::json{{"pvp", true}, {"noMount", true}, {"zone", 3}},
          "flags: own over parent's over grandparent's: " + tower.flags.dump());
    check(a->list[1].name == "Town" && tower.name == "tower" && tower.music.empty() && a->list[0].music == "w.ogg",
          "name defaults to id; music is not inherited");
    check(a->byTile.at({0, 0}).size() == 3 && a->byTile.at({0, 0})[0] == 2 && !a->byTile.contains({4, 4}),
          "tile buckets: highest priority first, only tiles an area touches");

    // Not overlaps: a shared edge, disjoint y ranges, different priorities.
    check(areas_of(R"({"version":1,"areas":[{"id":"a","rect":[0,0,10,10]},{"id":"b","rect":[10,0,20,10]},
        {"id":"c","polygon":[[0,10],[10,10],[5,20]]}]})", err).has_value(), "equal priority sharing an edge: fine " + err);
    check(areas_of(R"({"version":1,"areas":[{"id":"a","rect":[0,0,10,10],"y":[0,5]},{"id":"b","rect":[0,0,10,10],"y":[5,9]}]})",
                   err).has_value(), "equal priority, same rect, disjoint y: fine " + err);
    check(areas_of(R"({"version":1,"areas":[]})", err).has_value(), "no areas: fine");

    const std::pair<const char*, const char*> bad[] = {
        {R"({"version":1,"areas":[{"id":"a","rect":[0,0,10,10]},{"id":"b","rect":[5,5,15,15]}]})",
         "areas \"a\" and \"b\" overlap with equal priority 0"},
        {R"({"version":1,"areas":[{"id":"a","rect":[0,0,10,10]},{"id":"b","rect":[0,0,10,10]}]})", "overlap with equal priority"},
        {R"({"version":1,"areas":[{"id":"a","rect":[0,0,10,10],"priority":3},{"id":"b","polygon":[[-5,-5],[20,-5],[5,20]],"priority":3}]})",
         "areas \"a\" and \"b\" overlap with equal priority 3"},
        {R"({"version":1,"areas":[{"id":"a","rect":[0,0,10,10]},{"id":"b","rect":[2,2,4,4],"y":[0,1]}]})", "overlap with equal priority"},
        {R"({"version":1,"areas":[{"id":"a","rect":[0,0,10,10],"parent":"nope"}]})", "area \"a\": unknown parent \"nope\""},
        {R"({"version":1,"areas":[{"id":"a","rect":[0,0,1,1],"parent":"b","priority":1},{"id":"b","rect":[5,5,6,6],"parent":"a"}]})",
         "parent cycle"},
        {R"({"version":1,"areas":[{"id":"a","rect":[0,0,1,1]},{"id":"a","rect":[5,5,6,6]}]})", "area \"a\": duplicate id"},
        {R"({"version":1,"areas":[{"id":"a"}]})", "area \"a\": needs exactly one of \"rect\" and \"polygon\""},
        {R"({"version":1,"areas":[{"id":"a","rect":[0,0,1,1],"polygon":[[0,0],[1,0],[0,1]]}]})", "needs exactly one of"},
        {R"({"version":1,"areas":[{"id":"a","rect":[5,0,1,1]}]})", "\"rect\" must be [x0, z0, x1, z1] with x0 < x1"},
        {R"({"version":1,"areas":[{"id":"a","polygon":[[0,0],[1,0]]}]})", "\"polygon\" must be a list of at least 3"},
        {R"({"version":1,"areas":[{"id":"a","rect":[0,0,1,1],"y":[5,1]}]})", "\"y\" must be [min, max]"},
        {R"({"version":1,"areas":[{"id":"a","rect":[0,0,1,1],"flags":[1]}]})", "\"flags\" must be an object"},
        {R"({"version":1,"areas":[{"id":"a","rect":[0,0,1,1],"portal":{"map":2}}]})", "\"portal\" must be {\"map\": id"},
        {R"({"version":1,"areas":[{"id":"a","rect":[0,0,1e9,1]}]})", "spans more than 65536 tiles"},
        {R"({"version":1,"areas":[{"rect":[0,0,1,1]}]})", "areas[0]: \"id\" must be a non-empty string"},
        {R"({"version":2,"areas":[]})", "\"version\" must be 1"},
        {R"({"version":1})", "\"areas\" must be a list"},
    };
    for (const auto& [text, want] : bad) {
        const bool refused = !areas_of(text, err);
        check(refused && contains(err, want), std::string("areas refused: ") + want + " (got: " + err + ")");
    }
}

// Maps::load: areas.json per map, and the portals between maps.
void checkAreaLoading() {
    std::vector<std::string> errors;
    LuaRoot r;
    r.write("a/map.json", R"({"version":1,"id":1,"tiles":[[0,0,1,1]],"spawn":{"pos":[10,0,10]}})");
    r.write("b/map.json", R"({"version":1,"id":2,"tiles":[[0,0,0,0]],"spawn":{"pos":[10,0,10]}})");
    r.write("b/areas.json", R"({"version":1,"areas":[{"id":"door","rect":[0,0,20,20],"portal":{"map":1,"pos":[300,0,300]}}]})");
    r.write("a/areas.json", R"({"version":1,"areas":[
        {"id":"ok","rect":[0,0,20,20],"portal":{"map":2,"pos":[100,0,100],"heading":1}},
        {"id":"lost","rect":[30,0,40,10],"portal":{"map":7,"pos":[1,0,1]}},
        {"id":"off","rect":[50,0,60,10],"portal":{"map":2,"pos":[300,0,1]}},
        {"id":"bounce","rect":[70,0,80,10],"portal":{"map":2,"pos":[5,0,5]}}]})");
    errors.clear();
    const auto maps = world::Maps::load(r.root, errors);
    const std::set<std::string> got(errors.begin(), errors.end());
    const std::set<std::string> want = {
        "maps/a/areas.json: area \"lost\": portal to unknown map 7",
        "maps/a/areas.json: area \"off\": portal destination is not on a tile of map 2",
        "maps/a/areas.json: area \"bounce\": portal destination is inside portal area \"door\" of map 2",
    };
    std::string all;
    for (const auto& e : errors) all += e + "; ";
    check(got == want, "every bad portal reported: " + all);
    const auto& ok = maps.info(1).areas.list[0];
    check(ok.portal && ok.portal->map == 2 && ok.portal->pos == std::array<float, 3>{100, 0, 100} && ok.portal->heading == 1,
          "portal parsed");

    r.write("b/areas.json", R"({"version":1,"areas":[{"id":"x","rect":[0,0,1,1],"parent":"y"}]})");
    r.write("a/areas.json", "{nope");
    errors.clear();
    world::Maps::load(r.root, errors);
    const std::set<std::string> got2(errors.begin(), errors.end());
    check(got2 == std::set<std::string>{"maps/a/areas.json: not valid JSON", "maps/b/areas.json: area \"x\": unknown parent \"y\""},
          "bad areas.json reported per file, its map not loaded");
}

#ifdef KAST_SAMPLE_MAPS
// The sample maps and lua/: area events on spawn and on crossing a border, then a walk into the cave portal.
void checkAreaWorld() {
    {
        std::vector<std::string> errors;
        const auto sample = world::Maps::load(KAST_SAMPLE_MAPS, errors);
        check(errors.empty() && sample.info(1).areas.list.size() == 4 && sample.info(2).areas.list.size() == 2,
              "sample maps and their areas load" + (errors.empty() ? "" : ": " + errors[0]));
    }
    LogCapture capture;
    std::vector<std::string> mapErrors;
    // The sample components: the sample NPC definitions give their NPCs Health.
    TestWorld w(lua::load_component_defs(KAST_WORLD_LUA_SAMPLE, world::kReservedComponentNames).defs, {},
                world::Maps::load(KAST_SAMPLE_MAPS, mapErrors));
    const auto errors = w.world.load_scripts(KAST_WORLD_LUA_SAMPLE, {"zone_cycle"});
    check(mapErrors.empty() && errors.empty(), "area world: sample maps and lua load");
    std::vector<std::string> events;
    const auto record = [&](const char* what) {
        return [&events, what](const nlohmann::json& p) {
            events.push_back(std::format("{} {} {} {} pvp={}", what, p["entity"].get<uint32_t>(), p["map"].get<int>(),
                                         p["area"].get<std::string>(), p["flags"].value("pvp", false)));
        };
    };
    auto enter = w.world.bus().subscribe("area.enter", record("enter"));
    auto leave = w.world.bus().subscribe("area.leave", record("leave"));
    auto& reg = w.world.registry();

    // Cora waits on map 2 next to the portal destination.
    const auto a = w.spawn(1, 1, "Alice", 1024, 1024), b = w.spawn(2, 2, "Bob", 1046, 1010);
    const auto c = w.spawn(3, 3, "Cora", 256, 230, "", 2);
    const uint32_t A = wire(a), B = wire(b), C = wire(c);
    w.world.tick(T0);
    check(events == std::vector<std::string>{std::format("enter {} 1 goldshire pvp=true", A),
                                             std::format("enter {} 1 eastvale pvp=false", B),
                                             std::format("enter {} 2 cave pvp=false", C)},
          "spawn: area.enter only, flags inherited (pvp from goldshire)");
    check(reg.get<world::AreaRef>(a).area == 1 && reg.get<world::AreaRef>(b).area == 0, "AreaRef set on spawn");
    check(logged(std::format("area.enter entity {} map 1 area goldshire (Goldshire) flags {{noMount=false, pvp=true}}", A)),
          "events.lua logs area.enter");

    // A walks toward the portal in steps under the speed limit; it leaves goldshire at x = 1035.
    const proto::Vec3 path[] = {{1030, 0, 1018}, {1036, 0, 1012}, {1042, 0, 1006}};
    std::vector<std::vector<std::string>> perStep;
    for (int i = 0; i < 3; ++i) {
        events.clear();
        w.move(a, {path[i], 2.4f, 0});
        w.world.tick(T0 + std::chrono::seconds(i + 1));
        perStep.push_back(events);
    }
    check(perStep[0].empty() && perStep[2].empty(), "no events while inside one area");
    check(perStep[1] == std::vector<std::string>{std::format("leave {} 1 goldshire pvp=true", A),
                                                 std::format("enter {} 1 eastvale pvp=false", A)},
          "crossing a border: area.leave then area.enter, once each");
    check(std::ranges::any_of(w.sent.of<proto::Spawn>(1), [&](const proto::Spawn& s) { return s.entityId == B; }),
          "A sees B before the portal");

    // Into cave_mouth: transferred in the same tick, before the aoi pass.
    w.sent.clear();
    events.clear();
    w.move(a, {{1045, 0, 998}, 2.8f, 0});
    w.world.tick(T0 + std::chrono::seconds(4));
    const auto& tf = reg.get<world::Transform>(a);
    check(reg.get<world::MapRef>(a).map == 2 && tf.pos == proto::Vec3{256, 0, 240} && tf.heading == 3.14f,
          "portal: on map 2 at the portal pos and heading");
    check(events == std::vector<std::string>{std::format("leave {} 1 eastvale pvp=false", A),
                                             std::format("enter {} 1 cave_mouth pvp=false", A),
                                             std::format("leave {} 1 cave_mouth pvp=false", A),
                                             std::format("enter {} 2 cave pvp=false", A)},
          "portal: leave eastvale, enter cave_mouth, leave it, enter the cave on map 2: " + joined(events));
    check(reg.get<world::AreaRef>(a).map == 2 && reg.get<world::AreaRef>(a).area == 0, "AreaRef: cave on map 2");
    const auto& toA = w.sent.bySession[1];
    const size_t change = first_of<proto::MapChange>(w.sent, 1), despawn = first_of<proto::Despawn>(w.sent, 1),
                 spawn = first_of<proto::Spawn>(w.sent, 1);
    check(change == 0 && std::get<proto::MapChange>(toA[0]).map == 2 && change < despawn && despawn < spawn &&
              spawn != SIZE_MAX,
          "portal: MapChange, then Despawn, then Spawn, all in the portal tick");
    bool despawnedB = false, sawBat = false;
    for (const auto& d : w.sent.of<proto::Despawn>(1)) despawnedB |= d.entityId == B;
    for (const auto& s : w.sent.of<proto::Spawn>(1)) sawBat |= s.name == "Bat";
    check(despawnedB && sawBat, "portal: A loses B and sees the Bat on map 2");
    bool bLostA = false;
    for (const auto& d : w.sent.of<proto::Despawn>(2)) bLostA |= d.entityId == A;
    check(bLostA, "B gets A's Despawn");
    bool leaked = false;
    for (uint32_t sid : {2u, 3u})
        for (const auto& m : w.sent.of<proto::EntityMove>(sid)) leaked |= m.entityId == A;
    check(!leaked, "no EntityMove for A's portal step, to B on map 1 or Cora on map 2");
    check(std::ranges::any_of(w.sent.of<proto::Spawn>(3), [&](const proto::Spawn& s) { return s.entityId == A; }),
          "Cora gets A's Spawn in the portal tick");

    // No ping-pong: the destination is not in a portal, and staying put fires nothing more.
    events.clear();
    w.move(a, {{256, 0, 241}, 0, 0});
    w.world.tick(T0 + std::chrono::seconds(5));
    check(reg.get<world::MapRef>(a).map == 2 && events.empty(), "moving on the cave map: no transfer, no events");
    w.world.despawn(a);
    check(!w.sent.saves.empty() && w.sent.saves.back().mapId == 2, "save keeps map 2");
}

// kast.map.area / kast.map.areaAt over the sample maps.
constexpr const char* kAreaLuaSystem = R"lua(
local done = false
return { update = function()
    if done then return end
    done = true
    local p
    for e in kast.query("Character") do p = e end
    local a = kast.map.area(p)
    expect(a and a.id == "goldshire" and a.name == "Goldshire", "area(player): goldshire")
    expect(a.flags.pvp == true and a.flags.noMount == false, "area flags inherited")
    expect(a.music == nil, "music not inherited")
    local e = kast.map.areaAt(1, 1100, 0, 1100)
    expect(e.id == "eastvale" and e.music == "sounds/eastvale.ogg" and e.flags.pvp == false, "areaAt eastvale with music")
    expect(kast.map.areaAt(1, 1024, 300, 1024).id == "eastvale", "areaAt above goldshire's y range: eastvale")
    expect(kast.map.areaAt(1, 1045, 0, 995).id == "cave_mouth", "areaAt the portal")
    expect(kast.map.areaAt(2, 256, 0, 256).flags.noMount == true, "areaAt map 2")
    expect(kast.map.areaAt(1, 5000, 0, 5000) == nil, "areaAt outside every area: nil")
    local bat = kast.create("Bat2", {template = "Bat", map = 2, position = {x = 256, y = 0, z = 275}})
    expect(kast.map.area(bat).id == "cave_exit", "area(NPC)")
    raises('kast.map.areaAt: "map" must be the id of a loaded map, got 9', function() kast.map.areaAt(9, 1, 1, 1) end)
    raises('kast.map.areaAt: "y" must be a finite number', function() kast.map.areaAt(1, 1, "y", 1) end)
    raises("kast.map.area: invalid entity id 999999", function() kast.map.area(999999) end)
    raises("kast is read-only", function() kast.map.area = nil end)
    kast.log("area lua done")
end })lua";

void checkAreaLua() {
    LogCapture capture;
    LuaRoot r;
    r.write("systems/areas.lua", std::string(kLuaExpect) + kAreaLuaSystem);
    for (const char* name : {"Guard", "Sentry", "Bat"}) // the sample spawn files' NPCs
        r.write(std::format("npcs/{}.lua", name), std::format("return {{name = '{}', template = 'Guard'}}", name));
    std::vector<std::string> mapErrors;
    TestWorld w({}, {}, world::Maps::load(KAST_SAMPLE_MAPS, mapErrors));
    check(w.world.load_scripts(r.root).empty(), "area lua: loads");
    w.spawn(1, 1, "Alice", 1024, 1024);
    w.world.tick(T0);
    check_lua_expectations("area lua", "area lua done");
    for (const auto& s : w.world.script_status()) check(s.ok, "area lua: system still enabled " + s.error);
}
#endif

// --- ground (docs/services/world/maps.html#ground) ---

float ramp(float x, float z) { return 0.1f * x + 0.05f * z; }

// The ramp as tile (tx, tz)'s .r16 on a map with heightRange [0, 100].
std::string ramp_r16(int tx, int tz) {
    constexpr int n = map::kTileSamples;
    std::string bytes(size_t(n) * n * 2, '\0');
    for (int v = 0; v < n; ++v)
        for (int u = 0; u < n; ++u) {
            const auto sample = uint16_t(std::lround(ramp(float(tx * 256 + u), float(tz * 256 + v)) / 100 * 65535));
            bytes[2 * (size_t(v) * n + u)] = char(sample & 0xFF);
            bytes[2 * (size_t(v) * n + u) + 1] = char(sample >> 8);
        }
    return bytes;
}

constexpr float kGroundTol = 0.002f; // 16-bit samples over a 100 m range: 0.8 mm steps
bool on_ramp(std::optional<float> y, float x, float z) { return y && std::fabs(*y - ramp(x, z)) < kGroundTol; }

constexpr const char* kGroundLuaSystem = R"lua(
local done = false
return { update = function()
    if done then return end
    done = true
    local y = kast.map.ground(1, 10.3, 20.7)
    expect(y ~= nil and math.abs(y - (0.1 * 10.3 + 0.05 * 20.7)) < 0.002, "kast.map.ground on the ramp: " .. tostring(y))
    expect(kast.map.ground(1, 300, 300) == 0, "kast.map.ground on a tile without heights: heightMin")
    expect(kast.map.ground(1, 512, 10) == nil, "kast.map.ground off the map: nil")
    raises('kast.map.ground: "map" must be the id of a loaded map, got 9', function() kast.map.ground(9, 0, 0) end)
    raises('kast.map.ground: "x" must be a finite number', function() kast.map.ground(1, 0 / 0, 0) end)
    kast.log("ground lua done")
end }
)lua";

void checkGround() {
    // A 2 x 2 tile ramp: three tiles with a heights file, tile 1_1 without (flat at heightMin).
    LuaRoot r;
    r.write("ramp/map.json", R"({"version":1,"id":1,"tiles":[[0,0,1,1]],"heightRange":[0,100],"spawn":{"pos":[100,0,100]}})");
    r.write("ramp/heights/0_0.r16", ramp_r16(0, 0));
    r.write("ramp/heights/1_0.r16", ramp_r16(1, 0));
    r.write("ramp/heights/0_1.r16", ramp_r16(0, 1));
    r.write("ramp/.server/spawns/0_0.json",
            R"({"version":2,"spawns":[{"id":"high","npc":"T","pos":[50,100,60],"respawnSec":-1}]})");
    std::vector<std::string> errors;
    world::Maps maps = world::Maps::load(r.root, errors);
    check(errors.empty(), "ground: the ramp map loads " + joined(errors));
    check(on_ramp(maps.ground(1, 10, 20), 10, 20), "ground: at a sample");
    check(on_ramp(maps.ground(1, 10.3f, 20.7f), 10.3f, 20.7f) && on_ramp(maps.ground(1, 10.8f, 20.1f), 10.8f, 20.1f),
          "ground: between samples, in both triangles");
    check(on_ramp(maps.ground(1, 255.999f, 100), 255.999f, 100) && on_ramp(maps.ground(1, 256, 100), 256, 100) &&
              on_ramp(maps.ground(1, 100, 256), 100, 256),
          "ground: either side of the x tile edge and on the z tile edge");
    check(maps.ground(1, 300, 300) == 0.f, "ground: a tile without a heights file is flat at heightMin");
    check(!maps.ground(1, 512, 10) && !maps.ground(1, -0.1f, 10) && !maps.ground(9, 10, 10),
          "ground: nullopt off the map's tiles and on an unknown map");
    std::string err;
    const auto tile = map::decode_heights(ramp_r16(1, 0), ".r16", err);
    bool same = tile.has_value();
    for (float x = 256; same && x < 512; x += 7.37f)
        for (float z = 0; same && z < 256; z += 11.13f) same = *maps.ground(1, x, z) == map::ground(*tile, maps.info(1), x - 256, z);
    check(same, "ground: Maps::ground is the shared map::ground on the tile's heights");

    LuaRoot bad;
    bad.write("b/map.json", R"({"version":1,"id":1,"tiles":[[0,0,0,0]],"spawn":{"pos":[10,0,10]}})");
    bad.write("b/heights/0_0.r16", "short");
    bad.write("b/heights/5_5.r16", "not a tile of the map: never read");
    errors.clear();
    world::Maps::load(bad.root, errors);
    check(errors == std::vector<std::string>{"maps/b/heights/0_0.r16: r16: 5 bytes, need 132098 (257 x 257 x 2)"},
          "ground: a bad heights file is a load error: " + joined(errors));

    LogCapture capture;
    LuaRoot scripts;
    scripts.write("systems/ground.lua", std::string(kLuaExpect) + kGroundLuaSystem);
    scripts.write("npcs/t.lua", "return {name = 'T', template = 'T'}");
    TestWorld w({}, {}, std::move(maps));
    const auto luaErrors = w.world.load_scripts(scripts.root);
    check(luaErrors.empty(), "ground lua: loads " + joined(luaErrors));
    const auto& reg = w.world.registry();
    const auto at = [](int s) { return T0 + std::chrono::seconds(s); };

    // Entering: a saved position under the ground is lifted onto it; above it is kept (the client falls).
    const auto under = w.world.spawn(1, {1, 1, "Under", 1, {100, -50, 100}, 0, ""}, T0);
    const auto sky = w.world.spawn(2, {2, 1, "Sky", 1, {120, ramp(120, 100) + 50, 100}, 0, ""}, T0);
    check(on_ramp(w.sent.of<proto::EnterWorldOk>(1).at(0).pos.y, 100, 100), "saved under the ground: enters on it");
    check(on_ramp(w.sent.of<proto::EnterWorldOk>(2).at(0).pos.y - 50, 120, 100), "saved 50 m above the ground: kept");
    w.world.tick(T0);
    check_lua_expectations("ground lua", "ground lua done");

    // Moves: under the ground lifted, rising into the air corrected, falling accepted.
    w.sent.clear();
    w.move(under, {{101, ramp(101, 100) - 5, 100}, 0, 0});
    w.world.tick(at(1));
    check(w.sent.of<proto::Correction>(1).empty() && on_ramp(reg.get<world::Transform>(under).pos.y, 101, 100),
          "move 5 m under the ground: accepted, lifted onto it");
    w.move(under, {{102, ramp(102, 100) + 10, 100}, 0, 0});
    w.world.tick(at(2));
    const auto fly = w.sent.of<proto::Correction>(1);
    check(fly.size() == 1 && fly[0].pos == reg.get<world::Transform>(under).pos && fly[0].pos.x == 101,
          "rising 10 m above the ground: Correction to the last position");
    w.move(sky, {{121, ramp(121, 100) + 49, 100}, 0, 0});
    w.world.tick(at(3));
    check(w.sent.of<proto::Correction>(2).empty() && on_ramp(reg.get<world::Transform>(sky).pos.y - 49, 121, 100),
          "falling from 50 m above the ground: accepted");

    // NPCs stand on the ground: kast.create / create_npc, spawn files. Transfers land on it.
    const auto npc = w.world.create_npc("Up", "T", 1, proto::Vec3{200, 100, 30}, 0);
    check(on_ramp(reg.get<world::Transform>(npc).pos.y, 200, 30), "NPC created at y = 100 stands on the ground");
    bool spawned = false;
    for (auto [e, ref, tf] : reg.view<world::SpawnRef, world::Transform>().each()) spawned = on_ramp(tf.pos.y, 50, 60);
    check(spawned, "spawn file pos y = 100: the NPC stands on the ground");
    w.sent.clear();
    check(w.world.transfer(under, {1, {400, 999, 50}, 0}), "ground: transfer accepted");
    const auto mc = w.sent.of<proto::MapChange>(1);
    check(mc.size() == 1 && on_ramp(mc[0].pos.y, 400, 50), "transfer to y = 999: MapChange on the ground");
}

// --- navmesh (docs/services/world/maps.html#navmesh) ---

// A 2 x 1 tile fixture: a gentle ramp up x (~3 degrees) and a block across the straight line from (100, 100) to
// (450, 100): x 310..350, z 0..200, 22 m high, its sides 8 m wide 70 degree slopes. The way round is z > 200.
float cliff(float x, float z) {
    const float inside = std::min({x - 310, 350 - x, 200 - z});
    return 0.05f * x + 22 * std::clamp(inside / 8, 0.0f, 1.0f);
}

std::string r16_of(float (*height)(float, float), int tx, int tz) {
    constexpr int n = map::kTileSamples;
    std::string bytes(size_t(n) * n * 2, '\0');
    for (int v = 0; v < n; ++v)
        for (int u = 0; u < n; ++u) {
            const auto sample = uint16_t(std::lround(height(float(tx * 256 + u), float(tz * 256 + v)) / 100 * 65535));
            bytes[2 * (size_t(v) * n + u)] = char(sample & 0xFF);
            bytes[2 * (size_t(v) * n + u) + 1] = char(sample >> 8);
        }
    return bytes;
}

constexpr const char* kCliffManifest =
    R"({"version":1,"id":1,"tiles":[[0,0,1,0]],"heightRange":[0,100],"nav":{"maxSlopeDeg":45},"spawn":{"pos":[100,0,100]}})";

constexpr const char* kNavLuaSystem = R"lua(
local done = false
return { update = function()
    if done then return end
    done = true
    local from = {x = 100, y = kast.map.ground(1, 100, 100), z = 100}
    local to = {x = 450, y = kast.map.ground(1, 450, 100), z = 100}
    local path = kast.map.path(1, from, to)
    expect(type(path) == "table" and #path > 2, "kast.map.path: a list of more than 2 points")
    local first, last = path[1], path[#path]
    expect(math.abs(first.x - 100) < 1 and math.abs(first.z - 100) < 1, "kast.map.path: starts at from")
    expect(math.abs(last.x - 450) < 1 and math.abs(last.z - 100) < 1, "kast.map.path: ends at to")
    local around = false
    for _, p in ipairs(path) do
        expect(type(p.x) == "number" and type(p.y) == "number" and type(p.z) == "number", "kast.map.path: {x, y, z} points")
        around = around or p.z > 200
    end
    expect(around, "kast.map.path: goes round the block (z > 200)")
    expect(kast.map.path(2, {x = 10, y = 0, z = 10}, {x = 20, y = 0, z = 20}) == nil, "kast.map.path: nil without a navmesh")
    expect(kast.map.path(1, from, {x = 330, y = 1000, z = 100}) == nil, "kast.map.path: nil when to is not near the mesh")
    raises('kast.map.path: "from" must be a table {x=, y=, z=} of finite numbers', function() kast.map.path(1, 5, to) end)
    raises('kast.map.path: "map" must be the id of a loaded map, got 9', function() kast.map.path(9, from, to) end)
    kast.log("nav lua done")
end }
)lua";

void checkNavmesh() {
    std::string err;
    const auto info = map::parse_manifest(nlohmann::json::parse(kCliffManifest), err);
    map::TileHeights heights;
    for (int tx : {0, 1}) heights[{tx, 0}] = *map::decode_heights(r16_of(cliff, tx, 0), ".r16", err);
    check(mapbake::seam_errors(*info, heights).empty(), "bake: the fixture's seam matches");
    map::TileHeights torn = heights;
    torn.at({1, 0})[size_t(40) * map::kTileSamples] += 4000; // column 0 of tile 1_0 at z 40
    const auto seams = mapbake::seam_errors(*info, torn);
    check(seams == std::vector<std::string>{"seam 0_0 | 1_0: 1 samples differ, worst 6.10 m at z 40"},
          "bake: a torn seam is reported: " + joined(seams));

    LuaRoot r;
    r.write("cliff/map.json", kCliffManifest);
    r.write("flat/map.json", R"({"version":1,"id":2,"tiles":[[0,0,0,0]],"spawn":{"pos":[10,0,10]}})");
    std::map<int, std::string> navtiles;
    for (int tx : {0, 1}) {
        r.write(std::format("cliff/heights/{}_0.r16", tx), r16_of(cliff, tx, 0));
        const auto baked = mapbake::bake_tile(*info, heights, {tx, 0}, err);
        check(baked && baked->polys > 0 && baked->navtile.starts_with("KNAV"),
              std::format("bake: tile {}_0 in memory ({} polys) {}", tx, baked ? baked->polys : 0, err));
        if (baked) r.write(std::format("cliff/.server/nav/{}_0.navtile", tx), navtiles[tx] = baked->navtile);
    }
    std::vector<std::string> errors;
    world::Maps maps = world::Maps::load(r.root, errors);
    check(errors.empty() && maps.nav().has(1) && !maps.nav().has(2),
          "nav: navtiles load; only the baked map has a navmesh " + joined(errors));

    const auto at = [&](float x, float z) { return proto::Vec3{x, *maps.ground(1, x, z), z}; };
    const auto path = maps.nav().path(1, at(100, 100), at(450, 100));
    float length = 0;
    bool around = false, throughBlock = false;
    for (size_t i = 0; i < path.size(); ++i) {
        if (i) length += std::hypot(path[i].x - path[i - 1].x, path[i].z - path[i - 1].z);
        around |= path[i].z > 200;
        throughBlock |= path[i].x > 310 && path[i].x < 350 && path[i].z < 200;
    }
    check(path.size() > 2 && std::hypot(path.back().x - 450, path.back().z - 100) < 1,
          std::format("nav: a path from tile 0_0 to tile 1_0 ({} points)", path.size()));
    check(around && !throughBlock && length > 400,
          std::format("nav: it goes round the block, not through it ({:.0f} m against 350 m straight)", length));
    check(maps.nav().path(1, at(100, 100), {330, 1000, 100}).empty() && maps.nav().path(2, {1, 0, 1}, {9, 0, 9}).empty(),
          "nav: no path to a point off the mesh, or on a map without one");
    const float slopeDeg = std::atan(cliff(314.5f, 100) - cliff(313.5f, 100)) * 180 / std::numbers::pi_v<float>;
    check(std::fabs(slopeDeg - 70) < 0.5f && !maps.nav().on_mesh(1, at(314, 100), 6),
          std::format("nav: on_mesh false on the {:.1f} degree slope", slopeDeg));
    check(maps.nav().on_mesh(1, at(100, 100), 6) && maps.nav().on_mesh(1, at(256, 230), 6) &&
              maps.nav().on_mesh(1, {100, at(100, 100).y + 5, 100}, 6),
          "nav: on_mesh true on walkable ground, at the tile seam and 5 m above it (jumping)");
    check(!maps.nav().on_mesh(1, {100, at(100, 100).y + 20, 100}, 6) && !maps.nav().on_mesh(2, {10, 0, 10}, 6),
          "nav: on_mesh false 20 m above (maxAir 6) and on a map without a navmesh");

    // Bad navtiles: load errors naming the file.
    const auto load_error = [&](const std::string& bytes) {
        LuaRoot bad;
        bad.write("cliff/map.json", kCliffManifest).write("cliff/.server/nav/0_0.navtile", bytes);
        std::vector<std::string> errs;
        world::Maps::load(bad.root, errs);
        return joined(errs);
    };
    const std::string body = navtiles[0].substr(map::kNavtileHeaderBytes);
    const std::string badMagic = load_error("XNAV" + navtiles[0].substr(4));
    check(badMagic == "maps/cliff/.server/nav/0_0.navtile: not a navtile (bad magic, want 'KNAV')",
          "nav: bad magic rejected: " + badMagic);
    std::string v99 = navtiles[0];
    v99[4] = 99;
    const std::string badVersion = load_error(v99);
    check(badVersion == "maps/cliff/.server/nav/0_0.navtile: navtile version 99, this server reads 1 (rebake with kast_mapbake)",
          "nav: bad navtile version rejected: " + badVersion);
    const std::string badDetour = load_error(map::navtile_header(6) + body);
    check(contains(badDetour, "0_0.navtile: Detour version 6, this server has "), "nav: bad Detour version rejected: " + badDetour);
    const std::string otherTile = load_error(navtiles[1]);
    check(otherTile == "maps/cliff/.server/nav/0_0.navtile: baked for tile 1_0", "nav: a tile baked for another rejected: " + otherTile);
    const std::string truncated = load_error(navtiles[0].substr(0, map::kNavtileHeaderBytes + 3));
    check(truncated == "maps/cliff/.server/nav/0_0.navtile: truncated Detour tile", "nav: a truncated navtile rejected: " + truncated);

    // Moves: off the navmesh -> Correction; MOVE_NAV_CHECK=0 or a map without navtiles: phase 5 rules only.
    const auto corrections = [&](bool navCheck, uint16_t map, proto::Vec3 from, proto::Vec3 to) {
        world::WorldRules rules;
        rules.move.navCheck = navCheck;
        std::vector<std::string> errs;
        TestWorld w({}, rules, world::Maps::load(r.root, errs));
        const auto e = w.world.spawn(1, {1, 1, "Walker", map, from, 0, ""}, T0);
        w.world.tick(T0);
        w.sent.clear();
        w.move(e, {to, 0, 0});
        w.world.tick(T0 + std::chrono::seconds(1));
        return w.sent.of<proto::Correction>(1).size();
    };
    check(corrections(true, 1, at(305, 100), at(314, 100)) == 1, "move onto the 70 degree slope: off_mesh -> Correction");
    check(corrections(true, 1, at(300, 100), at(308, 100)) == 0, "move along walkable ground: accepted");
    check(corrections(false, 1, at(305, 100), at(314, 100)) == 0, "MOVE_NAV_CHECK=0: the same move onto the slope accepted");
    check(corrections(true, 2, {10, 0, 10}, {18, 0, 10}) == 0 && corrections(true, 2, {10, 0, 10}, {10, 20, 10}) == 1,
          "a map without navtiles: phase 5 rules (walk accepted, flying corrected)");
    proto::Vec3 slope = at(314, 100);
    MoveBudget budget{11, T0};
    check(validate_move({}, budget, at(305, 100), slope, slope.y, T0, false) == MoveVerdict::off_mesh && budget.meters == 11,
          "validate_move: onMesh false is off_mesh, spending no budget");

    LogCapture capture;
    LuaRoot scripts;
    scripts.write("systems/nav.lua", std::string(kLuaExpect) + kNavLuaSystem);
    TestWorld w({}, {}, std::move(maps));
    const auto luaErrors = w.world.load_scripts(scripts.root);
    check(luaErrors.empty(), "nav lua: loads " + joined(luaErrors));
    w.world.tick(T0);
    check_lua_expectations("nav lua", "nav lua done");
}

// --- NPC definitions (docs/services/world/npcs.html#npc-definitions) ---

constexpr const char* kHp = R"(return { name = "Hp", replicate = "aoi", fields = {
    { name = "cur", type = "float", default = 10, min = 0 }, { name = "max", type = "float", default = 10 } } })";
constexpr const char* kSecret = R"(return { name = "Secret", replicate = "owner", fields = { { name = "v", type = "int" } } })";

// The component types (Stats, aoi Hp, owner Secret) and the plain machine the NPC definitions below use.
LuaRoot& npc_root(LuaRoot& r) {
    r.write("components/stats.lua", kStats).write("components/hp.lua", kHp).write("components/secret.lua", kSecret);
    r.write("fsms/plain.lua", kPlain);
    return r;
}

// Every error World::load_scripts reports for these files on top of npc_root.
std::vector<std::string> npc_errors(const std::vector<std::pair<std::string, std::string>>& files) {
    LuaRoot r;
    npc_root(r);
    for (const auto& [path, body] : files) r.write(path, body);
    TestWorld w(r.defs().defs);
    return w.world.load_scripts(r.root);
}

void checkNpcDefinitionErrors() {
    LogCapture capture;
    const std::string longName(256, 'x');
    const std::string name = "npcs/a.lua: \"name\" must be a non-empty string of at most 255 bytes";
    const std::string tpl = "npcs/a.lua: \"template\" must be a non-empty string of at most 255 bytes";
    const auto with = [](const std::string& c) { return "return {name = 'A', template = 'T', components = " + c + "}"; };
    for (const auto& [body, want] : std::vector<std::pair<std::string, std::string>>{
             {"return {template = 'T'}", name},
             {"return {name = '', template = 'T'}", name},
             {"return {name = 5, template = 'T'}", name},
             {"return {name = '" + longName + "', template = 'T'}", name},
             {"return {name = 'A'}", tpl},
             {"return {name = 'A', template = ''}", tpl},
             {"return {name = 'A', template = '" + longName + "'}", tpl},
             {"return {name = 'A', template = 'T', extra = 1}", "npcs/a.lua: unknown key extra"},
             {"return 5", "npcs/a.lua: must return exactly one table {name = ..., template = ..., components = {...}, loot = {...}}"},
             {with("5"), "npcs/a.lua: \"components\" must be a table {Type = {field = value, ...}, ...}, got 5"},
             {with("{'x'}"), "npcs/a.lua: \"components\": component names must be strings, got 1"},
             {with("{Nope = {}}"), "npcs/a.lua: unknown component \"Nope\""},
             {with("{Npc = {}}"), "npcs/a.lua: \"Npc\" is read-only on the server"},
             {with("{Transform = {}}"), "npcs/a.lua: \"Transform\" is read-only on the server"},
             {with("{Spawn = {}}"), "npcs/a.lua: \"Spawn\" is read-only on the server"},
             {with("{Character = {}}"), "npcs/a.lua: \"Character\" is read-only on the server"},
             {with("{Stats = 5}"), "npcs/a.lua: \"Stats\" must be a table {field = value, ...}, got 5"},
             {with("{Stats = {nope = 1}}"), "npcs/a.lua: \"Stats\": unknown field \"nope\""},
             {with("{Stats = {flag = 'x'}}"), "npcs/a.lua: \"Stats\": \"flag\" must be true or false"},
             {with("{Stats = {hp = 1.5}}"), "npcs/a.lua: \"Stats\": \"hp\" must be an integer"},
             {with("{Stats = {hp = 101}}"), "npcs/a.lua: \"Stats\": \"hp\" must be <= 100"},
             {with("{Stats = {hp = -1}}"), "npcs/a.lua: \"Stats\": \"hp\" must be >= 0"},
             {with("{Stats = {kind = 'zzz'}}"), "npcs/a.lua: \"Stats\": \"kind\" unknown value \"zzz\" (expected one of \"a\", \"b\")"},
             {with("{Stats = {tags = {'a', 3}}}"), "npcs/a.lua: \"Stats\": \"tags\" element 2 must be a string"},
             {with("{StateMachine = {machine = 'nope'}}"),
              "npcs/a.lua: \"StateMachine\": \"machine\" \"nope\" names no valid state machine definition in fsms/"},
             {with("{StateMachine = {}}"),
              "npcs/a.lua: \"StateMachine\": \"machine\" \"\" names no valid state machine definition in fsms/"},
         }) {
        const auto errors = npc_errors({{"npcs/a.lua", body}});
        const std::string got = errors.size() == 1 ? errors[0] : "<" + std::to_string(errors.size()) + " errors: " + joined(errors) + ">";
        check(got == want, "npc definition rejected: want [" + want + "] got [" + got + "]");
    }
    const auto boom = npc_errors({{"npcs/a.lua", "error('boom')"}});
    check(boom.size() == 1 && boom[0].starts_with("npcs/a.lua:1: boom"), "npc definition: a Lua error is path:line " + joined(boom));

    check(npc_errors({{"npcs/d1.lua", "return {name = 'D', template = 'T'}"}, {"npcs/sub/d2.lua", "return {name = 'D', template = 'T'}"}}) ==
              std::vector<std::string>{"npcs/d1.lua: name \"D\" is also declared in npcs/sub/d2.lua",
                                       "npcs/sub/d2.lua: name \"D\" is also declared in npcs/d1.lua"},
          "npc definitions: a duplicate name rejects both files, each naming the other");
    check(npc_errors({{"npcs/a.lua", "return {}"}, {"npcs/b.lua", "return {name = 'B'}"}, {"npcs/c.lua", "return {name = 'C', template = 'T'}"}})
                  .size() == 2,
          "npc definitions: every bad file is reported, not just the first");
    check(npc_errors({}).empty(), "npc definitions: no npcs/ folder is fine");
    check(npc_errors({{"npcs/readme.txt", "not lua"}}).empty(), "npc definitions: an npcs/ folder without .lua files is fine");
    check(npc_errors({{"npcs/sandbox.lua", "return {name = io and 'X' or 'S', template = os and 'X' or 'T'}"}}).empty(),
          "npc definitions: run sandboxed (no io, os)");
}

// --- Item definitions and NPC loot tables (docs/services/world/lua.html#items) ---

constexpr const char* kWing = "return {id = 'wing', name = 'Wing', icon = 'icons/wing.png', quality = 'poor', maxStack = 20, "
                              "sellPrice = 1, description = 'Leathery.'}";

void checkItemsAndLoot() {
    LogCapture capture;
    const char* parts[] = {"id = 'a'", "name = 'A'", "icon = 'i.png'", "quality = 'common'", "maxStack = 5", "sellPrice = 0",
                           "description = ''"};
    const auto item = [&](const std::string& drop, const std::string& extra) { // every field but drop, then extra
        std::string out;
        for (const std::string part : parts)
            if (!part.starts_with(drop + " ")) out += (out.empty() ? "" : ", ") + part;
        return "return {" + out + extra + "}";
    };
    for (const auto& [body, want] : std::vector<std::pair<std::string, std::string>>{
             {"return 5", "items/a.lua: must return exactly one table {id = ..., name = ..., icon = ..., quality = ..., maxStack = ..., sellPrice = ..., description = ...}"},
             {item("", ", extra = 1"), "items/a.lua: unknown key extra"},
             {item("id", ""), "items/a.lua: \"id\" must be a non-empty string of at most 64 bytes"},
             {item("id", ", id = 'Bad Id'"), "items/a.lua: \"id\" may only hold a-z, 0-9 and _"},
             {item("name", ""), "items/a.lua: \"name\" must be a non-empty string of at most 255 bytes"},
             {item("icon", ""), "items/a.lua: \"icon\" must be a non-empty string of at most 255 bytes"},
             {item("description", ""), "items/a.lua: \"description\" must be a string of at most 1024 bytes"},
             {item("quality", ", quality = 'shiny'"), "items/a.lua: \"quality\" must be one of \"poor\", \"common\", \"uncommon\", \"rare\", \"epic\""},
             {item("maxStack", ", maxStack = 0"), "items/a.lua: \"maxStack\" must be an integer in [1, 1000]"},
             {item("maxStack", ", maxStack = 1.5"), "items/a.lua: \"maxStack\" must be an integer in [1, 1000]"},
             {item("sellPrice", ", sellPrice = -1"), "items/a.lua: \"sellPrice\" must be an integer in [0, 1000000000]"},
         }) {
        LuaRoot r;
        r.write("items/a.lua", body);
        TestWorld w({});
        const auto errors = w.world.load_scripts(r.root);
        const std::string got = errors.size() == 1 ? errors[0] : "<" + joined(errors) + ">";
        check(got == want, "item definition rejected: want [" + want + "] got [" + got + "]");
    }
    {
        LuaRoot r;
        r.write("items/w1.lua", kWing).write("items/sub/w2.lua", kWing);
        TestWorld w({});
        const auto errors = w.world.load_scripts(r.root);
        check(errors == std::vector<std::string>{"items/sub/w2.lua: id \"wing\" is also declared in items/w1.lua",
                                                 "items/w1.lua: id \"wing\" is also declared in items/sub/w2.lua"},
              "item definitions: a duplicate id rejects both files " + joined(errors));
    }

    const auto loot = [](const std::string& l) { return "return {name = 'A', template = 'T', loot = " + l + "}"; };
    for (const auto& [body, want] : std::vector<std::pair<std::string, std::string>>{
             {loot("5"), "npcs/a.lua: \"loot\": must be a table"},
             {loot("{coins = 1}"), "npcs/a.lua: \"loot\": unknown key coins"},
             {loot("{gold = {min = 3, max = 1}}"), "npcs/a.lua: \"loot\": \"gold\": \"max\" must be an integer in [3, 1000000]"},
             {loot("{gold = {min = -1, max = 1}}"), "npcs/a.lua: \"loot\": \"gold\": \"min\" must be an integer in [0, 1000000]"},
             {loot("{items = 5}"), "npcs/a.lua: \"loot\": \"items\" must be a sequence {{item = ..., chance = ..., min = ..., max = ...}, ...}"},
             {loot("{items = {{item = 'nope', chance = 1, min = 1, max = 1}}}"), "npcs/a.lua: \"loot\": \"items\"[1]: \"item\" must name an item definition in items/"},
             {loot("{items = {{item = 'wing', chance = 0, min = 1, max = 1}}}"), "npcs/a.lua: \"loot\": \"items\"[1]: \"chance\" must be a number in (0, 1]"},
             {loot("{items = {{item = 'wing', chance = 1.5, min = 1, max = 1}}}"), "npcs/a.lua: \"loot\": \"items\"[1]: \"chance\" must be a number in (0, 1]"},
             {loot("{items = {{item = 'wing', chance = 1, min = 0, max = 1}}}"), "npcs/a.lua: \"loot\": \"items\"[1]: \"min\" must be an integer in [1, 1000000]"},
             {loot("{items = {{item = 'wing', chance = 1, min = 2, max = 1}}}"), "npcs/a.lua: \"loot\": \"items\"[1]: \"max\" must be an integer in [2, 1000000]"},
             {loot("{items = {{item = 'wing', chance = 1, min = 1, max = 1, x = 1}}}"), "npcs/a.lua: \"loot\": \"items\"[1]: unknown key x"},
         }) {
        const auto errors = npc_errors({{"items/wing.lua", kWing}, {"npcs/a.lua", body}});
        const std::string got = errors.size() == 1 ? errors[0] : "<" + joined(errors) + ">";
        check(got == want, "npc loot rejected: want [" + want + "] got [" + got + "]");
    }

    // kast.items and kast.npcDef from a system; both read-only or fresh copies.
    LuaRoot r;
    r.write("items/wing.lua", kWing);
    r.write("npcs/bat.lua", "return {name = 'Bat', template = 'BatT', loot = {gold = {min = 1, max = 3}, "
                            "items = {{item = 'wing', chance = 0.8, min = 1, max = 2}}}}");
    r.write("npcs/ghost.lua", "return {name = 'Ghost', template = 'G'}");
    r.write("systems/items.lua", std::string(kLuaExpect) + R"lua(
return { init = function()
    local w = kast.items.wing
    expect(w.id == "wing" and w.name == "Wing" and w.icon == "icons/wing.png" and w.quality == "poor" and w.maxStack == 20 and
           w.sellPrice == 1 and w.description == "Leathery.", "kast.items: every field")
    expect(kast.items.nope == nil, "kast.items: unknown id is nil")
    raises("kast is read-only", function() kast.items.wing.maxStack = 99 end)
    raises("kast is read-only", function() kast.items.x = {} end)
    local bat = kast.npcDef("Bat")
    expect(bat.name == "Bat" and bat.template == "BatT" and bat.loot.gold.min == 1 and bat.loot.gold.max == 3, "kast.npcDef: name, template, gold")
    local e = bat.loot.items[1]
    expect(e.item == "wing" and e.chance == 0.8 and e.min == 1 and e.max == 2, "kast.npcDef: loot items")
    bat.loot.gold.min = 50
    expect(kast.npcDef("Bat").loot.gold.min == 1, "kast.npcDef returns a fresh copy")
    expect(kast.npcDef("Ghost").loot == nil and kast.npcDef("Nope") == nil, "no loot / no definition: nil")
    raises("kast.npcDef: name must be a string, got 5", function() kast.npcDef(5) end)
    kast.log("items lua done")
end, update = function() end }
)lua");
    TestWorld w({});
    const auto errors = w.world.load_scripts(r.root);
    check(errors.empty(), "items lua: loads " + joined(errors));
    check_lua_expectations("items lua", "items lua done");
}

constexpr const char* kNpcDefs[][2] = {
    {"npcs/guard.lua", "return {name = 'Guard', template = 'GuardTpl', components = {Stats = {hp = 50, tags = {'x', 'y'}}, "
                       "Hp = {cur = 7}, StateMachine = {machine = 'plain'}}}"},
    {"npcs/sub/rat.lua", "return {name = 'Rat', template = 'RatTpl', components = {Stats = {}}}"},
    {"npcs/ghost.lua", "return {name = 'Ghost', template = 'GhostTpl'}"},
};

void write_npc_defs(LuaRoot& r) {
    npc_root(r);
    for (const auto& [path, body] : kNpcDefs) r.write(path, body);
}

// kast.create with opts.npc / opts.components / opts.template; every bad call raises and creates nothing.
constexpr const char* kNpcCreateSystem = R"lua(
local g
local done = false
return {
init = function() g = kast.create("G1", {npc = "Guard", position = {x = 1010, y = 0, z = 1000}}) end,
update = function()
    if done then return end
    done = true
    local n = kast.get(g, "Npc")
    expect(n.name == "G1" and n.template == "GuardTpl" and n.npc == "Guard", "{npc}: name from the call, template and npc from the definition")
    local s = kast.get(g, "Stats")
    expect(s.hp == 50 and #s.tags == 2 and s.tags[2] == "y" and s.kind == "b" and s.flag == false and s.pos.x == 1, "{npc}: definition data over the field defaults")
    expect(kast.get(g, "Hp").cur == 7 and kast.get(g, "Hp").max == 10, "{npc}: unlisted fields at their defaults")
    expect(kast.fsm.state(g) == "a", "{npc}: the definition's StateMachine entered its initial state at create (in init)")
    expect(not kast.has(g, "Secret") and not kast.has(g, "Spawn"), "{npc}: only the listed components")
    local rat = kast.create("R", {npc = "Rat"})
    local rs = kast.get(rat, "Stats")
    expect(rs.hp == 10 and rs.kind == "b" and rs.tags[1] == "a" and not kast.has(rat, "Hp") and not kast.has(rat, "StateMachine"), "Stats = {}: all defaults")
    local ghost = kast.create("Gh", {npc = "Ghost"})
    expect(not kast.has(ghost, "Stats") and not kast.has(ghost, "Hp") and not kast.has(ghost, "StateMachine"), "no components: none")
    local o = kast.create("G2", {npc = "Guard", components = {Stats = {kind = "a"}, Secret = {v = 3}, StateMachine = {initialState = "b"}}})
    local os = kast.get(o, "Stats")
    expect(os.kind == "a" and os.hp == 50 and os.tags[2] == "y", "opts.components merges per field over the definition")
    expect(kast.get(o, "Secret").v == 3, "opts.components adds a type the definition lacks")
    expect(kast.fsm.state(o) == "b", "opts.components: StateMachine initialState over the definition")
    local again = kast.create("G3", {npc = "Guard"})
    expect(kast.get(again, "Stats").kind == "b" and not kast.has(again, "Secret"), "an override leaves the definition unchanged")
    local t = kast.create("T1", {template = "Tpl", components = {Stats = {hp = 5}}})
    local tn = kast.get(t, "Npc")
    expect(tn.npc == "" and tn.template == "Tpl" and kast.get(t, "Stats").hp == 5 and kast.get(t, "Stats").kind == "b", "{template, components}: npc empty, data over the defaults")
    local plain = kast.create("P", {template = "Tpl"})
    expect(kast.get(plain, "Npc").npc == "" and not kast.has(plain, "Stats"), "{template}: npc empty, no components")
    local count = function() local c = 0 ; for _ in kast.query("Npc") do c = c + 1 end ; return c end
    local before = count()
    local one = "kast.create: opts needs exactly one of npc (an npcs/ definition) and template"
    for _, c in ipairs({
        {one, {npc = "Guard", template = "T"}},
        {one, {}},
        {one, {components = {Stats = {}}}},
        {'kast.create: unknown npc "Nope" (no definition in npcs/)', {npc = "Nope"}},
        {"kast.create: npc must be a non-empty string of at most 255 bytes, got 5", {npc = 5}},
        {"kast.create: npc must be a non-empty string of at most 255 bytes, got ", {npc = ""}},
        {'kast.create: "Stats": "hp" must be <= 100', {npc = "Guard", components = {Stats = {hp = 101}}}},
        {'kast.create: "Stats": unknown field "nope"', {npc = "Guard", components = {Stats = {nope = 1}}}},
        {'kast.create: "Stats": "kind" unknown value "z"', {template = "T", components = {Stats = {kind = "z"}}}},
        {'kast.create: unknown component "Nope"', {npc = "Guard", components = {Nope = {}}}},
        {'kast.create: "Npc" is read-only on the server', {template = "T", components = {Npc = {}}}},
        {'kast.create: "Transform" is read-only on the server', {npc = "Guard", components = {Transform = {}}}},
        {'kast.create: "StateMachine": "machine" "nope" names no valid state machine definition in fsms/', {npc = "Guard", components = {StateMachine = {machine = "nope"}}}},
        {'kast.create: "components" must be a table {Type = {field = value, ...}, ...}, got 5', {npc = "Guard", components = 5}},
        {'kast.create: "position" must be a table', {npc = "Guard", components = {Stats = {hp = 1}}, position = 5}},
    }) do raises(c[1], function() kast.create("N", c[2]) end) end
    expect(count() == before, "no failed create made an NPC")
    kast.log("npc create done")
end }
)lua";

void checkNpcCreateFromDefinitions() {
    LogCapture capture;
    LuaRoot r;
    write_npc_defs(r);
    r.write("systems/create.lua", std::string(kLuaExpect) + kNpcCreateSystem);
    TestWorld w(r.defs().defs);
    const auto errors = w.world.load_scripts(r.root);
    check(errors.empty(), "npc create: loads " + joined(errors));
    w.spawn(1, 301, "Alice", 1000, 1000);
    auto& reg = w.world.registry();
    w.world.tick(T0);
    check_lua_expectations("npc create", "npc create done");
    for (const auto& s : w.world.script_status()) check(s.ok, "npc create: system still enabled " + s.error);
    w.world.tick(T0 + std::chrono::milliseconds(50)); // the NPCs update created come into view

    // AOI enter: the Spawn, then the aoi components of the full set; none / owner ones reach nobody.
    entt::entity g = entt::null, o = entt::null;
    for (auto [e, n] : reg.view<world::Npc>().each()) {
        if (n.name == "G1") g = e;
        if (n.name == "G2") o = e;
    }
    check(g != entt::null && o != entt::null, "npc create: G1 and G2 exist");
    if (g == entt::null || o == entt::null) return;
    const auto& toA = w.sent.bySession[1];
    size_t spawnAt = SIZE_MAX, hpAt = SIZE_MAX;
    for (size_t i = 0; i < toA.size(); ++i) {
        if (const auto* s = std::get_if<proto::Spawn>(&toA[i]); s && s->entityId == wire(g) && spawnAt == SIZE_MAX) spawnAt = i;
        if (const auto* u = std::get_if<proto::ComponentUpdate>(&toA[i]); u && u->entityId == wire(g) && u->name == "Hp" && hpAt == SIZE_MAX)
            hpAt = i;
    }
    check(spawnAt != SIZE_MAX && hpAt != SIZE_MAX && spawnAt < hpAt, "npc create: a player gets the NPC's Spawn, then its aoi Hp");
    const auto hp = w.sent.updates(1, wire(g), "Hp");
    check(hp.size() == 1 && Json::parse(hp[0].json) == Json::parse(R"({"cur":7.0,"max":10.0})"),
          "npc create: Hp sent once, with the definition's data: " + (hp.empty() ? std::string() : hp[0].json));
    check(w.sent.updates(1, wire(g), "Stats").empty() && w.sent.updates(1, wire(o), "Secret").empty() &&
              !w.sent.updates(1, wire(o), "Hp").empty(),
          "npc create: replicate none and owner components reach nobody; G2's Hp reaches the viewer");
}

// Spawn files instantiate definitions: exactly the definition's components and data, fresh on every respawn; an
// unknown "npc" refuses the start, naming the file and the spawn.
void checkNpcSpawnDefinitions() {
    LogCapture capture;
    LuaRoot lua, maps;
    write_npc_defs(lua);
    maps.write("a/map.json", R"({"version":1,"id":1,"tiles":[[0,0,7,7]],"spawn":{"pos":[1024,0,1024]}})");
    maps.write("a/.server/spawns/4_4.json", R"({"version":2,"spawns":[{"id":"g","npc":"Guard","pos":[1030,0,1030],"respawnSec":1}]})");
    std::vector<std::string> mapErrors;
    TestWorld w(lua.defs().defs, {}, world::Maps::load(maps.root, mapErrors));
    const auto errors = w.world.load_scripts(lua.root);
    check(mapErrors.empty() && errors.empty(), "npc spawn: loads " + joined(mapErrors) + joined(errors));
    auto& reg = w.world.registry();
    w.world.tick(T0);
    const auto first = [&] { return reg.view<world::Npc>().empty() ? entt::entity{entt::null} : reg.view<world::Npc>().front(); };
    const auto g = first();
    check(g != entt::null, "npc spawn: placed on the first tick");
    if (g == entt::null) return;
    const auto& npc = reg.get<world::Npc>(g);
    check(npc.name == "Guard" && npc.templateName == "GuardTpl" && npc.npc == "Guard", "npc spawn: name, template, npc from the definition");
    const auto has = [&](entt::entity e, const char* type) { return lua::pool(reg, type).contains(e); };
    const Json stats = Json::parse(R"({"hp":50,"flag":false,"kind":"b","pos":[1.0,2.0,3.0],"tags":["x","y"]})");
    check(has(g, "Stats") && has(g, "Hp") && has(g, "StateMachine") && !has(g, "Secret") && w.value(g, "Stats") == stats &&
              w.value(g, "Hp") == Json::parse(R"({"cur":7.0,"max":10.0})"),
          "npc spawn: exactly the definition's components, with its data: " + w.value(g, "Stats").dump());
    check(w.world.fsm().state(lua::EntityId{wire(g)}) == "a", "npc spawn: StateMachine in its initial state");

    lua::pool(reg, "Stats").get(g).value["hp"] = 1;
    w.world.fsm().change(lua::EntityId{wire(g)}, "b");
    w.world.destroy_npc(g);
    w.world.tick(T0 + std::chrono::seconds(1));
    const auto again = first();
    check(again != entt::null && w.value(again, "Stats") == stats && w.world.fsm().state(lua::EntityId{wire(again)}) == "a",
          "npc spawn: a respawned NPC starts with fresh definition data and machine state");

    LuaRoot badMaps;
    badMaps.write("a/map.json", R"({"version":1,"id":1,"tiles":[[0,0,0,0]],"spawn":{"pos":[10,0,10]}})");
    badMaps.write("a/.server/spawns/0_0.json", R"({"version":2,"spawns":[{"id":"x","npc":"Nope","pos":[10,0,10],"respawnSec":5},
        {"id":"y","npc":"Guard","pos":[20,0,10],"respawnSec":5}]})");
    TestWorld bad(lua.defs().defs, {}, world::Maps::load(badMaps.root, mapErrors));
    check(bad.world.load_scripts(lua.root) ==
              std::vector<std::string>{"maps/a/.server/spawns/0_0.json: spawn \"x\": unknown npc \"Nope\" (no definition in npcs/)"},
          "npc spawn: an npc with no definition refuses the start, naming the file and the spawn");
}

#ifdef KAST_SAMPLE_MAPS
// The sample hills map (id 3): its four PNG heightmaps load, and the ground at the portal destination is the
// terrain's, not hills_gate's y = 30.
void checkGroundSample() {
    std::vector<std::string> errors;
    const auto maps = world::Maps::load(KAST_SAMPLE_MAPS, errors);
    check(errors.empty(), "sample maps load with their heights " + joined(errors));
    const auto y = maps.ground(3, 256, 256);
    check(y && *y > 1 && *y < 79 && *y != 30, "hills: ground at (256, 256) from its heightmaps: " + std::to_string(y.value_or(-1)));
    check(maps.ground(1, 1024, 1024) == 0.f, "start: no heights files, flat at heightMin 0");
    // Its baked navtiles (maps/hills/.server/nav): the bake's slope limit is the client's nav.maxSlopeDeg (40).
    const auto at = [&](float x, float z) { return proto::Vec3{x, *maps.ground(3, x, z), z}; };
    check(maps.nav().has(3) && !maps.nav().has(1), "hills has a navmesh, start none");
    check(maps.nav().on_mesh(3, at(215, 331), 6) && maps.nav().on_mesh(3, at(256, 256), 6),
          "hills: on_mesh on a 32 degree slope (215, 331) and at the portal destination");
    check(!maps.nav().on_mesh(3, at(185, 317.5f), 6) && maps.nav().on_mesh(3, at(185, 331), 6) &&
              maps.nav().path(3, at(185, 310), at(185, 331)).empty(),
          "hills: on_mesh false on the mesa's 81 degree wall; its top is walkable but no path leads up");
    // Its zone object fixtures (maps/hills/objects, .server/interactables): the obstacles are cut out of the baked
    // navtiles, the visual is not, and the server knows the interactable.
    check(!maps.nav().on_mesh(3, at(266, 248), 6) && !maps.nav().on_mesh(3, at(160, 331), 6) &&
              maps.nav().on_mesh(3, at(262, 248), 6),
          "hills: the obstacle crates (266, 248) and (160, 331) are off the navmesh, 3 m beside them on");
    check(maps.nav().on_mesh(3, at(248, 244), 6), "hills: the visual crate (248, 244) stays walkable");
    const map::Interactable* shrine = maps.interactable(3, 1);
    check(shrine && shrine->event == "test.shrine" && shrine->data["msg"] == "hello" && shrine->name == "Test Shrine" &&
              !maps.interactable(1, 1),
          "hills: interactable 1 (test.shrine, data {msg = hello}) is on map 3 only");
}
#endif

#ifdef KAST_STARTER_MMO
// starter-mmo's Meadow Shrine (maps/meadow objects + .server/interactables, systems/shrines.lua): activated in range it
// restores the player's Health and sends "shrine.blessing" with the shrine's data.message; out of range, nothing.
void checkStarterMmoShrine() {
    LogCapture capture;
    const fs::path root = KAST_STARTER_MMO;
    const auto defs = lua::load_component_defs(root, world::kReservedComponentNames);
    std::vector<std::string> mapErrors;
    auto maps = world::Maps::load(root / "maps", mapErrors);
    check(mapErrors.empty(), "starter-mmo shrine: maps load " + joined(mapErrors));
    const map::Interactable* shrine = maps.interactable(10, 1);
    check(shrine && shrine->event == "shrine.activated" && shrine->name == "Meadow Shrine",
          "starter-mmo shrine: the meadow's interactable 1 is the Meadow Shrine");
    if (!shrine) return;
    world::WorldRules rules;
    rules.startMap = 10;
    TestWorld w(defs.defs, rules, std::move(maps));
    const auto errors = w.world.load_scripts(root);
    check(errors.empty(), "starter-mmo shrine: systems load " + joined(errors));
    if (!errors.empty()) return;
    const auto pilgrim = w.spawn(1, 51, "Pilgrim", shrine->pos[0], shrine->pos[2] - 3, "", 10);
    const auto faraway = w.spawn(2, 52, "Faraway", shrine->pos[0] + 20, shrine->pos[2], "", 10);
    check(map::footprint_distance(shrine->shape, shrine->pos, shrine->yaw, shrine->pos[0], shrine->pos[2] - 3) < shrine->range,
          "starter-mmo shrine: the Pilgrim stands within range of its edge");
    TimePoint t = T0;
    w.world.tick(t);
    w.world.tick(t += std::chrono::milliseconds(50));
    for (auto e : {pilgrim, faraway}) lua::pool(w.world.registry(), "Health").get(e).value["current"] = 5;
    w.sent.clear();
    w.world.push_interact(pilgrim, 1);
    w.world.push_interact(faraway, 1);
    for (int i = 0; i < 3; ++i) w.world.tick(t += std::chrono::milliseconds(50));
    const auto blessings = [&](uint32_t sid) {
        std::vector<std::string> out;
        for (const auto& e : w.sent.of<proto::ServerEvent>(sid))
            if (e.name == "shrine.blessing") out.push_back(Json::parse(e.json).value("text", ""));
        return out;
    };
    const Json h = w.value(pilgrim, "Health");
    check(blessings(1) == std::vector<std::string>{"The shrine hums softly. You feel refreshed."} && h["current"] == h["max"],
          "starter-mmo shrine: in range, Health back to max and the blessing sent: " + h.dump());
    check(blessings(2).empty() && w.value(faraway, "Health")["current"] == 5, "starter-mmo shrine: 20 m away, nothing");
    check(!said("FAIL") && log_index("\"level\":\"error\"") == SIZE_MAX, "starter-mmo shrine: no Lua errors");
}
#endif

// --- zone objects (docs/services/world/maps.html#objects) ---

constexpr const char* kVisualEntry = R"({"type":"visual","prefab":"prefabs/grass.prefab","pos":[10,0,10]})";

// objects/<tx>_<tz>.json: every field, then each problem as one "objects[i]: ..." error that skips only its entry.
void checkObjectParsing() {
    const auto parse = [](const std::string& text) { return map::parse_objects(nlohmann::json::parse(text), {0, 0}); };
    const auto good = parse(R"({"version":1,"objects":[
        {"type":"visual","prefab":"prefabs/grass.prefab","pos":[10,1,10],"yaw":1.5,"scale":0.5,"seed":3},
        {"type":"obstacle","prefab":"prefabs/rock.prefab","pos":[20,2,20],"shape":{"cylinder":{"radius":2,"height":3}}},
        {"type":"interactable","prefab":"prefabs/shrine.prefab","pos":[30,0,30],"id":7,"name":"Shrine",
         "shape":{"box":{"halfExtents":[1,2,1.5]}}}]})");
    check(good.errors.empty() && good.objects.size() == 3, "objects: a valid file parses: " + joined(good.errors));
    if (good.objects.size() == 3) {
        const auto& [v, o, i] = std::tie(good.objects[0], good.objects[1], good.objects[2]);
        check(v.type == map::ObjectType::Visual && v.prefab == "prefabs/grass.prefab" && v.pos == std::array<float, 3>{10, 1, 10} &&
                  v.yaw == 1.5f && v.scale == 0.5f && !v.shape,
              "objects: a visual's fields (unknown keys ignored, no shape)");
        check(o.type == map::ObjectType::Obstacle && o.scale == 1 && o.yaw == 0 && o.shape &&
                  o.shape->kind == map::Shape::Kind::Cylinder && o.shape->radius == 2 && o.shape->height == 3,
              "objects: an obstacle's cylinder, scale 1 and yaw 0 by default");
        check(i.type == map::ObjectType::Interactable && i.id == 7 && i.name == "Shrine" && i.shape &&
                  i.shape->kind == map::Shape::Kind::Box && i.shape->halfExtents == std::array<float, 3>{1, 2, 1.5f},
              "objects: an interactable's id, name and box");
    }
    const std::vector<std::pair<std::string, std::string>> bad = {
        {"5", "objects[1]: must be an object"},
        {R"({"type":"tree","prefab":"a.prefab","pos":[1,0,1]})", "objects[1]: \"type\" must be \"visual\", \"obstacle\" or \"interactable\""},
        {R"({"type":"visual","pos":[1,0,1]})", "objects[1]: \"prefab\" must be a project-relative .prefab path"},
        {R"({"type":"visual","prefab":"models/rock.glb","pos":[1,0,1]})", "objects[1]: \"prefab\" must be a project-relative .prefab path"},
        {R"({"type":"visual","prefab":"a.prefab"})", "objects[1]: \"pos\" must be [x, y, z]"},
        {R"({"type":"visual","prefab":"a.prefab","pos":[300,0,1]})", "objects[1]: \"pos\" is outside tile 0_0"},
        {R"({"type":"visual","prefab":"a.prefab","pos":[1,0,1],"yaw":"north"})", "objects[1]: \"yaw\" must be a number (radians)"},
        {R"({"type":"visual","prefab":"a.prefab","pos":[1,0,1],"scale":0})", "objects[1]: \"scale\" must be a number > 0"},
        {R"({"type":"obstacle","prefab":"a.prefab","pos":[1,0,1]})", "objects[1]: a blocking object needs \"shape\""},
        {R"({"type":"obstacle","prefab":"a.prefab","pos":[1,0,1],"shape":{"sphere":{"radius":1}}})",
         "objects[1]: \"shape\" must be {\"box\": {\"halfExtents\": [x, y, z]}} or {\"cylinder\": {\"radius\": r, \"height\": h}}"},
        {R"({"type":"obstacle","prefab":"a.prefab","pos":[1,0,1],"shape":{"box":{"halfExtents":[1,-1,1]}}})",
         "objects[1]: \"shape.box.halfExtents\" must be [x, y, z], each > 0"},
        {R"({"type":"obstacle","prefab":"a.prefab","pos":[1,0,1],"shape":{"cylinder":{"radius":1}}})",
         "objects[1]: \"shape.cylinder\" must be {\"radius\": r, \"height\": h}, each > 0"},
        {R"({"type":"interactable","prefab":"a.prefab","pos":[1,0,1],"shape":{"cylinder":{"radius":1,"height":1}}})",
         "objects[1]: \"id\" must be an integer in [1, 4294967295]"},
        {R"({"type":"interactable","prefab":"a.prefab","pos":[1,0,1],"id":0,"shape":{"cylinder":{"radius":1,"height":1}}})",
         "objects[1]: \"id\" must be an integer in [1, 4294967295]"},
    };
    for (const auto& [entry, want] : bad) {
        const auto got = parse(std::string(R"({"version":1,"objects":[)") + kVisualEntry + "," + entry + "]}");
        check(got.errors == std::vector<std::string>{want} && got.objects.size() == 1,
              "objects: bad entry skipped, the rest kept: want [" + want + "] got [" + joined(got.errors) + "]");
    }
    const std::string shrine = R"({"type":"interactable","prefab":"a.prefab","pos":[1,0,1],"id":4,"shape":{"cylinder":{"radius":1,"height":1}}})";
    const auto dup = parse(R"({"version":1,"objects":[)" + shrine + "," + shrine + "]}");
    check(dup.errors == std::vector<std::string>{"objects[1]: duplicate id 4"} && dup.objects.size() == 1,
          "objects: a repeated id is an error, the first kept: " + joined(dup.errors));
    for (const auto& [text, want] : std::vector<std::pair<std::string, std::string>>{
             {"[]", "must be a JSON object"}, {R"({"version":2,"objects":[]})", "\"version\" must be 1"},
             {R"({"version":1})", "\"objects\" must be a list"}}) {
        const auto got = parse(text);
        check(got.errors == std::vector<std::string>{want} && got.objects.empty(), "objects: file error " + want + ": " + joined(got.errors));
    }

    // .server/interactables/<tx>_<tz>.json: fail fast, like spawn files.
    const auto inter = [](const std::string& entries, std::string& err) {
        return map::parse_interactables(nlohmann::json::parse(R"({"version":1,"interactables":[)" + entries + "]}"), {0, 0}, err);
    };
    std::string err;
    const auto list = inter(R"({"id":7,"pos":[30,0,30],"yaw":0.5,"shape":{"box":{"halfExtents":[1,2,1]}},"range":4,
        "event":"shrine.activated","data":{"msg":"hi","n":[1,2]},"name":"Shrine","note":"kept"},
        {"id":8,"pos":[40,0,40],"shape":{"cylinder":{"radius":2,"height":1}},"event":"well.used"})", err);
    check(list && list->size() == 2, "interactables: a valid file parses " + err);
    if (list && list->size() == 2) {
        const auto& a = (*list)[0];
        check(a.id == 7 && a.yaw == 0.5f && a.range == 4 && a.event == "shrine.activated" && a.data["msg"] == "hi" &&
                  a.name == "Shrine" && a.shape.kind == map::Shape::Kind::Box,
              "interactables: every field");
        check((*list)[1].range == 3 && (*list)[1].data.is_null() && (*list)[1].name.empty(),
              "interactables: range 3 by default, no data, no name");
    }
    const std::string ok = R"("pos":[1,0,1],"shape":{"cylinder":{"radius":1,"height":1}},"event":"e")";
    std::string deep = "1";
    for (int i = 0; i < 32; ++i) deep = "{\"a\":" + deep + "}";
    for (const auto& [entries, want] : std::vector<std::pair<std::string, std::string>>{
             {"7", "interactables[0]: must be an object"},
             {R"({"pos":[1,0,1]})", "interactables[0]: \"id\" must be an integer in [1, 4294967295]"},
             {R"({"id":5,)" + ok + R"(},{"id":5,)" + ok + "}", "interactable 5: duplicate id"},
             {R"({"id":5,"pos":[1,0,999],"shape":{"cylinder":{"radius":1,"height":1}},"event":"e"})", "interactable 5: \"pos\" is outside tile 0_0"},
             {R"({"id":5,"pos":[1,0,1],"event":"e"})",
              "interactable 5: \"shape\" must be {\"box\": {\"halfExtents\": [x, y, z]}} or {\"cylinder\": {\"radius\": r, \"height\": h}}"},
             {R"({"id":5,"range":0,)" + ok + "}", "interactable 5: \"range\" must be a number > 0 (metres)"},
             {R"({"id":5,"pos":[1,0,1],"shape":{"cylinder":{"radius":1,"height":1}},"event":""})",
              "interactable 5: \"event\" must be a non-empty string of at most 255 bytes"},
             {R"({"id":5,"data":[1],)" + ok + "}", "interactable 5: \"data\" must be an object"},
             {R"({"id":5,"data":)" + deep + "," + ok + "}", "interactable 5: \"data\" is nested more than 31 deep"},
             {R"({"id":5,"name":3,)" + ok + "}", "interactable 5: \"name\" must be a string"}}) {
        err.clear();
        check(!inter(entries, err) && err == want, "interactables: want [" + want + "] got [" + err + "]");
    }

    // Footprints and the horizontal distance to their edge.
    map::Shape wall;
    wall.halfExtents = {2, 1, 1};
    map::Shape post;
    post.kind = map::Shape::Kind::Cylinder;
    post.radius = 2;
    const std::array<float, 3> o{0, 5, 0};
    const auto close_to = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
    check(close_to(map::footprint_distance(wall, o, 0, 3, 0), 1) && close_to(map::footprint_distance(wall, o, 0, 0, 3), 2) &&
              map::footprint_distance(wall, o, 0, 1.9f, 0.9f) == 0 && close_to(map::footprint_distance(wall, o, 0, 5, 5), 5),
          "footprint_distance: to a box's edge, 0 inside, to its corner");
    const float quarter = std::numbers::pi_v<float> / 2;
    check(close_to(map::footprint_distance(wall, o, quarter, 0, 3), 1) && close_to(map::footprint_distance(wall, o, quarter, 3, 0), 2),
          "footprint_distance: a box turned by yaw (its long side now along z)");
    check(close_to(map::footprint_distance(post, o, 0, 5, 0), 3) && map::footprint_distance(post, o, 0, 1, 1) == 0,
          "footprint_distance: to a cylinder's edge, 0 inside");
    const auto corners = map::footprint(wall, o, quarter);
    check(corners.size() == 4 && close_to(std::fabs(corners[0][1]), 2) && close_to(std::fabs(corners[0][0]), 1) &&
              map::footprint(post, o, 0).size() == 16,
          "footprint: a box's 4 corners turned by yaw, a cylinder's 16 points");
}

constexpr const char* kObjectsManifest = R"({"version":1,"id":1,"tiles":[[0,0,1,0]],"spawn":{"pos":[10,0,10]}})";
constexpr const char* kShrineFile = R"({"version":1,"interactables":[
    {"id":7,"pos":[100,0,100],"shape":{"box":{"halfExtents":[1,1,1]}},"event":"shrine.activated","data":{"msg":"hi"}}]})";

// Maps::load: interactables files of owned tiles, every problem refusing the start with the file.
void checkInteractableLoading() {
    const auto errors = [](const std::vector<std::pair<std::string, std::string>>& files) {
        LuaRoot r;
        r.write("m/map.json", kObjectsManifest);
        for (const auto& [path, body] : files) r.write("m/.server/interactables/" + path, body);
        std::vector<std::string> errs;
        world::Maps::load(r.root, errs);
        return joined(errs);
    };
    LuaRoot r;
    r.write("m/map.json", kObjectsManifest).write("m/.server/interactables/0_0.json", kShrineFile);
    std::vector<std::string> errs;
    const auto maps = world::Maps::load(r.root, errs);
    const auto* shrine = maps.interactable(1, 7);
    check(errs.empty() && shrine && shrine->event == "shrine.activated" && !maps.interactable(1, 8) && !maps.interactable(2, 7),
          "interactables load by map and id " + joined(errs));
    const std::string well = R"({"version":1,"interactables":[{"id":7,"pos":[300,0,10],"shape":{"cylinder":{"radius":1,"height":1}},"event":"w"}]})";
    check(errors({{"0_0.json", kShrineFile}, {"1_0.json", well}}) ==
              "maps/m/.server/interactables/1_0.json: interactable 7: duplicate id on this map",
          "interactables: an id used twice on a map is refused: " + errors({{"0_0.json", kShrineFile}, {"1_0.json", well}}));
    check(errors({{"shrine.json", kShrineFile}}) == "maps/m/.server/interactables/shrine.json: file name must be <tx>_<tz>.json",
          "interactables: a bad file name is refused");
    check(errors({{"5_5.json", kShrineFile}}) == "maps/m/.server/interactables/5_5.json: tile is not on the map",
          "interactables: a file for a tile not on the map is refused");
    check(errors({{"0_0.json", "{nope"}}) == "maps/m/.server/interactables/0_0.json: not valid JSON", "interactables: bad JSON refused");
    check(errors({{"1_0.json", kShrineFile}}) == "maps/m/.server/interactables/1_0.json: interactable 7: \"pos\" is outside tile 1_0",
          "interactables: a parse error is refused with the file: " + errors({{"1_0.json", kShrineFile}}));
}

// Interact: in range on the player's map publishes the object's event {entity, map, object, data}; unknown id, another
// map's object and out of range publish nothing and log at debug.
void checkInteract() {
    LogCapture capture;
    log::set_level(log::Level::debug);
    LuaRoot r;
    r.write("components/report.lua", kReport);
    r.write("systems/shrine.lua", R"(
local got = {}
return {
    init = function()
        kast.subscribe("shrine.activated", function(e)
            got[#got + 1] = string.format("%d %d %d %s", e.entity, e.map, e.object, e.data and e.data.msg or "nil")
        end)
        kast.subscribe("well.used", function(e)
            got[#got + 1] = string.format("%d %d %d %s", e.entity, e.map, e.object, tostring(e.data))
        end)
    end,
    onEnter = function(e) kast.add(e, "Report") end,
    update = function(dt) for e in kast.query("Report") do kast.set(e, "Report", {lines = got}) end end,
})");
    world::Maps maps = test_maps();
    map::Interactable shrine;
    shrine.id = 7;
    shrine.pos = {100, 0, 100};
    shrine.shape.halfExtents = {1, 1, 1};
    shrine.event = "shrine.activated";
    shrine.data = {{"msg", "hi"}};
    maps.add_interactable(1, shrine);
    map::Interactable well = shrine;
    well.id = 8;
    well.pos = {200, 0, 100};
    well.shape.kind = map::Shape::Kind::Cylinder;
    well.shape.radius = 2;
    well.range = 1;
    well.event = "well.used";
    well.data = nullptr;
    maps.add_interactable(1, well);
    map::Interactable cave = shrine;
    cave.id = 9;
    maps.add_interactable(2, cave);
    TestWorld w(r.defs().defs, {}, std::move(maps));
    check(w.world.load_scripts(r.root).empty(), "interact: system loads");
    const auto alice = w.spawn(1, 41, "Alice", 103.9f, 100);  // 2.9 m from the shrine's edge: in range 3
    const auto bob = w.spawn(2, 42, "Bob", 104.1f, 100);      // 3.1 m: out of range (and far from the well)
    w.world.tick(T0);
    w.world.push_interact(alice, 7);
    w.world.push_interact(alice, 9);    // map 2's
    w.world.push_interact(alice, 1234); // nobody's
    w.world.push_interact(bob, 7);
    w.world.push_interact(bob, 8);      // 94 m from the well
    w.world.tick(T0 + std::chrono::milliseconds(50));
    w.world.tick(T0 + std::chrono::milliseconds(100));
    const Json lines = w.value(bob, "Report").value("lines", Json::array());
    check(lines == Json::array({std::to_string(wire(alice)) + " 1 7 hi"}),
          "interact: only Alice's in-range activation publishes {entity, map, object, data}: " + lines.dump());
    check(said_at("interact rejected").size() == 4 && logged("\"reason\":\"not on this map\"") && logged("\"object\":1234") &&
              logged("\"reason\":\"out of range\""),
          "interact: the 4 rejections are logged at debug");
    // The well (no data): data nil in Lua.
    const auto carol = w.spawn(3, 43, "Carol", 202.5f, 100);
    w.world.tick(T0 + std::chrono::milliseconds(150));
    w.world.push_interact(carol, 8);
    w.world.tick(T0 + std::chrono::milliseconds(200));
    w.world.tick(T0 + std::chrono::milliseconds(250));
    const Json more = w.value(carol, "Report").value("lines", Json::array());
    check(more.size() == 2 && more[1] == std::to_string(wire(carol)) + " 1 8 nil", "interact: no data -> e.data nil: " + more.dump());
    log::set_level(log::Level::info);
    for (const auto& s : w.world.script_status()) check(s.ok, "interact: system still enabled " + s.error);
}

// The bake cuts obstacles and interactables (box and cylinder, a neighbour tile's too) out of the navmesh, with
// agentRadius clearance; visuals stay walkable. Then a move into one is off_mesh, and paths go round.
void checkObstacleNavmesh() {
    std::string err;
    const auto info = map::parse_manifest(nlohmann::json::parse(
        R"({"version":1,"id":1,"tiles":[[0,0,1,0]],"heightRange":[0,10],"spawn":{"pos":[10,0,10]}})"), err);
    const auto objects = map::parse_objects(nlohmann::json::parse(R"({"version":1,"objects":[
        {"type":"obstacle","prefab":"w.prefab","pos":[128,0,128],"shape":{"box":{"halfExtents":[2,2,40]}}},
        {"type":"interactable","prefab":"s.prefab","pos":[60,0,60],"id":1,"shape":{"cylinder":{"radius":3,"height":2}}},
        {"type":"visual","prefab":"g.prefab","pos":[200,0,200]},
        {"type":"obstacle","prefab":"r.prefab","pos":[254,0,60],"yaw":0.785,"shape":{"box":{"halfExtents":[4,1,4]}}}]})"), {0, 0});
    check(objects.errors.empty() && objects.objects.size() == 4, "obstacle bake: fixture objects parse " + joined(objects.errors));
    const map::TileHeights flat;
    LuaRoot r;
    r.write("m/map.json", R"({"version":1,"id":1,"tiles":[[0,0,1,0]],"heightRange":[0,10],"spawn":{"pos":[10,0,10]}})");
    int plainPolys = 0;
    for (int tx : {0, 1}) {
        const auto plain = mapbake::bake_tile(*info, flat, {tx, 0}, err);
        // Tile 1_0 gets tile 0_0's objects too: the rock at x 254 reaches over the seam (the mapbake CLI passes neighbours').
        const auto baked = mapbake::bake_tile(*info, flat, {tx, 0}, err, objects.objects);
        check(plain && baked && baked->polys > plain->polys, std::format("obstacle bake: tile {}_0 has more polys with its "
              "obstacles cut out ({} against {}) {}", tx, baked ? baked->polys : 0, plain ? plain->polys : 0, err));
        if (tx == 0 && plain) plainPolys = plain->polys;
        if (baked) r.write(std::format("m/.server/nav/{}_0.navtile", tx), baked->navtile);
    }
    std::vector<std::string> errors;
    world::Maps maps = world::Maps::load(r.root, errors);
    check(errors.empty() && plainPolys > 0, "obstacle bake: navtiles load " + joined(errors));
    const auto on = [&](float x, float z) { return maps.nav().on_mesh(1, {x, 0, z}, 6); };
    check(!on(128, 128) && !on(129.5f, 160) && on(133, 128), "obstacle bake: the wall's box is off the mesh, 3 m beside it on");
    check(!on(60, 60) && !on(62, 60) && on(66, 60), "obstacle bake: the interactable's cylinder is off the mesh, beyond it on");
    check(on(200, 200) && on(100, 30), "obstacle bake: a visual and open ground stay walkable");
    check(!on(254, 60) && !on(257, 60) && on(266, 60), "obstacle bake: a turned box over the tile seam is cut from both tiles");
    const auto path = maps.nav().path(1, {100, 0, 128}, {156, 0, 128});
    bool around = false, through = false;
    for (const auto& p : path) {
        around |= std::fabs(p.z - 128) > 40;
        through |= std::fabs(p.x - 128) < 2 && std::fabs(p.z - 128) < 40;
    }
    check(path.size() > 2 && around && !through, std::format("obstacle bake: the path goes round the wall ({} points)", path.size()));

    const auto corrections = [&](proto::Vec3 from, proto::Vec3 to) {
        std::vector<std::string> errs;
        TestWorld w({}, {}, world::Maps::load(r.root, errs));
        const auto e = w.world.spawn(1, {1, 1, "Walker", 1, from, 0, ""}, T0);
        w.world.tick(T0);
        w.sent.clear();
        w.move(e, {to, 0, 0});
        w.world.tick(T0 + std::chrono::seconds(1));
        return w.sent.of<proto::Correction>(1).size();
    };
    check(corrections({120, 0, 128}, {127, 0, 128}) == 1, "move into the wall: off_mesh -> Correction");
    check(corrections({120, 0, 100}, {124, 0, 100}) == 0, "move beside the wall: accepted");
}

int lua_checks(int& checks) {
    checkCodec();
    checkDefinitionParsing();
    checkDefinitionErrors();
    checkNameRules();
    checkSandbox();
    checkKastApi();
    checkHooks();
    checkCharacterCreation();
    checkErrorsDisableOneSystem();
    checkStartupErrors();
    checkPersistence();
    checkReplication();
    checkEventBus();
    checkLuaEvents();
    checkLuaEventsFromCpp();
    checkLuaEventErrors();
    checkLuaEventFailures();
    checkLuaEventHooks();
    checkClientEvents();
    checkReply();
    checkLuaLogging();
    checkFsmDefinitions();
    checkFsmRules();
    checkFsmQueue();
    checkFsmCallbackErrors();
    checkFsmLuaApi();
    checkFsmEntities();
    checkNpcApi();
    checkNpcCap();
    checkNpcRouting();
    checkNpcDestroyedOnEnter();
    checkNpcShutdown();
    checkSpawnParsing();
    checkSpawnLoading();
    checkRespawn();
    checkSpawnLua();
    checkClientEventsIn();
    checkMoveEpochs();
    checkMovableAndMapSpawn();
#ifdef KAST_STARTER_MMO
    checkStarterMmoCombat();
    checkStarterMmoShrine();
#endif
#ifdef KAST_STARTER_MMO_TEST_LUA
    checkStarterMmoLoot();
    checkStarterMmoClasses();
    checkStarterMmoEquipment();
#endif
    checkNpcDefinitionErrors();
    checkItemsAndLoot();
    checkNpcCreateFromDefinitions();
    checkNpcSpawnDefinitions();
    checkTileMath();
    checkManifests();
    checkHeights();
    checkMapWorld();
    checkMapLua();
    checkAreaShapes();
    checkAreaLoading();
    checkGround();
    checkNavmesh();
    checkObjectParsing();
    checkInteractableLoading();
    checkInteract();
    checkObstacleNavmesh();
#ifdef KAST_SAMPLE_MAPS
    checkGroundSample();
    checkNpcSample();
    checkAreaWorld();
    checkAreaLua();
#else
    std::printf("sample maps not in this build: NPC sample, area world and Lua checks skipped (integration scenarios "
                "16-18 cover them)\n");
#endif
    checks += g_checks;
    return g_failures;
}
