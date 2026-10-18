// The Areas, Spawns and Objects panels and their tools in the 3D view (Panels, see zone_panels.h): drawing rect and
// polygon areas, placing spawns, adding path points, placing zone objects and scattering visuals, dragging area
// vertices, spawns, path points and objects, and their overlays (zone objects' shapes as ground outlines).
#include "zone_panels.h"

#include <scene_io.h> // jsonNumber
#include <ui_common.h>

#include <bx/math.h>
#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <format>

namespace zone {
namespace {

namespace km = kast::map;

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
constexpr ImVec4 kErrorText{1.0f, 0.45f, 0.35f, 1.0f};
constexpr float kHandlePx = 10.0f; // grab distance of a handle, in window points
constexpr float kHeadingLength = 8.0f; // metres

bool near(ImVec2 a, ImVec2 b) { return (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) <= kHandlePx * kHandlePx; }

std::string str(const Json& j, const char* key) {
    const Json v = j.value(key, Json());
    return v.is_string() ? v.get<std::string>() : std::string();
}

float number(const Json& j, const char* key, float fallback) {
    const Json v = j.value(key, Json());
    return v.is_number() ? v.get<float>() : fallback;
}

Vec3 vec3(const Json& v) {
    if (!v.is_array() || v.size() != 3) return {};
    return {v[0].is_number() ? v[0].get<float>() : 0, v[1].is_number() ? v[1].get<float>() : 0, v[2].is_number() ? v[2].get<float>() : 0};
}

// Even-odd point in polygon (as kast::map's).
bool inside(const std::vector<Point>& poly, float x, float z) {
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        if ((poly[i][1] > z) != (poly[j][1] > z) &&
            x < (poly[j][0] - poly[i][0]) * (z - poly[i][1]) / (poly[j][1] - poly[i][1]) + poly[i][0])
            in = !in;
    return in;
}

// One above the highest priority, so a new area never overlaps another at equal priority.
int next_priority(const ZoneState& state) {
    int top = -1;
    if (state.areas.is_object())
        for (const Json& a : state.areas.value("areas", Json::array())) top = std::max(top, a.value("priority", 0));
    return top + 1;
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// One undo step per edit of the last item: from its activation to its deactivation.
void undoScope(ZoneDoc& doc) {
    if (ImGui::IsItemActivated()) doc.begin();
    if (ImGui::IsItemDeactivated()) doc.commit();
}

// An object's shape as the parser reads it, for the overlay; nullopt for none (or a bad one: validate() reports it).
std::optional<km::Shape> shape_of(const Json& object) {
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

std::string stem(const std::string& prefab) { return std::filesystem::path(prefab).stem().string(); }

ImU32 type_color(km::ObjectType type) {
    return type == km::ObjectType::Visual ? kVisualColor : type == km::ObjectType::Obstacle ? kObstacleColor : kInteractableColor;
}

constexpr const char* kTypeNames[] = {"Visual", "Obstacle", "Interactable"}; // in km::ObjectType order

bool id_used(const ZoneState& state, uint32_t id, ObjectRef except) {
    for (const ObjectRef& ref : all_objects(state))
        if (!(ref == except) && object_json(state, ref).value("id", Json()) == id) return true;
    return false;
}

} // namespace

std::optional<ImVec2> Panels::screen(const ZoneDoc& doc, float x, float z) const {
    const auto p = view_.project(view_.groundPoint(doc, x, z));
    if (!p) return std::nullopt;
    return ImVec2((*p)[0], (*p)[1]);
}

void Panels::updateObjects(Session& session, bool click) {
    ZoneDoc& doc = *session.doc;
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        if (!polygon_.empty() || rect_) {
            polygon_.clear();
            rect_.reset();
        } else {
            tool_ = Tool::None;
        }
    }
    const Point at = hover_ ? Point{(*hover_)[0], (*hover_)[2]} : Point{};

    switch (tool_) {
    case Tool::AreaRect:
        if (click && hover_) rect_ = {{at, at}};
        if (rect_ && hover_) (*rect_)[1] = at;
        if (rect_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const auto [a, b] = *rect_;
            if (std::fabs(a[0] - b[0]) >= 1 && std::fabs(a[1] - b[1]) >= 1)
                doc.edit([&](ZoneState& s) { area_ = add_rect_area(s, unique_area_id(s, "area"), a, b, next_priority(s)); });
            rect_.reset();
        }
        if (rect_) {
            const auto [a, b] = *rect_;
            const Point c[4] = {a, {b[0], a[1]}, b, {a[0], b[1]}};
            for (int i = 0; i < 4; ++i) view_.drawGroundLine(doc, c[i], c[(i + 1) % 4], kDraftColor, 2.0f);
        }
        break;
    case Tool::AreaPolygon: {
        bool finish = !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Enter) && polygon_.size() >= 3;
        if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Backspace) && !polygon_.empty()) polygon_.pop_back();
        if (click && hover_) {
            const auto first = polygon_.empty() ? std::nullopt : screen(doc, polygon_[0][0], polygon_[0][1]);
            if (polygon_.size() >= 3 && first && near(*first, io.MousePos)) finish = true;
            else polygon_.push_back(at);
        }
        if (finish) {
            doc.edit([&](ZoneState& s) { area_ = add_polygon_area(s, unique_area_id(s, "area"), polygon_, next_priority(s)); });
            polygon_.clear();
        }
        for (size_t i = 0; i + 1 < polygon_.size(); ++i) view_.drawGroundLine(doc, polygon_[i], polygon_[i + 1], kDraftColor, 2.0f);
        if (!polygon_.empty() && hover_) view_.drawGroundLine(doc, polygon_.back(), at, kDraftColor, 1.0f);
        break;
    }
    case Tool::PlaceSpawn:
        if (click && hover_) {
            const std::string npc = session.npcs.empty() ? std::string("npc") : session.npcs.front();
            doc.edit([&](ZoneState& s) {
                spawn_ = unique_spawn_id(s, lower(npc) + "_1");
                add_spawn(s, spawn_, npc, *hover_, 0, 60);
            });
        }
        break;
    case Tool::SetSpawn:
        if (click && hover_) doc.edit([&](ZoneState& s) { s.manifest["spawn"]["pos"] = vec3_json(*hover_); });
        break;
    case Tool::PlaceObject:
        if (click && hover_ && !placePrefab_.empty())
            doc.edit([&](ZoneState& s) { object_ = add_object(s, placeType_, placePrefab_, *hover_, 0, 1); });
        break;
    case Tool::Scatter:
        updateScatter(session, click);
        break;
    case Tool::PathPoints:
        if (const auto ref = find_spawn(doc.state, spawn_); ref && click && hover_)
            doc.edit([&](ZoneState& s) { add_path_point(s, *ref, *hover_); });
        break;
    case Tool::None: {
        const auto ref = find_spawn(doc.state, spawn_);
        if (click) {
            drag_ = {};
            // Handles first (the selection's vertices and path points, then any spawn), else select what is under it.
            if (area_ && doc.state.areas.is_object() && *area_ < doc.state.areas["areas"].size()) {
                const auto points = area_points(doc.state.areas["areas"][*area_]);
                for (size_t i = 0; i < points.size() && drag_.kind == Drag::Kind::None; ++i)
                    if (const auto p = screen(doc, points[i][0], points[i][1]); p && near(*p, io.MousePos))
                        drag_ = {Drag::Kind::AreaVertex, i};
            }
            if (ref && drag_.kind == Drag::Kind::None)
                if (const Json* route = spawn_path(doc.state, *ref)) {
                    const Json& points = route->value("points", Json::array());
                    for (size_t i = 0; i < points.size() && drag_.kind == Drag::Kind::None; ++i) {
                        const Vec3 p = vec3(points[i]);
                        if (const auto s = screen(doc, p[0], p[2]); s && near(*s, io.MousePos)) drag_ = {Drag::Kind::PathPoint, i};
                    }
                }
            for (const SpawnRef& r : all_spawns(doc.state)) {
                if (drag_.kind != Drag::Kind::None) break;
                const Json& s = spawn_json(doc.state, r);
                const Vec3 p = vec3(s.value("pos", Json()));
                if (const auto sp = screen(doc, p[0], p[2]); sp && near(*sp, io.MousePos)) {
                    spawn_ = str(s, "id");
                    drag_ = {Drag::Kind::Spawn, 0};
                }
            }
            // Then zone objects: the marker nearest the mouse.
            float nearest = kHandlePx * kHandlePx;
            if (drag_.kind == Drag::Kind::None)
                for (const ObjectRef& r : all_objects(doc.state)) {
                    const Vec3 p = vec3(object_json(doc.state, r).value("pos", Json()));
                    const auto sp = screen(doc, p[0], p[2]);
                    if (!sp) continue;
                    const float d = (sp->x - io.MousePos.x) * (sp->x - io.MousePos.x) + (sp->y - io.MousePos.y) * (sp->y - io.MousePos.y);
                    if (d > nearest) continue;
                    nearest = d;
                    object_ = r;
                    drag_ = {Drag::Kind::Object, 0};
                }
            if (drag_.kind != Drag::Kind::None) {
                doc.begin(); // the whole drag is one undo step
            } else if (hover_) {
                object_.reset();
                // The highest-priority area under the click, as the server picks.
                area_.reset();
                int best = 0;
                if (doc.state.areas.is_object())
                    for (size_t i = 0; i < doc.state.areas.value("areas", Json::array()).size(); ++i) {
                        const Json& a = doc.state.areas["areas"][i];
                        if (inside(area_points(a), at[0], at[1]) && (!area_ || a.value("priority", 0) > best)) {
                            area_ = i;
                            best = a.value("priority", 0);
                        }
                    }
            }
        }
        if (drag_.kind != Drag::Kind::None && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            doc.commit();
            drag_ = {};
        }
        if (drag_.kind != Drag::Kind::None && hover_) {
            const auto now = find_spawn(doc.state, spawn_);
            switch (drag_.kind) {
            case Drag::Kind::AreaVertex: move_area_vertex(areas_list(doc.state)[*area_], drag_.index, at); break;
            case Drag::Kind::Spawn:
                if (now) move_spawn(doc.state, *now, *hover_);
                break;
            case Drag::Kind::PathPoint:
                if (Json* route = now ? spawn_path(doc.state, *now) : nullptr) (*route)["points"][drag_.index] = vec3_json(*hover_);
                break;
            case Drag::Kind::Object:
                if (object_) object_ = move_object(doc.state, *object_, *hover_);
                break;
            case Drag::Kind::None: break;
            }
        }
        break;
    }
    default: break;
    }
    drawObjectOverlays(session);
}

