// The Areas and Spawns panels and their tools in the 3D view (Panels, see zone_panels.h): drawing rect and polygon
// areas, placing spawns, adding path points, dragging area vertices, spawns and path points, and their overlays.
#include "zone_panels.h"

#include <scene_io.h> // jsonNumber
#include <ui_common.h>

#include <bx/math.h>
#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
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
            if (drag_.kind != Drag::Kind::None) {
                doc.begin(); // the whole drag is one undo step
            } else if (hover_) {
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

} // namespace zone
