// The Zone tab's Areas, Spawns, Objects and Bake sections and their Scene view tools:
// drawing rect and polygon areas, placing spawns, adding path points, placing zone objects and scattering
// visuals, dragging area vertices, spawns, path points and objects with no tool, their overlays (zone objects' shapes as
// ground outlines), and Bake Navmesh (kast_mapbake). The edits are zone/zone_edit.h.
#include "zone_panel.h"

#include "editor_settings.h"
#include "ui_common.h"

#include <log.h>
#include <scene_io.h> // jsonNumber

#include <bx/math.h>
#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>
#include <utility>

namespace {

namespace km = kast::map;
namespace fs = std::filesystem;
using zone::Json;
using zone::Point;
using zone::Vec3;

constexpr ImU32 kAreaColor = IM_COL32(255, 170, 0, 200);
constexpr ImU32 kAreaSelected = IM_COL32(255, 204, 0, 255);
constexpr ImU32 kPortalColor = IM_COL32(110, 200, 255, 230);
constexpr ImU32 kSpawnMarker = IM_COL32(255, 110, 70, 255);
constexpr ImU32 kPathColor = IM_COL32(120, 255, 140, 220);
constexpr ImU32 kDraftColor = IM_COL32(255, 255, 255, 230);
constexpr ImU32 kVisualColor = IM_COL32(120, 220, 90, 200);
constexpr ImU32 kObstacleColor = IM_COL32(255, 153, 0, 230); // the palette's darken
constexpr ImU32 kInteractableColor = IM_COL32(110, 200, 255, 240);
constexpr ImU32 kScatterColor = IM_COL32(255, 204, 0, 255);
constexpr ImU32 kEraseColor = IM_COL32(255, 80, 60, 255);
constexpr float kHandlePx = 10.0f;     // grab distance of a handle, in window points
constexpr float kHeadingLength = 8.0f; // metres
constexpr const char* kTypeNames[] = {"Visual", "Obstacle", "Interactable"}; // in km::ObjectType order

bool near(ImVec2 a, ImVec2 b) { return (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) <= kHandlePx * kHandlePx; }

// j's key, or null (no object, no key): a reference, for what runs every frame over every tile's file.
const Json& member(const Json& j, const char* key)
{
    static const Json none;
    if (!j.is_object())
        return none;
    const auto it = j.find(key);
    return it != j.end() ? *it : none;
}

std::string str(const Json& j, const char* key)
{
    const Json v = j.value(key, Json());
    return v.is_string() ? v.get<std::string>() : std::string();
}

float number(const Json& j, const char* key, float fallback)
{
    const Json v = j.value(key, Json());
    return v.is_number() ? v.get<float>() : fallback;
}

Vec3 vec3(const Json& v)
{
    if (!v.is_array() || v.size() != 3)
        return {};
    const auto f = [](const Json& n) { return n.is_number() ? n.get<float>() : 0.0f; };
    return {f(v[0]), f(v[1]), f(v[2])};
}

// The window point of ground point (x, z); nullopt behind the camera.
std::optional<ImVec2> screen(const GroundDraw& g, float x, float z) { return g.project(g.ground(x, z)); }

// Even-odd point in polygon (as kast::map's).
bool inside(const std::vector<Point>& poly, float x, float z)
{
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        if ((poly[i][1] > z) != (poly[j][1] > z) &&
            x < (poly[j][0] - poly[i][0]) * (z - poly[i][1]) / (poly[j][1] - poly[i][1]) + poly[i][0])
            in = !in;
    return in;
}

// One above the highest priority, so a new area never overlaps another at equal priority.
int nextPriority(const zone::ZoneState& state)
{
    int top = -1;
    if (state.areas.is_object())
        for (const Json& a : state.areas.value("areas", Json::array()))
            top = std::max(top, a.value("priority", 0));
    return top + 1;
}

size_t areaCount(const zone::ZoneState& state)
{
    return state.areas.is_object() ? state.areas.value("areas", Json::array()).size() : 0;
}

std::string lower(std::string s)
{
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// One undo step per edit of the last item: from its activation to its deactivation.
void undoScope(zone::ZoneDoc& z)
{
    if (ImGui::IsItemActivated())
        z.begin();
    if (ImGui::IsItemDeactivated())
        z.commit();
}

// An object's shape as the parser reads it, for the overlay; nullopt for none (or a bad one: validate() reports it).
std::optional<km::Shape> shapeOf(const Json& object)
{
    const Json& s = object.value("shape", Json());
    km::Shape shape;
    if (s.contains("box") && s["box"].is_object()) {
        const Vec3 h = vec3(s["box"].value("halfExtents", Json()));
        shape.halfExtents = {h[0], h[1], h[2]};
    } else if (s.contains("cylinder") && s["cylinder"].is_object()) {
        shape.kind = km::Shape::Kind::Cylinder;
        shape.radius = number(s["cylinder"], "radius", 0);
        shape.height = number(s["cylinder"], "height", 0);
    } else {
        return std::nullopt;
    }
    return shape;
}

std::string stem(const std::string& prefab) { return fs::path(prefab).stem().string(); }

ImU32 typeColor(km::ObjectType type)
{
    return type == km::ObjectType::Visual ? kVisualColor : type == km::ObjectType::Obstacle ? kObstacleColor
                                                                                             : kInteractableColor;
}

bool idUsed(const zone::ZoneState& state, uint32_t id, zone::ObjectRef except)
{
    for (const zone::ObjectRef& ref : zone::all_objects(state))
        if (!(ref == except) && zone::object_json(state, ref).value("id", Json()) == id)
            return true;
    return false;
}

std::string utf8(const fs::path& p)
{
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string objectName(const zone::ZoneState& state, zone::ObjectRef ref)
{
    return std::format("{} (tile {}, #{})", stem(str(zone::object_json(state, ref), "prefab")), zone::tile_name(ref.tile),
                       ref.index);
}

} // namespace

void ZonePanel::checkSelection(const zone::ZoneDoc* z)
{
    if (!z || z->folder != selectionFolder_) { // another map (or none): nothing of the old one is selected
        selectionFolder_ = z ? z->folder : std::string();
        area_.reset();
        spawn_.clear();
        object_.reset();
        polygon_.clear();
        rect_.reset();
        dataFor_ = 0;
        return;
    }
    if (area_ && *area_ >= areaCount(z->state))
        area_.reset();
    if (object_) { // undone or deleted
        const std::vector<zone::ObjectRef> refs = zone::all_objects(z->state);
        if (std::ranges::find(refs, *object_) == refs.end())
            object_.reset();
    }
}

void ZonePanel::defaultPrefabs(const ZoneSession& zone)
{
    if (zone.prefabs().empty())
        return;
    if (placePrefab_.empty())
        placePrefab_ = zone.prefabs().front();
    // The first prefab ticked once, so the Scatter brush works out of the box (untick it for others).
    if (!std::exchange(scatterSeeded_, true) && scatter_.prefabs.empty())
        scatter_.prefabs.push_back(zone.prefabs().front());
}

void ZonePanel::select(const ZoneSession& zone, const std::string& id)
{
    const zone::ZoneDoc* z = zone.zone();
    if (!z)
        return;
    checkSelection(z);
    if (const auto area = zone::find_area(z->state, id))
        area_ = area;
    if (zone::find_spawn(z->state, id))
        spawn_ = id;
    if (!id.starts_with("object:"))
        return;
    int v[3] = {};
    const char* p = id.data() + 7;
    const char* end = id.data() + id.size();
    for (int i = 0; i < 3; ++i) {
        const auto [next, ec] = std::from_chars(p, end, v[i]);
        if (ec != std::errc() || (i < 2 && (next == end || *next != ',')))
            return;
        p = next + 1;
    }
    const zone::ObjectRef ref{{v[0], v[1]}, size_t(std::max(v[2], 0))};
    const std::vector<zone::ObjectRef> refs = zone::all_objects(z->state);
    if (std::ranges::find(refs, ref) != refs.end())
        object_ = ref;
}

// --- Scene view ---

bool ZonePanel::objectsInput(const ZoneSession& zone, zone::ZoneDoc& z, const km::MapInfo& info, const GroundDraw& g,
                             bool click)
{
    const ImGuiIO& io = ImGui::GetIO();
    const Point at = hover_ ? Point{(*hover_)[0], (*hover_)[2]} : Point{};
    switch (tool_) {
    case Tool::AreaRect:
        if (click && hover_)
            rect_ = {{at, at}};
        if (rect_ && hover_)
            (*rect_)[1] = at;
        if (rect_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const auto [a, b] = *rect_;
            rect_.reset();
            if (std::fabs(a[0] - b[0]) >= 1 && std::fabs(a[1] - b[1]) >= 1) {
                z.edit([&](zone::ZoneState& s) {
                    area_ = zone::add_rect_area(s, zone::unique_area_id(s, "area"), a, b, nextPriority(s));
                });
                kast::log::info(std::format("Zone: area {} added ({:.0f} x {:.0f} m)",
                                            str(zone::areas_list(z.state)[*area_], "id"), std::fabs(a[0] - b[0]),
                                            std::fabs(a[1] - b[1])));
            }
        }
        break;
    case Tool::AreaPolygon: {
        bool finish = !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Enter) && polygon_.size() >= 3;
        if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Backspace) && !polygon_.empty())
            polygon_.pop_back();
        if (click && hover_) {
            const auto first = polygon_.empty() ? std::nullopt : screen(g, polygon_[0][0], polygon_[0][1]);
            if (polygon_.size() >= 3 && first && near(*first, io.MousePos))
                finish = true;
            else
                polygon_.push_back(at);
        }
        if (finish) {
            z.edit([&](zone::ZoneState& s) {
                area_ = zone::add_polygon_area(s, zone::unique_area_id(s, "area"), polygon_, nextPriority(s));
            });
            kast::log::info(std::format("Zone: area {} added ({} points)", str(zone::areas_list(z.state)[*area_], "id"),
                                        polygon_.size()));
            polygon_.clear();
        }
        break;
    }
    case Tool::PlaceSpawn:
        if (click && hover_) {
            const std::string npc = zone.npcs().empty() ? std::string("npc") : zone.npcs().front();
            z.edit([&](zone::ZoneState& s) {
                spawn_ = zone::unique_spawn_id(s, lower(npc) + "_1");
                zone::add_spawn(s, spawn_, npc, *hover_, 0, 60);
            });
            kast::log::info(std::format("Zone: spawn {} ({}) placed at {:.1f}, {:.1f}, {:.1f}", spawn_, npc,
                                        (*hover_)[0], (*hover_)[1], (*hover_)[2]));
        }
        break;
    case Tool::PathPoints:
        if (const auto ref = zone::find_spawn(z.state, spawn_); ref && click && hover_) {
            z.edit([&](zone::ZoneState& s) { zone::add_path_point(s, *ref, *hover_); });
            kast::log::info(std::format("Zone: path point added to {} at {:.1f}, {:.1f}", spawn_, at[0], at[1]));
        }
        break;
    case Tool::PlaceObject:
        if (click && hover_ && !placePrefab_.empty()) {
            z.edit([&](zone::ZoneState& s) { object_ = zone::add_object(s, placeType_, placePrefab_, *hover_, 0, 1); });
            kast::log::info(std::format("Zone: {} {} placed at {:.1f}, {:.1f}", kTypeNames[int(placeType_)],
                                        objectName(z.state, *object_), at[0], at[1]));
        }
        break;
    case Tool::Scatter:
        scatterInput(z, info, click);
        break;
    case Tool::None:
        if (click && !pickHandle(z, g))
            return false; // not on a handle: the Scene view picks an entity
        if (drag_.kind != Drag::Kind::None && hover_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const auto now = zone::find_spawn(z.state, spawn_);
            switch (drag_.kind) {
            case Drag::Kind::AreaVertex:
                if (area_ && *area_ < areaCount(z.state))
                    zone::move_area_vertex(zone::areas_list(z.state)[*area_], drag_.index, at);
                break;
            case Drag::Kind::Spawn:
                if (now)
                    zone::move_spawn(z.state, *now, *hover_);
                break;
            case Drag::Kind::PathPoint:
                if (Json* route = now ? zone::spawn_path(z.state, *now) : nullptr)
                    (*route)["points"][drag_.index] = zone::vec3_json(*hover_);
                break;
            case Drag::Kind::Object:
                if (object_)
                    object_ = zone::move_object(z.state, *object_, *hover_);
                break;
            case Drag::Kind::None: break;
            }
        }
        return drag_.kind != Drag::Kind::None;
    default: break;
    }
    return true;
}