void Panels::drawObjectOverlays(const Session& session) const {
    const ZoneDoc& doc = *session.doc;
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    if (doc.state.areas.is_object()) {
        const Json& list = doc.state.areas.value("areas", Json::array());
        for (size_t i = 0; i < list.size(); ++i) {
            const auto points = area_points(list[i]);
            if (points.size() < 3) continue;
            const bool selected = area_ == i;
            const ImU32 color = selected ? kAreaSelected : list[i].contains("portal") ? kPortalColor : kAreaColor;
            for (size_t k = 0; k < points.size(); ++k)
                view_.drawGroundLine(doc, points[k], points[(k + 1) % points.size()], color, selected ? 3.0f : 1.5f);
            Point c{};
            for (const Point& p : points) c = {c[0] + p[0] / float(points.size()), c[1] + p[1] / float(points.size())};
            if (const auto s = screen(doc, c[0], c[1])) {
                const std::string label = std::format("{} (p{}){}", str(list[i], "id"), list[i].value("priority", 0),
                                                      list[i].contains("portal") ? " portal" : "");
                draw->AddText(*s, color, label.c_str());
            }
            if (selected)
                for (const Point& p : points)
                    if (const auto s = screen(doc, p[0], p[1]))
                        draw->AddRectFilled({s->x - 4, s->y - 4}, {s->x + 4, s->y + 4}, kAreaSelected);
        }
    }
    for (const ObjectRef& ref : all_objects(doc.state)) {
        const Json& o = object_json(doc.state, ref);
        const km::ObjectType type = object_type(o);
        const Vec3 p = vec3(o.value("pos", Json()));
        const bool selected = object_ == ref;
        const ImU32 color = selected ? kAreaSelected : type_color(type);
        if (const auto shape = type == km::ObjectType::Visual ? std::nullopt : shape_of(o)) {
            const auto outline = km::footprint(*shape, p, number(o, "yaw", 0));
            for (size_t k = 0; k < outline.size(); ++k)
                view_.drawGroundLine(doc, outline[k], outline[(k + 1) % outline.size()], color, selected ? 3.0f : 1.5f);
        }
        const auto at = screen(doc, p[0], p[2]);
        if (!at) continue;
        draw->AddCircleFilled(*at, selected ? 6.0f : type == km::ObjectType::Visual ? 2.5f : 4.0f, color);
        if (type == km::ObjectType::Interactable) {
            const std::string label = std::format("#{} {}", o.value("id", 0u), o.value("name", stem(str(o, "prefab"))));
            draw->AddText({at->x + 8, at->y - 8}, color, label.c_str());
        } else if (selected) {
            draw->AddText({at->x + 8, at->y - 8}, color, stem(str(o, "prefab")).c_str());
        }
    }
    for (const auto& [tile, file] : doc.state.spawns) {
        const Json paths = file.value("paths", Json::object()); // items() must not outlive what it walks
        for (const auto& [name, route] : paths.items()) {
            const Json& points = route.value("points", Json::array());
            for (size_t i = 0; i + 1 < points.size() + (route.value("loop", true) && points.size() > 2 ? 1 : 0); ++i) {
                const Vec3 a = vec3(points[i]), b = vec3(points[(i + 1) % points.size()]);
                view_.drawGroundLine(doc, {a[0], a[2]}, {b[0], b[2]}, kPathColor, 1.5f);
            }
            for (const Json& p : points)
                if (const auto s = screen(doc, vec3(p)[0], vec3(p)[2])) draw->AddCircleFilled(*s, 3.5f, kPathColor);
        }
        for (const Json& s : file.value("spawns", Json::array())) {
            const Vec3 p = vec3(s.value("pos", Json()));
            const auto at = screen(doc, p[0], p[2]);
            if (!at) continue;
            const bool selected = str(s, "id") == spawn_;
            const ImU32 color = selected ? kAreaSelected : kSpawnMarker;
            draw->AddCircleFilled(*at, selected ? 7.0f : 5.0f, color);
            const float h = number(s, "heading", 0);
            if (const auto tip = screen(doc, p[0] + kHeadingLength * std::sin(h), p[2] + kHeadingLength * std::cos(h)))
                draw->AddLine(*at, *tip, color, 2.5f);
            const std::string label = std::format("{} ({})", str(s, "id"), str(s, "npc"));
            draw->AddText({at->x + 8, at->y - 8}, color, label.c_str());
        }
    }
}

