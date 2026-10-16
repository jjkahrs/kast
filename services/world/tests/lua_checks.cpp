// Assert-style checks for the Lua layer: component definitions (every type, option and error), reserved and duplicate
// names, the sandbox, kast.* semantics, hooks, error handling, persistence + drift, component replication routing, the
// event bus (kast.publish/subscribe/unsubscribe), client events (kast.sendTo/sendNearby/broadcast) and state machines
// (definitions, rules, queueing, callback errors, kast.fsm, the StateMachine built-in, C++ / Lua parity) and NPCs
// (kast.create / kast.destroy, Transform rules, AOI and replication routing, shutdown, the sample lua/).
// Lua files are written to a temp folder; the World runs with captured Send/Save, so there is no network or database.
#include "core/log.h"
#include "lua/components.h"
#include "world/components.h"
#include "world/world.h"

#include <codec.h>

#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <map>
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

struct TestWorld {
    Sent sent;
    world::World world;
    explicit TestWorld(lua::Defs defs, world::WorldRules rules = {})
        : world(rules, [this](uint32_t sid, const proto::Message& m) { sent.bySession[sid].push_back(m); },
                [this](const db::CharSave& s) { sent.saves.push_back(s); }, std::move(defs)) {}
    entt::entity spawn(uint32_t sid, uint64_t charId, const std::string& name, float x, float z,
                       const std::string& components = "") {
        db::CharRow row{charId, 1, name, {x, 0, z}, 0.5f, components};
        return world.spawn(sid, row, T0);
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

// --- events (README "Lua components and systems", Events): the same cases as the engine's checks ---

std::vector<std::string> g_log; // log lines while a LogCapture lives

struct LogCapture {
    LogCapture() {
        g_log.clear();
        log::set_test_sink([](const std::string& line) { g_log.push_back(line); });
    }
    ~LogCapture() { log::set_test_sink(nullptr); }
};

// Index of the first captured log line containing part, or SIZE_MAX.
size_t log_index(const std::string& part) {
    for (size_t i = 0; i < g_log.size(); ++i)
        if (contains(g_log[i], part)) return i;
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

// --- state machines (README "Lua components and systems", State machines) ---

// Indexes of the captured log lines whose "msg" contains part (log lines are JSON: quotes in msg are escaped).
std::vector<size_t> said_at(const std::string& part) {
    std::vector<size_t> out;
    for (size_t i = 0; i < g_log.size(); ++i) {
        const auto line = nlohmann::json::parse(g_log[i], nullptr, false);
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

    g_log.clear();
    check(fsm.canChange("door", "open") && fsm.canChange("door", "locked") && fsm.canChange("door", "broken") &&
              !fsm.canChange("door", "closed") && !fsm.canChange("door", "flying"),
          "fsm: canChange: listed, omitted and \"*\" allowed; self not listed and unknown denied");
    check(g_log.empty() && fsm.state("door") == "closed", "fsm: canChange has no side effects and logs nothing");

    check(!fsm.change("door", "closed") &&
              said(R"(State machine "door": "closed" can't be entered from "closed" (allowedFrom); staying in "closed")"),
          "fsm: a self-transition not allowed by allowedFrom is denied with the engine's warning");
    check(!fsm.change("door", "flying") && said(R"(State machine "door": unknown state "flying"; staying in "closed")"),
          "fsm: an unknown state is denied with a warning");
    check(said_at("fsms/door.lua:").empty() && fsm.state("door") == "closed", "fsm: a denial runs no callbacks");
    w.world.tick(T0 + std::chrono::milliseconds(50));
    check(changed.empty(), "fsm: a denial publishes no event");

    g_log.clear();
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
    g_log.clear();
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
    g_log.clear();
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
    g_log.clear();
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
    g_log.clear();
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

// --- NPCs (README "Lua components and systems", NPCs) ---

// Every kast.create / kast.destroy / NPC Transform rule from Lua; each bad call raises and changes nothing.
constexpr const char* kNpcApiSystem = R"lua(
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
    expect(n.name == "Guard" and n.template == "GuardTpl", "Npc {name, template}")
    local tf = kast.get(g, "Transform")
    expect(tf.position.x == 1024 and tf.position.y == 0 and tf.position.z == 1024 and tf.heading == 0, "default: spawn point, heading 0")
    raises('entity ' .. g .. ' has no "Character"', function() kast.get(g, "Character") end)
    local h = kast.create("Far", {template = "T", position = {x = -5, y = 1000, z = 5000}, heading = 2})
    tf = kast.get(h, "Transform")
    expect(tf.position.x == 0 and tf.position.y == 500 and tf.position.z == 2048 and tf.heading == 2, "position clamped into the zone and MIN_Y/MAX_Y")
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
        {"kast.create: opts must be a table {template = ...}, got nil", "N", nil},
        {"kast.create: opts must be a table {template = ...}, got 5", "N", 5},
        {tplErr .. "nil", "N", {}},
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
    expect(tf.position.x == 2048 and tf.position.y == -100 and tf.position.z == 10 and tf.heading == 1, "Transform set clamps")
    raises('"Transform": unknown field "pos"', function() kast.set(g, "Transform", {heading = 2, pos = {}}) end)
    raises('"Transform": field names must be strings, got 1', function() kast.set(g, "Transform", {5}) end)
    raises('"Transform": "position" must be a table {x=, y=, z=} of finite numbers', function() kast.set(g, "Transform", {heading = 2, position = {x = 1}}) end)
    raises('"Transform": "heading" must be a finite number', function() kast.set(g, "Transform", {position = {x = 1, y = 1, z = 1}, heading = 1 / 0}) end)
    tf = kast.get(g, "Transform")
    expect(tf.position.x == 2048 and tf.heading == 1, "every failed Transform set left it unchanged")
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
void checkNpcSample() {
    LogCapture capture;
    const fs::path root = KAST_WORLD_LUA_SAMPLE;
    const auto defs = lua::load_component_defs(root, world::kReservedComponentNames);
    check(defs.errors.empty(), "sample definitions load");
    TestWorld w(defs.defs);
    const auto errors = w.world.load_scripts(root, {"zone_cycle"});
    check(errors.empty(), "sample systems load: " + (errors.empty() ? "" : errors[0]));
    auto& reg = w.world.registry();
    check(reg.view<world::Npc>().size() == 1, "the sample creates one NPC");
    if (reg.view<world::Npc>().size() != 1) return;
    const auto g = reg.view<world::Npc>().front();
    check(reg.get<world::Npc>(g).name == "Guard" && reg.get<world::Npc>(g).templateName == "Guard", "Guard, template Guard");
    w.spawn(1, 141, "Alice", 1024, 1024);
    std::set<std::string> states;
    float farthest = 0, maxStep = 0;
    proto::Vec3 last = reg.get<world::Transform>(g).pos;
    for (int i = 1; i <= 400; ++i) {
        w.world.tick(T0 + std::chrono::milliseconds(50 * i));
        const proto::Vec3 p = reg.get<world::Transform>(g).pos;
        farthest = std::max(farthest, std::hypot(p.x - 1024, p.z - 1024));
        maxStep = std::max(maxStep, std::hypot(p.x - last.x, p.z - last.z));
        last = p;
        states.insert(w.world.fsm().state(lua::EntityId{wire(g)}));
    }
    check(states == std::set<std::string>{"walking", "waiting"}, "the patrol machine walks and waits");
    check(farthest > 10 && farthest < 15, "it patrols around the spawn point (farthest " + std::to_string(farthest) + " m)");
    check(maxStep <= 3 * 0.05f + 1e-3f, "at 3 m/s (largest step " + std::to_string(maxStep) + " m)");
    const auto spawns = w.sent.of<proto::Spawn>(1);
    check(std::ranges::any_of(spawns, [](const proto::Spawn& s) { return s.kind == proto::SpawnKind::npc && s.templateName == "Guard"; }),
          "the player at the spawn point gets the Guard's Spawn");
    size_t moves = 0;
    for (const auto& m : w.sent.of<proto::EntityMove>(1)) moves += m.entityId == wire(g);
    check(moves > 200, "and its moves (" + std::to_string(moves) + ")");
    for (const auto& s : w.world.script_status()) check(s.ok, "sample system enabled: " + s.path + " " + s.error);
    check(!said("FAIL") && log_index("\"level\":\"error\"") == SIZE_MAX, "no Lua errors");
}

int lua_checks(int& checks) {
    checkCodec();
    checkDefinitionParsing();
    checkDefinitionErrors();
    checkNameRules();
    checkSandbox();
    checkKastApi();
    checkHooks();
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
    checkNpcSample();
    checks += g_checks;
    return g_failures;
}