bool ZonePanel::pickHandle(zone::ZoneDoc& z, const GroundDraw& g)
{
    if (!tabFront_) // the zone's handles are live only while the Zone tab shows
        return false;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    drag_ = {};
    // Handles first (the selection's vertices and path points, then any spawn), then zone objects.
    if (area_ && *area_ < areaCount(z.state)) {
        const auto points = zone::area_points(z.state.areas["areas"][*area_]);
        for (size_t i = 0; i < points.size() && drag_.kind == Drag::Kind::None; ++i)
            if (const auto p = screen(g, points[i][0], points[i][1]); p && near(*p, mouse)) {
                drag_ = {Drag::Kind::AreaVertex, i};
                dragLog_ = std::format("area {} vertex {}", str(z.state.areas["areas"][*area_], "id"), i);
            }
    }
    if (const auto ref = zone::find_spawn(z.state, spawn_); ref && drag_.kind == Drag::Kind::None)
        if (const Json* route = zone::spawn_path(z.state, *ref)) {
            const Json& points = route->value("points", Json::array());
            for (size_t i = 0; i < points.size() && drag_.kind == Drag::Kind::None; ++i) {
                const Vec3 p = vec3(points[i]);
                if (const auto s = screen(g, p[0], p[2]); s && near(*s, mouse)) {
                    drag_ = {Drag::Kind::PathPoint, i};
                    dragLog_ = std::format("path point {} of {}", i, spawn_);
                }
            }
        }
    for (const zone::SpawnRef& r : zone::all_spawns(z.state)) {
        if (drag_.kind != Drag::Kind::None)
            break;
        const Json& s = zone::spawn_json(z.state, r);
        const Vec3 p = vec3(s.value("pos", Json()));
        if (const auto sp = screen(g, p[0], p[2]); sp && near(*sp, mouse)) {
            spawn_ = str(s, "id");
            drag_ = {Drag::Kind::Spawn, 0};
            dragLog_ = "spawn " + spawn_;
        }
    }
    float nearest = kHandlePx * kHandlePx; // zone objects: the marker nearest the mouse
    if (drag_.kind == Drag::Kind::None)
        for (const zone::ObjectRef& r : zone::all_objects(z.state)) {
            const Vec3 p = vec3(zone::object_json(z.state, r).value("pos", Json()));
            const auto sp = screen(g, p[0], p[2]);
            if (!sp)
                continue;
            const float d = (sp->x - mouse.x) * (sp->x - mouse.x) + (sp->y - mouse.y) * (sp->y - mouse.y);
            if (d > nearest)
                continue;
            nearest = d;
            object_ = r;
            drag_ = {Drag::Kind::Object, 0};
            dragLog_ = "object " + objectName(z.state, r);
        }
    if (drag_.kind != Drag::Kind::None) {
        z.begin(); // the whole drag is one undo step (endStroke commits it)
        return true;
    }
    if (hover_) { // the highest-priority area under the click, as the server picks
        object_.reset();
        area_.reset();
        int best = 0;
        for (size_t i = 0; i < areaCount(z.state); ++i) {
            const Json& a = z.state.areas["areas"][i];
            if (inside(zone::area_points(a), (*hover_)[0], (*hover_)[2]) && (!area_ || a.value("priority", 0) > best)) {
                area_ = i;
                best = a.value("priority", 0);
            }
        }
    }
    return false;
}