void Panels::drawAreas(Session& session) {
    if (ImGui::Begin("Areas") && session.doc) {
        ZoneDoc& doc = *session.doc;
        toolButton("Draw Rect", Tool::AreaRect);
        toolButton("Draw Polygon", Tool::AreaPolygon);
        ImGui::NewLine();
        if (tool_ == Tool::AreaRect) ImGui::TextDisabled("Drag on the terrain. Esc: stop");
        if (tool_ == Tool::AreaPolygon)
            ImGui::TextDisabled("Click points; click the first or press Enter to close. Backspace: undo a point. Esc: cancel");
        if (tool_ == Tool::None) ImGui::TextDisabled("Click an area to select it; drag its corners to reshape");

        const size_t count = doc.state.areas.is_object() ? doc.state.areas.value("areas", Json::array()).size() : 0;
        if (area_ && *area_ >= count) area_.reset();
        if (ImGui::BeginChild("##areaList", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 6), ImGuiChildFlags_Borders)) {
            for (size_t i = 0; i < count; ++i) {
                const Json& a = doc.state.areas["areas"][i];
                const std::string label = std::format("{}  ({}, priority {}){}##{}", str(a, "id"), str(a, "name"),
                                                      a.value("priority", 0), a.contains("portal") ? "  portal" : "", i);
                if (ImGui::Selectable(label.c_str(), area_ == i)) area_ = i;
            }
        }
        ImGui::EndChild();

        if (area_) {
            Json& a = areas_list(doc.state)[*area_];
            std::string id = str(a, "id");
            if (ImGui::InputText("Id", &id)) rename_area(doc.state, *area_, id);
            undoScope(doc);
            std::string name = str(a, "name");
            if (ImGui::InputText("Name", &name)) areas_list(doc.state)[*area_]["name"] = name;
            undoScope(doc);
            int priority = a.value("priority", 0);
            if (ImGui::InputInt("Priority", &priority)) areas_list(doc.state)[*area_]["priority"] = priority;
            undoScope(doc);

            const std::string parent = str(a, "parent");
            if (ImGui::BeginCombo("Parent", parent.empty() ? "(none)" : parent.c_str())) {
                if (ImGui::Selectable("(none)", parent.empty()))
                    doc.edit([&](ZoneState& s) { areas_list(s)[*area_].erase("parent"); });
                for (size_t i = 0; i < count; ++i) {
                    const std::string other = str(doc.state.areas["areas"][i], "id");
                    if (i != *area_ && ImGui::Selectable(other.c_str(), other == parent))
                        doc.edit([&](ZoneState& s) { areas_list(s)[*area_]["parent"] = other; });
                }
                ImGui::EndCombo();
            }

            bool portal = areas_list(doc.state)[*area_].contains("portal");
            if (ImGui::Checkbox("Portal", &portal))
                doc.edit([&](ZoneState& s) {
                    Json& area = areas_list(s)[*area_];
                    if (!portal) area.erase("portal");
                    else area["portal"] = {{"map", session.project && !session.project->maps.empty()
                                                       ? unsigned(session.project->maps.front().info.id) : 0u},
                                           {"pos", Json::array({128, 0, 128})},
                                           {"heading", 0}};
                });
            if (portal) {
                Json& p = areas_list(doc.state)[*area_]["portal"];
                const unsigned target = p.value("map", 0u);
                std::string current = std::format("{} (unknown map)", target);
                if (session.project)
                    for (const ProjectMap& m : session.project->maps)
                        if (m.info.id == target) current = std::format("{}  maps/{}", target, m.folder);
                if (ImGui::BeginCombo("Target map", current.c_str())) {
                    if (session.project)
                        for (const ProjectMap& m : session.project->maps)
                            if (ImGui::Selectable(std::format("{}  maps/{}", m.info.id, m.folder).c_str(), m.info.id == target)) {
                                const unsigned mapId = m.info.id; // unsigned: the parser wants an unsigned number
                                doc.edit([&](ZoneState& s) { areas_list(s)[*area_]["portal"]["map"] = mapId; });
                            }
                    ImGui::EndCombo();
                }
                Vec3 pos = vec3(p.value("pos", Json()));
                if (ImGui::InputFloat3("Arrive at", pos.data(), "%.1f")) areas_list(doc.state)[*area_]["portal"]["pos"] = vec3_json(pos);
                undoScope(doc);
                float heading = number(p, "heading", 0) * 180.0f / bx::kPi;
                if (ImGui::SliderFloat("Arrive heading", &heading, -180.0f, 180.0f, "%.0f deg"))
                    areas_list(doc.state)[*area_]["portal"]["heading"] = kast::jsonNumber(heading * bx::kPi / 180.0f);
                undoScope(doc);
            }
            // Kept as they are; the tool does not edit them.
            const Json& kept = areas_list(doc.state)[*area_];
            for (const char* key : {"y", "music", "flags"})
                if (kept.contains(key)) ImGui::TextDisabled("%s: %s (kept)", key, kept[key].dump().c_str());
            if (ImGui::Button("Delete area")) {
                doc.edit([&](ZoneState& s) { delete_area(s, *area_); });
                area_.reset();
            }
        }
    }
    ImGui::End();
}

