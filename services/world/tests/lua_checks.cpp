// Assert-style checks for the Lua layer: component definitions (every type, option and error), reserved and duplicate
// names, the sandbox, kast.* semantics, hooks, error handling, persistence + drift, component replication routing, the
// event bus (kast.publish/subscribe/unsubscribe) and client events (kast.sendTo/sendNearby/broadcast).
// Lua files are written to a temp folder; the World runs with captured Send/Save, so there is no network or database.
#include "core/log.h"
#include "lua/components.h"
#include "world/components.h"
#include "world/world.h"

#include <codec.h>

#include <cstdio>
#include <format>
#include <fstream>
#include <map>
#include <random>
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
    for (const char* reserved : {"Character", "Transform", "Dirty", "SessionLink", "Visible"}) {
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
    explicit TestWorld(lua::Defs defs)
        : world({}, [this](uint32_t sid, const proto::Message& m) { sent.bySession[sid].push_back(m); },
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

} // namespace

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
    checks += g_checks;
    return g_failures;
}