void ZonePanel::scatterInput(zone::ZoneDoc& z, const km::MapInfo& info, bool click)
{
    if (click && hover_ && !stroking_ && (scatterErase_ || !scatter_.prefabs.empty())) {
        z.begin(); // the whole stroke is one undo step (endStroke commits it)
        stroking_ = scattering_ = true;
        strokeCount_ = 0;
        lastDab_.reset();
    }
    if (!scattering_ || !hover_ || !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        return;
    const Point at{(*hover_)[0], (*hover_)[2]};
    if (lastDab_ && std::hypot(at[0] - (*lastDab_)[0], at[1] - (*lastDab_)[1]) < scatter_.radius)
        return;
    lastDab_ = at;
    strokeCount_ += scatterErase_ ? zone::erase_visuals(z.state, at[0], at[1], scatter_.radius)
                                  : zone::scatter(info, z.state, scatter_, scatterRng_, at[0], at[1]);
}

void ZonePanel::drawObjectOverlays(const GroundDraw& g) const
{
    const zone::ZoneState& state = g.state;
    ImDrawList* draw = g.draw;
    const Json& list = state.areas.is_object() ? state.areas.value("areas", Json::array()) : Json::array();
    for (size_t i = 0; i < list.size(); ++i) {
        const auto points = zone::area_points(list[i]);
        if (points.size() < 3)
            continue;
        const bool selected = area_ == i;
        const ImU32 color = selected ? kAreaSelected : list[i].contains("portal") ? kPortalColor : kAreaColor;
        for (size_t k = 0; k < points.size(); ++k)
            g.line(points[k], points[(k + 1) % points.size()], color, selected ? 3.0f : 1.5f);
        Point c{};
        for (const Point& p : points)
            c = {c[0] + p[0] / float(points.size()), c[1] + p[1] / float(points.size())};
        if (const auto s = screen(g, c[0], c[1])) {
            const std::string label = std::format("{} (p{}){}", str(list[i], "id"), list[i].value("priority", 0),
                                                  list[i].contains("portal") ? " portal" : "");
            draw->AddText(*s, color, label.c_str());
        }
        if (selected)
            for (const Point& p : points)
                if (const auto s = screen(g, p[0], p[1]))
                    draw->AddRectFilled({s->x - 4, s->y - 4}, {s->x + 4, s->y + 4}, kAreaSelected);
    }
    for (const zone::ObjectRef& ref : zone::all_objects(state)) {
        const Json& o = zone::object_json(state, ref);
        const km::ObjectType type = zone::object_type(o);
        const Vec3 p = vec3(o.value("pos", Json()));
        const bool selected = object_ == ref;
        const ImU32 color = selected ? kAreaSelected : typeColor(type);
        if (const auto shape = type == km::ObjectType::Visual ? std::nullopt : shapeOf(o)) {
            const auto outline = km::footprint(*shape, p, number(o, "yaw", 0));
            for (size_t k = 0; k < outline.size(); ++k)
                g.line(outline[k], outline[(k + 1) % outline.size()], color, selected ? 3.0f : 1.5f);
        }
        const auto at = screen(g, p[0], p[2]);
        if (!at)
            continue;
        draw->AddCircleFilled(*at, selected ? 6.0f : type == km::ObjectType::Visual ? 2.5f : 4.0f, color);
        if (type == km::ObjectType::Interactable) {
            const std::string label = std::format("#{} {}", o.value("id", 0u), o.value("name", stem(str(o, "prefab"))));
            draw->AddText({at->x + 8, at->y - 8}, color, label.c_str());
        } else if (selected) {
            draw->AddText({at->x + 8, at->y - 8}, color, stem(str(o, "prefab")).c_str());
        }
    }
    for (const auto& [tile, file] : state.spawns) {
        for (const auto& [name, route] : member(file, "paths").items()) { // a reference: no copy per tile a frame
            const Json& points = route.value("points", Json::array());
            const size_t closing = route.value("loop", true) && points.size() > 2 ? 1 : 0;
            for (size_t i = 0; i + 1 < points.size() + closing; ++i) {
                const Vec3 a = vec3(points[i]), b = vec3(points[(i + 1) % points.size()]);
                g.line({a[0], a[2]}, {b[0], b[2]}, kPathColor, 1.5f);
            }
            for (const Json& p : points)
                if (const auto s = screen(g, vec3(p)[0], vec3(p)[2]))
                    draw->AddCircleFilled(*s, 3.5f, kPathColor);
        }
        for (const Json& s : member(file, "spawns")) {
            const Vec3 p = vec3(member(s, "pos"));
            const auto at = screen(g, p[0], p[2]);
            if (!at)
                continue;
            const bool selected = str(s, "id") == spawn_;
            const ImU32 color = selected ? kAreaSelected : kSpawnMarker;
            draw->AddCircleFilled(*at, selected ? 7.0f : 5.0f, color);
            const float h = number(s, "heading", 0);
            if (const auto tip = screen(g, p[0] + kHeadingLength * std::sin(h), p[2] + kHeadingLength * std::cos(h)))
                draw->AddLine(*at, *tip, color, 2.5f);
            const std::string label = std::format("{} ({})", str(s, "id"), str(s, "npc"));
            draw->AddText({at->x + 8, at->y - 8}, color, label.c_str());
        }
    }
    // The active tool's draft: the area being drawn, the scatter ring.
    const std::optional<Point> at = hover_ ? std::optional(Point{(*hover_)[0], (*hover_)[2]}) : std::nullopt;
    if (rect_) {
        const auto [a, b] = *rect_;
        const Point c[4] = {a, {b[0], a[1]}, b, {a[0], b[1]}};
        for (int i = 0; i < 4; ++i)
            g.line(c[i], c[(i + 1) % 4], kDraftColor, 2.0f);
    }
    for (size_t i = 0; i + 1 < polygon_.size(); ++i)
        g.line(polygon_[i], polygon_[i + 1], kDraftColor, 2.0f);
    if (!polygon_.empty() && at)
        g.line(polygon_.back(), *at, kDraftColor, 1.0f);
    if (tool_ == Tool::Scatter && at)
        g.circle((*at)[0], (*at)[1], scatter_.radius, scatterErase_ ? kEraseColor : kScatterColor);
}

// --- Zone tab sections ---

void ZonePanel::drawAreas(ZoneSession& zone)
{
    if (!header("Areas", "Areas"))
        return;
    zone::ZoneDoc* z = zone.zone();
    if (!z) {
        ImGui::TextDisabled("No zone open");
        return;
    }
    if (tool_ == Tool::AreaRect)
        ImGui::TextDisabled("Drag on the terrain. Esc: stop");
    else if (tool_ == Tool::AreaPolygon)
        ImGui::TextDisabled("Click points; click the first or press Enter to close. Backspace: undo a point. Esc: cancel");
    else
        ImGui::TextDisabled("Area Rect / Area Polygon at the top draw one. No tool: click an area in the Scene view to "
                            "select it; drag its corners to reshape");

    const size_t count = areaCount(z->state);
    if (ImGui::BeginChild("##areaList", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 6), ImGuiChildFlags_Borders)) {
        for (size_t i = 0; i < count; ++i) {
            const Json& a = z->state.areas["areas"][i];
            const std::string label = std::format("{}  ({}, priority {}){}##{}", str(a, "id"), str(a, "name"),
                                                  a.value("priority", 0), a.contains("portal") ? "  portal" : "", i);
            if (ImGui::Selectable(label.c_str(), area_ == i))
                area_ = i;
        }
    }
    ImGui::EndChild();
    if (!area_)
        return;

    const size_t index = *area_;
    Json& a = zone::areas_list(z->state)[index];
    std::string id = str(a, "id");
    if (ImGui::InputText("Id##area", &id))
        zone::rename_area(z->state, index, id);
    undoScope(*z);
    std::string name = str(zone::areas_list(z->state)[index], "name");
    if (ImGui::InputText("Name##area", &name))
        zone::areas_list(z->state)[index]["name"] = name;
    undoScope(*z);
    int priority = zone::areas_list(z->state)[index].value("priority", 0);
    if (ImGui::InputInt("Priority", &priority))
        zone::areas_list(z->state)[index]["priority"] = priority;
    undoScope(*z);

    const std::string parent = str(zone::areas_list(z->state)[index], "parent");
    if (ImGui::BeginCombo("Parent", parent.empty() ? "(none)" : parent.c_str())) {
        if (ImGui::Selectable("(none)", parent.empty()))
            z->edit([&](zone::ZoneState& s) { zone::areas_list(s)[index].erase("parent"); });
        for (size_t i = 0; i < count; ++i) {
            const std::string other = str(z->state.areas["areas"][i], "id");
            if (i != index && ImGui::Selectable(other.c_str(), other == parent))
                z->edit([&](zone::ZoneState& s) { zone::areas_list(s)[index]["parent"] = other; });
        }
        ImGui::EndCombo();
    }

    const std::vector<zone::ProjectMap>& maps = zone.project().maps;
    bool portal = zone::areas_list(z->state)[index].contains("portal");
    if (ImGui::Checkbox("Portal", &portal))
        z->edit([&](zone::ZoneState& s) {
            Json& area = zone::areas_list(s)[index];
            if (!portal)
                area.erase("portal");
            else
                area["portal"] = {{"map", maps.empty() ? 0u : unsigned(maps.front().info.id)},
                                  {"pos", Json::array({128, 0, 128})},
                                  {"heading", 0}};
        });
    if (portal) {
        const Json& p = zone::areas_list(z->state)[index]["portal"];
        const unsigned target = p.value("map", 0u);
        std::string current = std::format("{} (unknown map)", target);
        for (const zone::ProjectMap& m : maps)
            if (m.info.id == target)
                current = std::format("{}  maps/{}", target, m.folder);
        if (ImGui::BeginCombo("Target map", current.c_str())) {
            for (const zone::ProjectMap& m : maps)
                if (ImGui::Selectable(std::format("{}  maps/{}", m.info.id, m.folder).c_str(), m.info.id == target)) {
                    const unsigned mapId = m.info.id; // unsigned: the parser wants an unsigned number
                    z->edit([&](zone::ZoneState& s) { zone::areas_list(s)[index]["portal"]["map"] = mapId; });
                }
            ImGui::EndCombo();
        }
        Vec3 pos = vec3(zone::areas_list(z->state)[index]["portal"].value("pos", Json()));
        if (ImGui::InputFloat3("Arrive at", pos.data(), "%.1f"))
            zone::areas_list(z->state)[index]["portal"]["pos"] = zone::vec3_json(pos);
        undoScope(*z);
        float heading = number(zone::areas_list(z->state)[index]["portal"], "heading", 0) * 180.0f / bx::kPi;
        if (ImGui::SliderFloat("Arrive heading", &heading, -180.0f, 180.0f, "%.0f deg"))
            zone::areas_list(z->state)[index]["portal"]["heading"] = kast::jsonNumber(heading * bx::kPi / 180.0f);
        undoScope(*z);
    }
    // Kept as they are; the tab does not edit them.
    const Json& kept = zone::areas_list(z->state)[index];
    for (const char* key : {"y", "music", "flags"})
        if (kept.contains(key))
            ImGui::TextDisabled("%s: %s (kept)", key, kept[key].dump().c_str());
    if (ImGui::Button("Delete area")) {
        z->edit([&](zone::ZoneState& s) { zone::delete_area(s, index); });
        area_.reset();
    }
}