void Panels::drawSpawns(Session& session) {
    if (ImGui::Begin("Spawns") && session.doc) {
        ZoneDoc& doc = *session.doc;
        std::string err;
        const auto info = doc.info(err);
        toolButton("Place Spawn", Tool::PlaceSpawn);
        toolButton("Add Path Points", Tool::PathPoints);
        ImGui::NewLine();
        if (tool_ == Tool::PlaceSpawn) ImGui::TextDisabled("Click the terrain to place a spawn. Esc: stop");
        if (tool_ == Tool::PathPoints)
            ImGui::TextDisabled(spawn_.empty() ? "Select a spawn first" : "Click the terrain to add points to its path");
        if (tool_ == Tool::None) ImGui::TextDisabled("Drag spawns and path points to move them");

        if (ImGui::BeginChild("##spawnList", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 6), ImGuiChildFlags_Borders)) {
            for (const SpawnRef& ref : all_spawns(doc.state)) {
                const Json& s = spawn_json(doc.state, ref);
                const std::string label = std::format("{}  {}  (tile {})##{}_{}", str(s, "id"), str(s, "npc"), tile_name(ref.tile),
                                                      tile_name(ref.tile), ref.index);
                if (ImGui::Selectable(label.c_str(), str(s, "id") == spawn_)) spawn_ = str(s, "id");
            }
        }
        ImGui::EndChild();

        const auto ref = find_spawn(doc.state, spawn_);
        if (ref && info) {
            Json& s = spawn_json(doc.state, *ref);
            std::string id = str(s, "id");
            if (ImGui::InputText("Id", &id)) {
                s["id"] = id;
                spawn_ = id;
            }
            undoScope(doc);
            const std::string npc = str(s, "npc");
            if (ImGui::BeginCombo("NPC", npc.c_str())) {
                for (const std::string& name : session.npcs)
                    if (ImGui::Selectable(name.c_str(), name == npc))
                        doc.edit([&](ZoneState& st) { spawn_json(st, *ref)["npc"] = name; });
                ImGui::EndCombo();
            }
            std::string typed = npc;
            if (ImGui::InputText("NPC (any name)", &typed)) spawn_json(doc.state, *ref)["npc"] = typed;
            undoScope(doc);

            const Vec3 pos = vec3(s.value("pos", Json()));
            float xz[2] = {pos[0], pos[2]};
            if (ImGui::InputFloat2("Position x, z", xz, "%.1f")) {
                const float y = ground_y(*info, doc.state, xz[0], xz[1]).value_or(pos[1]);
                move_spawn(doc.state, *ref, {xz[0], y, xz[1]});
            }
            undoScope(doc);
            const auto now = find_spawn(doc.state, spawn_); // the move may have changed its file
            if (!now) {
                ImGui::End();
                return;
            }
            Json& sp = spawn_json(doc.state, *now);
            ImGui::Text("Tile %s, y %.2f (on the ground)", tile_name(now->tile).c_str(), vec3(sp.value("pos", Json()))[1]);
            float heading = number(sp, "heading", 0) * 180.0f / bx::kPi;
            if (ImGui::SliderFloat("Heading", &heading, -180.0f, 180.0f, "%.0f deg"))
                sp["heading"] = kast::jsonNumber(heading * bx::kPi / 180.0f);
            undoScope(doc);
            bool never = number(sp, "respawnSec", -1) < 0;
            float respawn = never ? 60.0f : number(sp, "respawnSec", 60);
            if (ImGui::Checkbox("Never respawns", &never))
                doc.edit([&](ZoneState& st) { spawn_json(st, *now)["respawnSec"] = never ? -1.0 : 60.0; });
            if (!never) {
                if (ImGui::InputFloat("Respawn (s)", &respawn, 1.0f, 10.0f, "%.0f"))
                    spawn_json(doc.state, *now)["respawnSec"] = kast::jsonNumber(std::max(0.0f, respawn));
                undoScope(doc);
            }

            Json& file = doc.state.spawns.at(now->tile);
            const std::string path = str(spawn_json(doc.state, *now), "path");
            if (ImGui::BeginCombo("Path", path.empty() ? "(none)" : path.c_str())) {
                if (ImGui::Selectable("(none)", path.empty())) doc.edit([&](ZoneState& st) { spawn_json(st, *now).erase("path"); });
                const Json paths = file.value("paths", Json::object()); // items() must not outlive what it walks
                for (const auto& [name, route] : paths.items())
                    if (ImGui::Selectable(name.c_str(), name == path))
                        doc.edit([&](ZoneState& st) { spawn_json(st, *now)["path"] = name; });
                ImGui::EndCombo();
            }
            if (Json* route = spawn_path(doc.state, *now)) {
                float wait = number(*route, "wait", 0);
                if (ImGui::InputFloat("Wait at points (s)", &wait, 0.5f, 1.0f, "%.1f"))
                    (*spawn_path(doc.state, *now))["wait"] = kast::jsonNumber(std::max(0.0f, wait));
                undoScope(doc);
                bool loop = route->value("loop", true);
                if (ImGui::Checkbox("Loop (off: walk back along it)", &loop))
                    doc.edit([&](ZoneState& st) { (*spawn_path(st, *now))["loop"] = loop; });
                const size_t points = spawn_path(doc.state, *now)->value("points", Json::array()).size();
                ImGui::Text("%zu point(s)", points);
                ImGui::SameLine();
                ImGui::BeginDisabled(points <= 1); // a path needs at least one point
                if (ImGui::SmallButton("Remove last point"))
                    doc.edit([&](ZoneState& st) { (*spawn_path(st, *now))["points"].erase(points - 1); });
                ImGui::EndDisabled();
                if (ImGui::Button("Delete path")) doc.edit([&](ZoneState& st) { delete_path(st, now->tile, path); });
            } else if (ImGui::Button("New path from here")) {
                tool_ = Tool::PathPoints;
            }
            if (ImGui::Button("Delete spawn")) {
                doc.edit([&](ZoneState& st) { delete_spawn(st, *now); });
                spawn_.clear();
            }
        }
    }
    ImGui::End();
}