void ZonePanel::drawSpawns(ZoneSession& zone)
{
    if (!header("Spawns", "Spawns"))
        return;
    zone::ZoneDoc* z = zone.zone();
    std::string err;
    const auto info = z ? z->info(err) : std::nullopt;
    if (!info) {
        ImGui::TextDisabled(z ? "map.json is invalid (see Map)" : "No zone open");
        return;
    }
    if (tool_ == Tool::PlaceSpawn)
        ImGui::TextDisabled("Click the terrain to place a spawn. Esc: stop");
    else if (tool_ == Tool::PathPoints)
        ImGui::TextDisabled(spawn_.empty() ? "Select a spawn first" : "Click the terrain to add points to its path");
    else
        ImGui::TextDisabled("Place Spawn / Path Points at the top. No tool: drag spawns and path points to move them");

    if (ImGui::BeginChild("##spawnList", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 6), ImGuiChildFlags_Borders)) {
        for (const zone::SpawnRef& ref : zone::all_spawns(z->state)) {
            const Json& s = zone::spawn_json(z->state, ref);
            const std::string label = std::format("{}  {}  (tile {})##{}_{}", str(s, "id"), str(s, "npc"),
                                                  zone::tile_name(ref.tile), zone::tile_name(ref.tile), ref.index);
            if (ImGui::Selectable(label.c_str(), str(s, "id") == spawn_))
                spawn_ = str(s, "id");
        }
    }
    ImGui::EndChild();

    const auto ref = zone::find_spawn(z->state, spawn_);
    if (!ref)
        return;
    std::string id = str(zone::spawn_json(z->state, *ref), "id");
    if (ImGui::InputText("Id##spawn", &id)) {
        zone::spawn_json(z->state, *ref)["id"] = id;
        spawn_ = id;
    }
    undoScope(*z);
    const std::string npc = str(zone::spawn_json(z->state, *ref), "npc");
    if (ImGui::BeginCombo("NPC", npc.c_str())) {
        for (const std::string& name : zone.npcs())
            if (ImGui::Selectable(name.c_str(), name == npc))
                z->edit([&](zone::ZoneState& st) { zone::spawn_json(st, *ref)["npc"] = name; });
        ImGui::EndCombo();
    }
    std::string typed = npc;
    if (ImGui::InputText("NPC (any name)", &typed))
        zone::spawn_json(z->state, *ref)["npc"] = typed;
    undoScope(*z);

    const Vec3 pos = vec3(zone::spawn_json(z->state, *ref).value("pos", Json()));
    float xz[2] = {pos[0], pos[2]};
    if (ImGui::InputFloat2("Position x, z##spawn", xz, "%.1f")) {
        const float y = zone::ground_y(*info, z->state, xz[0], xz[1]).value_or(pos[1]);
        zone::move_spawn(z->state, *ref, {xz[0], y, xz[1]});
    }
    undoScope(*z);
    const auto now = zone::find_spawn(z->state, spawn_); // the move may have changed its file
    if (!now)
        return;
    Json& sp = zone::spawn_json(z->state, *now);
    ImGui::Text("Tile %s, y %.2f (on the ground)", zone::tile_name(now->tile).c_str(), vec3(sp.value("pos", Json()))[1]);
    float heading = number(sp, "heading", 0) * 180.0f / bx::kPi;
    if (ImGui::SliderFloat("Heading##spawn", &heading, -180.0f, 180.0f, "%.0f deg"))
        sp["heading"] = kast::jsonNumber(heading * bx::kPi / 180.0f);
    undoScope(*z);
    bool never = number(sp, "respawnSec", -1) < 0;
    float respawn = never ? 60.0f : number(sp, "respawnSec", 60);
    if (ImGui::Checkbox("Never respawns", &never))
        z->edit([&](zone::ZoneState& st) { zone::spawn_json(st, *now)["respawnSec"] = never ? -1.0 : 60.0; });
    if (!never) {
        if (ImGui::InputFloat("Respawn (s)", &respawn, 1.0f, 10.0f, "%.0f"))
            zone::spawn_json(z->state, *now)["respawnSec"] = kast::jsonNumber(std::max(0.0f, respawn));
        undoScope(*z);
    }

    ImGui::SeparatorText("Patrol path");
    const std::string path = str(zone::spawn_json(z->state, *now), "path");
    if (ImGui::BeginCombo("Path", path.empty() ? "(none)" : path.c_str())) {
        if (ImGui::Selectable("(none)", path.empty()))
            z->edit([&](zone::ZoneState& st) { zone::spawn_json(st, *now).erase("path"); });
        const Json paths = z->state.spawns.at(now->tile).value("paths", Json::object()); // items() must outlive the walk
        for (const auto& [name, route] : paths.items())
            if (ImGui::Selectable(name.c_str(), name == path))
                z->edit([&](zone::ZoneState& st) { zone::spawn_json(st, *now)["path"] = name; });
        ImGui::EndCombo();
    }
    if (Json* route = zone::spawn_path(z->state, *now)) {
        float wait = number(*route, "wait", 0);
        if (ImGui::InputFloat("Wait at points (s)", &wait, 0.5f, 1.0f, "%.1f"))
            (*zone::spawn_path(z->state, *now))["wait"] = kast::jsonNumber(std::max(0.0f, wait));
        undoScope(*z);
        bool loop = zone::spawn_path(z->state, *now)->value("loop", true);
        if (ImGui::Checkbox("Loop (off: walk back along it)", &loop))
            z->edit([&](zone::ZoneState& st) { (*zone::spawn_path(st, *now))["loop"] = loop; });
        const size_t points = zone::spawn_path(z->state, *now)->value("points", Json::array()).size();
        ImGui::Text("%zu point(s)", points);
        ImGui::SameLine();
        ImGui::BeginDisabled(points <= 1); // a path needs at least one point
        if (ImGui::SmallButton("Remove last point"))
            z->edit([&](zone::ZoneState& st) { (*zone::spawn_path(st, *now))["points"].erase(points - 1); });
        ImGui::EndDisabled();
        if (ImGui::Button("Delete path"))
            z->edit([&](zone::ZoneState& st) { zone::delete_path(st, now->tile, path); });
    } else if (ImGui::Button("New path from here")) {
        tool_ = Tool::PathPoints;
    }
    if (ImGui::Button("Delete spawn")) {
        z->edit([&](zone::ZoneState& st) { zone::delete_spawn(st, *now); });
        spawn_.clear();
    }
}

void ZonePanel::drawObjects(ZoneSession& zone)
{
    if (!header("Objects", "Objects"))
        return;
    zone::ZoneDoc* z = zone.zone();
    if (!z) {
        ImGui::TextDisabled("No zone open");
        return;
    }
    if (tool_ == Tool::PlaceObject)
        ImGui::TextDisabled("Click the terrain to place one. Esc: stop");
    else if (tool_ != Tool::Scatter)
        ImGui::TextDisabled("Place Object / Scatter at the top. No tool: click an object's marker in the Scene view to "
                            "select it; drag it to move it");
    if (ImGui::BeginCombo("Prefab", placePrefab_.empty() ? "(no .prefab in the project)" : placePrefab_.c_str())) {
        for (const std::string& prefab : zone.prefabs())
            if (ImGui::Selectable(prefab.c_str(), prefab == placePrefab_))
                placePrefab_ = prefab;
        ImGui::EndCombo();
    }
    int type = int(placeType_);
    if (ImGui::Combo("Type", &type, kTypeNames, IM_ARRAYSIZE(kTypeNames)))
        placeType_ = km::ObjectType(type);
    drawScatter(zone);

    const std::vector<zone::ObjectRef> refs = zone::all_objects(z->state);
    ImGui::Text("%zu object(s)", refs.size());
    if (ImGui::BeginChild("##objectList", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 6), ImGuiChildFlags_Borders)) {
        ImGuiListClipper clipper; // scattered tiles hold thousands
        clipper.Begin(int(refs.size()));
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const Json& o = zone::object_json(z->state, refs[i]);
                const std::string id = o.contains("id") ? std::format(" #{}", o.value("id", 0u)) : "";
                const std::string label =
                    std::format("{} {}{}  (tile {})##{}_{}", str(o, "type"), stem(str(o, "prefab")), id,
                                zone::tile_name(refs[i].tile), zone::tile_name(refs[i].tile), refs[i].index);
                if (ImGui::Selectable(label.c_str(), object_ == refs[i]))
                    object_ = refs[i];
            }
    }
    ImGui::EndChild();
    if (object_)
        drawObjectFields(zone);
}