void Panels::updateScatter(Session& session, bool click) {
    ZoneDoc& doc = *session.doc;
    if (hover_) view_.drawGroundCircle(doc, (*hover_)[0], (*hover_)[2], scatter_.radius, scatterErase_ ? kEraseColor : kScatterColor);
    if (click && hover_ && (scatterErase_ || !scatter_.prefabs.empty())) {
        doc.begin(); // the whole stroke is one undo step
        stroking_ = true;
        lastDab_.reset();
    }
    if (stroking_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        doc.commit();
        stroking_ = false;
    }
    if (!stroking_ || !hover_) return;
    const Point at{(*hover_)[0], (*hover_)[2]};
    if (lastDab_ && std::hypot(at[0] - (*lastDab_)[0], at[1] - (*lastDab_)[1]) < scatter_.radius) return;
    lastDab_ = at;
    std::string err;
    const auto info = doc.info(err);
    if (!info) return;
    if (scatterErase_) erase_visuals(doc.state, at[0], at[1], scatter_.radius);
    else scatter(*info, doc.state, scatter_, scatterRng_, at[0], at[1]);
}

void Panels::drawScatter(Session& session) {
    ImGui::SeparatorText("Scatter brush (visuals)");
    ImGui::Checkbox("Erase visuals", &scatterErase_);
    ImGui::TextDisabled(scatterErase_ ? "Drag on the terrain to remove visuals in the circle. Esc: stop"
                                      : "Drag on the terrain: a dab every radius moved, one undo step per stroke. Esc: stop");
    ImGui::SliderFloat("Radius (m)", &scatter_.radius, 0.5f, 64.0f, "%.1f");
    if (!scatterErase_) {
        ImGui::SliderInt("Per dab", &scatter_.count, 1, 100);
        ImGui::Checkbox("Random yaw", &scatter_.randomYaw);
        ImGui::DragFloatRange2("Scale", &scatter_.scaleMin, &scatter_.scaleMax, 0.01f, 0.05f, 10.0f, "min %.2f", "max %.2f");
        if (ImGui::InputInt("Seed", &scatterSeed_)) scatterRng_ = ScatterRng(uint32_t(scatterSeed_));
        ImGui::SameLine();
        if (ImGui::SmallButton("Restart")) scatterRng_ = ScatterRng(uint32_t(scatterSeed_)); // replay from the seed
        ImGui::TextUnformatted("Prefabs (one picked at random per object):");
        if (ImGui::BeginChild("##scatterPrefabs", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 5), ImGuiChildFlags_Borders)) {
            for (const std::string& prefab : session.prefabs) {
                const auto it = std::find(scatter_.prefabs.begin(), scatter_.prefabs.end(), prefab);
                bool on = it != scatter_.prefabs.end();
                if (ImGui::Checkbox(prefab.c_str(), &on)) {
                    if (on) scatter_.prefabs.push_back(prefab);
                    else scatter_.prefabs.erase(it);
                }
            }
        }
        ImGui::EndChild();
        if (scatter_.prefabs.empty()) ImGui::TextColored(kErrorText, "Tick at least one prefab");
    }
}

void Panels::drawZoneObjects(Session& session) {
    if (ImGui::Begin("Objects") && session.doc) {
        ZoneDoc& doc = *session.doc;
        toolButton("Place Object", Tool::PlaceObject);
        toolButton("Scatter", Tool::Scatter);
        ImGui::NewLine();
        if (tool_ == Tool::PlaceObject) ImGui::TextDisabled("Click the terrain to place one. Esc: stop");
        if (tool_ == Tool::None) ImGui::TextDisabled("Click an object's marker to select it; drag it to move it");
        if (placePrefab_.empty() && !session.prefabs.empty()) placePrefab_ = session.prefabs.front();
        if (ImGui::BeginCombo("Prefab", placePrefab_.empty() ? "(no .prefab in the project)" : placePrefab_.c_str())) {
            for (const std::string& prefab : session.prefabs)
                if (ImGui::Selectable(prefab.c_str(), prefab == placePrefab_)) placePrefab_ = prefab;
            ImGui::EndCombo();
        }
        int type = int(placeType_);
        if (ImGui::Combo("Type", &type, kTypeNames, IM_ARRAYSIZE(kTypeNames))) placeType_ = km::ObjectType(type);
        if (tool_ == Tool::Scatter) drawScatter(session);

        const std::vector<ObjectRef> refs = all_objects(doc.state);
        if (object_ && std::find(refs.begin(), refs.end(), *object_) == refs.end()) object_.reset(); // undone or deleted
        ImGui::Text("%zu object(s)", refs.size());
        if (ImGui::BeginChild("##objectList", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 6), ImGuiChildFlags_Borders)) {
            ImGuiListClipper clipper; // scattered tiles hold thousands
            clipper.Begin(int(refs.size()));
            while (clipper.Step())
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const Json& o = object_json(doc.state, refs[i]);
                    const std::string id = o.contains("id") ? std::format(" #{}", o.value("id", 0u)) : "";
                    const std::string label = std::format("{} {}{}  (tile {})##{}_{}", str(o, "type"), stem(str(o, "prefab")), id,
                                                          tile_name(refs[i].tile), tile_name(refs[i].tile), refs[i].index);
                    if (ImGui::Selectable(label.c_str(), object_ == refs[i])) object_ = refs[i];
                }
        }
        ImGui::EndChild();
        if (object_) drawObjectFields(session);
    }
    ImGui::End();
}