void ZonePanel::drawScatter(const ZoneSession& zone)
{
    if (tool_ != Tool::Scatter)
        return;
    ImGui::SeparatorText("Scatter brush (visuals)");
    ImGui::Checkbox("Erase visuals", &scatterErase_);
    ImGui::TextDisabled(scatterErase_ ? "Drag on the terrain to remove visuals in the circle. Esc: stop"
                                      : "Drag on the terrain: a dab every radius moved, one undo step per stroke. Esc: stop");
    ImGui::SliderFloat("Radius (m)", &scatter_.radius, 0.5f, 64.0f, "%.1f");
    if (scatterErase_)
        return;
    ImGui::SliderInt("Per dab", &scatter_.count, 1, 100);
    ImGui::Checkbox("Random yaw", &scatter_.randomYaw);
    ImGui::DragFloatRange2("Scale##scatter", &scatter_.scaleMin, &scatter_.scaleMax, 0.01f, 0.05f, 10.0f, "min %.2f",
                           "max %.2f");
    if (ImGui::InputInt("Seed", &scatterSeed_))
        scatterRng_ = zone::ScatterRng(uint32_t(scatterSeed_));
    ImGui::SameLine();
    if (ImGui::SmallButton("Restart"))
        scatterRng_ = zone::ScatterRng(uint32_t(scatterSeed_)); // replay from the seed
    ImGui::TextUnformatted("Prefabs (one picked at random per object):");
    if (ImGui::BeginChild("##scatterPrefabs", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 5),
                          ImGuiChildFlags_Borders)) {
        for (const std::string& prefab : zone.prefabs()) {
            const auto it = std::ranges::find(scatter_.prefabs, prefab);
            bool on = it != scatter_.prefabs.end();
            if (ImGui::Checkbox(prefab.c_str(), &on)) {
                if (on)
                    scatter_.prefabs.push_back(prefab);
                else
                    scatter_.prefabs.erase(it);
            }
        }
    }
    ImGui::EndChild();
    if (scatter_.prefabs.empty())
        ImGui::TextColored(kErrorColor, "Tick at least one prefab");
}