void Panels::drawObjectFields(Session& session) {
    ZoneDoc& doc = *session.doc;
    std::string err;
    const auto info = doc.info(err);
    if (!info) return;
    const ObjectRef ref = *object_;
    ImGui::SeparatorText("Selected object");
    int t = int(object_type(object_json(doc.state, ref)));
    if (ImGui::Combo("Object type", &t, kTypeNames, IM_ARRAYSIZE(kTypeNames)))
        doc.edit([&](ZoneState& s) { set_object_type(s, ref, km::ObjectType(t)); });
    const km::ObjectType type = object_type(object_json(doc.state, ref)); // after a change of it
    const std::string prefab = str(object_json(doc.state, ref), "prefab");
    if (ImGui::BeginCombo("Object prefab", prefab.c_str())) {
        for (const std::string& p : session.prefabs)
            if (ImGui::Selectable(p.c_str(), p == prefab)) doc.edit([&](ZoneState& s) { object_json(s, ref)["prefab"] = p; });
        ImGui::EndCombo();
    }
    const auto changed = [&] { // after a field edit: the server entry follows
        if (type == km::ObjectType::Interactable) sync_interactable(doc.state, *object_);
    };
    const Vec3 pos = vec3(object_json(doc.state, ref).value("pos", Json()));
    float xz[2] = {pos[0], pos[2]};
    if (ImGui::InputFloat2("Position x, z", xz, "%.2f")) {
        const float y = ground_y(*info, doc.state, xz[0], xz[1]).value_or(pos[1]);
        object_ = move_object(doc.state, ref, {xz[0], y, xz[1]});
    }
    undoScope(doc);
    const ObjectRef now = *object_; // the move may have changed its file
    Json& o = object_json(doc.state, now);
    ImGui::Text("Tile %s, y %.2f (on the ground)", tile_name(now.tile).c_str(), vec3(o.value("pos", Json()))[1]);
    float yaw = number(o, "yaw", 0) * 180.0f / bx::kPi;
    if (ImGui::SliderFloat("Yaw", &yaw, -180.0f, 180.0f, "%.0f deg")) {
        object_json(doc.state, now)["yaw"] = kast::jsonNumber(yaw * bx::kPi / 180.0f);
        changed();
    }
    undoScope(doc);
    float scale = number(object_json(doc.state, now), "scale", 1);
    if (ImGui::InputFloat("Scale", &scale, 0.1f, 0.5f, "%.2f")) object_json(doc.state, now)["scale"] = kast::jsonNumber(std::max(0.01f, scale));
    undoScope(doc);

    if (type != km::ObjectType::Visual) {
        const auto shape = shape_of(object_json(doc.state, now));
        int kind = shape && shape->kind == km::Shape::Kind::Cylinder ? 1 : 0;
        const char* kinds[] = {"Box", "Cylinder"};
        if (ImGui::Combo("Shape", &kind, kinds, 2))
            doc.edit([&](ZoneState& s) {
                object_json(s, now)["shape"] = kind == 0 ? default_shape() : Json{{"cylinder", {{"radius", 0.5}, {"height", 1}}}};
                if (type == km::ObjectType::Interactable) sync_interactable(s, now);
            });
        if (shape && shape->kind == km::Shape::Kind::Box) {
            float h[3] = {shape->halfExtents[0], shape->halfExtents[1], shape->halfExtents[2]};
            if (ImGui::InputFloat3("Half extents", h, "%.2f")) {
                object_json(doc.state, now)["shape"]["box"]["halfExtents"] =
                    vec3_json({std::max(0.01f, h[0]), std::max(0.01f, h[1]), std::max(0.01f, h[2])});
                changed();
            }
            undoScope(doc);
        } else if (shape) {
            float rh[2] = {shape->radius, shape->height};
            if (ImGui::InputFloat2("Radius, height", rh, "%.2f")) {
                Json& c = object_json(doc.state, now)["shape"]["cylinder"];
                c["radius"] = kast::jsonNumber(std::max(0.01f, rh[0]));
                c["height"] = kast::jsonNumber(std::max(0.01f, rh[1]));
                changed();
            }
            undoScope(doc);
        }
    }

    if (type == km::ObjectType::Interactable) {
        ImGui::SeparatorText("Interactable");
        const uint32_t id = object_json(doc.state, now).value("id", 0u);
        int typed = int(id);
        if (ImGui::InputInt("Id", &typed) && typed >= 1 && !id_used(doc.state, uint32_t(typed), now))
            set_interactable_id(doc.state, now, uint32_t(typed));
        undoScope(doc);
        std::string name = str(object_json(doc.state, now), "name");
        if (ImGui::InputText("Name", &name)) {
            if (name.empty()) object_json(doc.state, now).erase("name");
            else object_json(doc.state, now)["name"] = name;
            changed();
        }
        undoScope(doc);
        const uint32_t current = object_json(doc.state, now).value("id", 0u);
        if (!interactable_json(doc.state, now.tile, current)) sync_interactable(doc.state, now); // a hand-edited file
        ImGui::TextDisabled("Server only (.server/interactables/%s.json):", tile_name(now.tile).c_str());
        float range = number(*interactable_json(doc.state, now.tile, current), "range", 3);
        if (ImGui::InputFloat("Range (m)", &range, 0.5f, 1.0f, "%.1f"))
            (*interactable_json(doc.state, now.tile, current))["range"] = kast::jsonNumber(std::max(0.1f, range));
        undoScope(doc);
        std::string event = str(*interactable_json(doc.state, now.tile, current), "event");
        if (ImGui::InputText("Event", &event)) (*interactable_json(doc.state, now.tile, current))["event"] = event;
        undoScope(doc);
        if (event.empty()) ImGui::TextColored(kErrorText, "The event name must not be empty");
        if (dataFor_ != current) { // another selection: show its data
            dataFor_ = current;
            const Json& data = interactable_json(doc.state, now.tile, current)->value("data", Json());
            dataText_ = data.is_null() ? "" : data.dump(2);
            dataError_.clear();
        }
        if (ImGui::InputTextMultiline("Data (JSON object)", &dataText_, ImVec2(0, ImGui::GetTextLineHeight() * 5))) {
            const Json parsed = Json::parse(dataText_, nullptr, false);
            Json& entry = *interactable_json(doc.state, now.tile, current);
            dataError_.clear();
            if (dataText_.find_first_not_of(" \t\r\n") == std::string::npos) entry.erase("data");
            else if (parsed.is_object()) entry["data"] = parsed;
            else dataError_ = parsed.is_discarded() ? "not valid JSON (the last valid data is kept)" : "must be a JSON object";
        }
        undoScope(doc);
        if (!dataError_.empty()) ImGui::TextColored(kErrorText, "Data: %s", dataError_.c_str());
    }
    if (ImGui::Button("Delete object")) {
        doc.edit([&](ZoneState& s) { delete_object(s, now); });
        object_.reset();
    }
}

} // namespace zone