void ZonePanel::drawObjectFields(ZoneSession& zone)
{
    zone::ZoneDoc& z = *zone.zone();
    std::string err;
    const auto info = z.info(err);
    if (!info)
        return;
    const zone::ObjectRef ref = *object_;
    ImGui::SeparatorText("Selected object");
    int t = int(zone::object_type(zone::object_json(z.state, ref)));
    if (ImGui::Combo("Object type", &t, kTypeNames, IM_ARRAYSIZE(kTypeNames)))
        z.edit([&](zone::ZoneState& s) { zone::set_object_type(s, ref, km::ObjectType(t)); });
    const km::ObjectType type = zone::object_type(zone::object_json(z.state, ref)); // after a change of it
    const std::string prefab = str(zone::object_json(z.state, ref), "prefab");
    if (ImGui::BeginCombo("Object prefab", prefab.c_str())) {
        for (const std::string& p : zone.prefabs())
            if (ImGui::Selectable(p.c_str(), p == prefab))
                z.edit([&](zone::ZoneState& s) { zone::object_json(s, ref)["prefab"] = p; });
        ImGui::EndCombo();
    }
    const auto changed = [&] { // after a field edit: the server entry follows
        if (type == km::ObjectType::Interactable)
            zone::sync_interactable(z.state, *object_);
    };
    const Vec3 pos = vec3(zone::object_json(z.state, ref).value("pos", Json()));
    float xz[2] = {pos[0], pos[2]};
    if (ImGui::InputFloat2("Position x, z##object", xz, "%.2f")) {
        const float y = zone::ground_y(*info, z.state, xz[0], xz[1]).value_or(pos[1]);
        object_ = zone::move_object(z.state, ref, {xz[0], y, xz[1]});
    }
    undoScope(z);
    const zone::ObjectRef now = *object_; // the move may have changed its file
    Json& o = zone::object_json(z.state, now);
    ImGui::Text("Tile %s, y %.2f (on the ground)", zone::tile_name(now.tile).c_str(), vec3(o.value("pos", Json()))[1]);
    float yaw = number(o, "yaw", 0) * 180.0f / bx::kPi;
    if (ImGui::SliderFloat("Yaw", &yaw, -180.0f, 180.0f, "%.0f deg")) {
        zone::object_json(z.state, now)["yaw"] = kast::jsonNumber(yaw * bx::kPi / 180.0f);
        changed();
    }
    undoScope(z);
    float scale = number(zone::object_json(z.state, now), "scale", 1);
    if (ImGui::InputFloat("Scale", &scale, 0.1f, 0.5f, "%.2f"))
        zone::object_json(z.state, now)["scale"] = kast::jsonNumber(std::max(0.01f, scale));
    undoScope(z);

    if (type != km::ObjectType::Visual) {
        const auto shape = shapeOf(zone::object_json(z.state, now));
        int kind = shape && shape->kind == km::Shape::Kind::Cylinder ? 1 : 0;
        const char* kinds[] = {"Box", "Cylinder"};
        if (ImGui::Combo("Shape", &kind, kinds, 2))
            z.edit([&](zone::ZoneState& s) {
                zone::object_json(s, now)["shape"] =
                    kind == 0 ? zone::default_shape() : Json{{"cylinder", {{"radius", 0.5}, {"height", 1}}}};
                if (type == km::ObjectType::Interactable)
                    zone::sync_interactable(s, now);
            });
        if (shape && shape->kind == km::Shape::Kind::Box) {
            float h[3] = {shape->halfExtents[0], shape->halfExtents[1], shape->halfExtents[2]};
            if (ImGui::InputFloat3("Half extents", h, "%.2f")) {
                zone::object_json(z.state, now)["shape"]["box"]["halfExtents"] =
                    zone::vec3_json({std::max(0.01f, h[0]), std::max(0.01f, h[1]), std::max(0.01f, h[2])});
                changed();
            }
            undoScope(z);
        } else if (shape) {
            float rh[2] = {shape->radius, shape->height};
            if (ImGui::InputFloat2("Radius, height", rh, "%.2f")) {
                Json& c = zone::object_json(z.state, now)["shape"]["cylinder"];
                c["radius"] = kast::jsonNumber(std::max(0.01f, rh[0]));
                c["height"] = kast::jsonNumber(std::max(0.01f, rh[1]));
                changed();
            }
            undoScope(z);
        }
    }

    if (type == km::ObjectType::Interactable) {
        ImGui::SeparatorText("Interactable");
        const uint32_t id = zone::object_json(z.state, now).value("id", 0u);
        int typed = int(id);
        if (ImGui::InputInt("Id##interactable", &typed) && typed >= 1 && !idUsed(z.state, uint32_t(typed), now))
            zone::set_interactable_id(z.state, now, uint32_t(typed));
        undoScope(z);
        std::string name = str(zone::object_json(z.state, now), "name");
        if (ImGui::InputText("Name##interactable", &name)) {
            if (name.empty())
                zone::object_json(z.state, now).erase("name");
            else
                zone::object_json(z.state, now)["name"] = name;
            changed();
        }
        undoScope(z);
        const uint32_t current = zone::object_json(z.state, now).value("id", 0u);
        if (!zone::interactable_json(z.state, now.tile, current))
            zone::sync_interactable(z.state, now); // a hand-edited file
        ImGui::TextDisabled("Server only (.server/interactables/%s.json):", zone::tile_name(now.tile).c_str());
        float range = number(*zone::interactable_json(z.state, now.tile, current), "range", 3);
        if (ImGui::InputFloat("Range (m)", &range, 0.5f, 1.0f, "%.1f"))
            (*zone::interactable_json(z.state, now.tile, current))["range"] = kast::jsonNumber(std::max(0.1f, range));
        undoScope(z);
        std::string event = str(*zone::interactable_json(z.state, now.tile, current), "event");
        if (ImGui::InputText("Event", &event))
            (*zone::interactable_json(z.state, now.tile, current))["event"] = event;
        undoScope(z);
        if (event.empty())
            ImGui::TextColored(kErrorColor, "The event name must not be empty");
        if (dataFor_ != current) { // another selection: show its data
            dataFor_ = current;
            const Json& data = zone::interactable_json(z.state, now.tile, current)->value("data", Json());
            dataText_ = data.is_null() ? "" : data.dump(2);
            dataError_.clear();
        }
        if (ImGui::InputTextMultiline("Data (JSON object)", &dataText_, ImVec2(0, ImGui::GetTextLineHeight() * 5))) {
            const Json parsed = Json::parse(dataText_, nullptr, false);
            Json& entry = *zone::interactable_json(z.state, now.tile, current);
            dataError_.clear();
            if (dataText_.find_first_not_of(" \t\r\n") == std::string::npos)
                entry.erase("data");
            else if (parsed.is_object())
                entry["data"] = parsed;
            else
                dataError_ = parsed.is_discarded() ? "not valid JSON (the last valid data is kept)" : "must be a JSON object";
        }
        undoScope(z);
        if (!dataError_.empty())
            ImGui::TextColored(kErrorColor, "Data: %s", dataError_.c_str());
    }
    if (ImGui::Button("Delete object")) {
        z.edit([&](zone::ZoneState& s) { zone::delete_object(s, now); });
        object_.reset();
    }
}

// --- Bake ---

void ZonePanel::drawBake(ZoneSession& zone)
{
    if (!header("Bake", "Bake"))
        return;
    ImGui::TextWrapped("Bakes .server/nav/<tile>.navtile for the world server with kast_mapbake, on the saved map.");
    if (ImGui::InputText("kast_mapbake.exe", &mapbake_))
        mapbakeEdited_ = true;
    if (mapbakeEdited_ && !ImGui::IsItemActive()) {
        mapbakeEdited_ = false;
        std::string err;
        if (!settingsFile_.empty() && !saveEditorSetting(settingsFile_, "mapbake", mapbake_, err))
            kast::log::error("Zone: Bake: " + err);
    }
    const fs::path exe = zone::find_mapbake(mapbake_);
    if (exe.empty())
        ImGui::TextColored(kErrorColor, "kast_mapbake.exe not found: build services/world, or set its path");
    else
        ImGui::TextDisabled("Runs %s", utf8(exe).c_str());
    const zone::ZoneDoc* z = zone.zone();
    ImGui::BeginDisabled(!z || z->dirty() || bake_.running() || exe.empty());
    if (ImGui::Button("Bake Navmesh"))
        startBake(zone);
    ImGui::EndDisabled();
    if (z && z->dirty())
        disabledTooltip("Save the zone first");
    if (bake_.running()) {
        ImGui::SameLine();
        ImGui::Text("Baking... %c", "|/-\\"[int(ImGui::GetTime() * 8.0) & 3]);
    } else if (bakeExit_) {
        ImGui::SameLine();
        ImGui::TextColored(*bakeExit_ == 0 ? kOkColor : kErrorColor, "Exit code %d", *bakeExit_);
    }
    if (ImGui::BeginChild("##bakeLog", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 8), ImGuiChildFlags_Borders)) {
        for (const std::string& line : bakeLog_)
            ImGui::TextUnformatted(line.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

bool ZonePanel::startBake(ZoneSession& zone)
{
    const zone::ZoneDoc* z = zone.zone();
    if (!z) {
        kast::log::error("Zone: Bake: no zone open");
        return false;
    }
    if (z->dirty()) {
        kast::log::error("Zone: Bake: save the zone first");
        return false;
    }
    if (!zone.check()) {
        kast::log::error(std::format("Zone: Bake: {} problem(s), fix them first", zone.problems().size()));
        return false;
    }
    const fs::path exe = zone::find_mapbake(mapbake_);
    std::string err;
    if (exe.empty())
        err = "kast_mapbake.exe not found; set its path in the Bake section";
    const fs::path dir = zone.project().root / "maps" / fs::path(std::u8string(z->folder.begin(), z->folder.end()));
    if (exe.empty() || !bake_.start(exe, {dir}, err)) {
        kast::log::error("Zone: Bake: " + err);
        return false;
    }
    bakeLog_ = {"> " + utf8(exe) + " " + utf8(dir)};
    bakeExit_.reset();
    kast::log::info(std::format("Zone: baking maps/{}: {}", z->folder, bakeLog_.front()));
    return true;
}

void ZonePanel::pollBake()
{
    for (std::string& line : bake_.takeLines()) {
        kast::log::info("Bake: " + line);
        bakeLog_.push_back(std::move(line));
    }
    if (const auto code = bake_.exitCode(); code && code != bakeExit_ && !bake_.running()) {
        bakeExit_ = code;
        bakeLog_.push_back(std::format("exit code {}", *code));
        const std::string line = std::format("Zone: Bake finished: exit code {}", *code);
        if (*code == 0)
            kast::log::info(line);
        else
            kast::log::error(line);
    }
}
