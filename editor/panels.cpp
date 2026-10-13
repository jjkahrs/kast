#include "panels.h"

#include "ui_common.h"

#include <components.h>
#include <log.h>
#include <lua_components.h>
#include <scene_io.h>
#include <state_machines.h>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <climits>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>
#include <system_error>
#include <utility>

namespace {

using Json = nlohmann::ordered_json;

constexpr const char* kEntityPayload = "KAST_ENTITY";
constexpr SDL_DialogFileFilter kLuaFilter{"Lua script (*.lua)", "lua"};
constexpr ImVec4 kOkColor{0.4f, 0.85f, 0.4f, 1.0f};
constexpr ImVec4 kErrorColor{1.0f, 0.45f, 0.4f, 1.0f};

bool sameBits(const bx::Quaternion& a, const bx::Quaternion& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }

bool acceptEntityDrop(entt::entity& dropped)
{
    bool accepted = false;
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload)) {
            std::memcpy(&dropped, payload->Data, sizeof(dropped));
            accepted = true;
        }
        ImGui::EndDragDropTarget();
    }
    return accepted;
}

} // namespace

void Panels::drawHierarchy(Document& doc)
{
    if (ImGui::Begin("Hierarchy")) {
        for (entt::entity root : doc.scene.children(entt::null))
            drawNode(doc, root);

        // The rest of the panel: click clears selection, drop unparents to root, right-click creates at root.
        ImVec2 rest = ImGui::GetContentRegionAvail();
        rest.x = bx::max(rest.x, 1.0f);
        rest.y = bx::max(rest.y, ImGui::GetFrameHeight());
        ImGui::InvisibleButton("##empty", rest);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
            doc.selected = entt::null;
        if (entt::entity dropped = entt::null; acceptEntityDrop(dropped))
            edit_ = {Edit::Op::Reparent, dropped, entt::null};
        if (ImGui::BeginPopupContextItem("##rootMenu")) {
            drawCreateItems(entt::null);
            ImGui::EndPopup();
        }

        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput &&
            doc.scene.valid(doc.selected)) {
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
                edit_ = {Edit::Op::Delete, doc.selected};
            if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D))
                edit_ = {Edit::Op::Duplicate, doc.selected};
            if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
                renaming_ = doc.selected;
                focusRename_ = true;
                renameBuffer_ = doc.scene.registry().get<kast::Name>(doc.selected).value;
            }
        }
    }
    ImGui::End();
    applyEdit(doc);
}

void Panels::drawNode(Document& doc, entt::entity entity)
{
    entt::registry& registry = doc.scene.registry();
    const std::vector<entt::entity>& children = registry.get<kast::Hierarchy>(entity).children;
    std::string& name = registry.get<kast::Name>(entity).value;

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
    if (children.empty())
        flags |= ImGuiTreeNodeFlags_Leaf;
    if (doc.selected == entity)
        flags |= ImGuiTreeNodeFlags_Selected;

    ImGui::PushID(static_cast<int>(entt::to_integral(entity)));
    const bool renaming = renaming_ == entity;
    const bool open = ImGui::TreeNodeEx("##node", flags, "%s", renaming ? "" : name.c_str());

    if ((ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) &&
        !ImGui::IsItemToggledOpen())
        doc.selected = entity;
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(kEntityPayload, &entity, sizeof(entity));
        ImGui::TextUnformatted(name.c_str());
        ImGui::EndDragDropSource();
    }
    if (entt::entity dropped = entt::null; acceptEntityDrop(dropped))
        edit_ = {Edit::Op::Reparent, dropped, entity};
    if (ImGui::BeginPopupContextItem("##nodeMenu")) {
        drawCreateItems(entity);
        ImGui::Separator();
        if (ImGui::MenuItem("Rename", "F2")) {
            renaming_ = entity;
            focusRename_ = true;
            renameBuffer_ = name;
        }
        if (ImGui::MenuItem("Duplicate", "Ctrl+D"))
            edit_ = {Edit::Op::Duplicate, entity};
        if (ImGui::MenuItem("Delete", "Del"))
            edit_ = {Edit::Op::Delete, entity};
        ImGui::EndPopup();
    }

    if (renaming) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (std::exchange(focusRename_, false))
            ImGui::SetKeyboardFocusHere();
        bool commit = ImGui::InputText("##rename", &renameBuffer_,
                                       ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        bool cancel = false;
        if (ImGui::IsItemDeactivated()) {
            cancel = ImGui::IsKeyPressed(ImGuiKey_Escape);
            commit = !cancel;
        }
        if (commit && !renameBuffer_.empty() && renameBuffer_ != name) {
            name = renameBuffer_;
            markDirty(doc);
        }
        if (commit || cancel)
            renaming_ = entt::null;
    }

    if (open) {
        for (entt::entity child : children)
            drawNode(doc, child);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void Panels::drawCreateItems(entt::entity parent)
{
    for (ObjectKind kind : kObjectKinds)
        if (ImGui::MenuItem(objectKindLabel(kind)))
            edit_ = {Edit::Op::Create, entt::null, parent, kind};
}

void Panels::applyEdit(Document& doc)
{
    const Edit edit = std::exchange(edit_, Edit{});
    kast::Scene& scene = doc.scene;
    switch (edit.op) {
    case Edit::Op::Create:
        createObject(doc, edit.kind, edit.parent);
        break;
    case Edit::Op::Delete:
        doc.selected = edit.target;
        deleteSelected(doc);
        break;
    case Edit::Op::Duplicate:
        doc.selected = edit.target;
        duplicateSelected(doc);
        break;
    case Edit::Op::Reparent:
        if (!scene.valid(edit.target) || scene.parent(edit.target) == edit.parent)
            break;
        if (scene.reparent(edit.target, edit.parent))
            markDirty(doc);
        else
            kast::log::warn(std::format("Cannot parent '{}' under its own descendant",
                                        scene.registry().get<kast::Name>(edit.target).value));
        break;
    case Edit::Op::None:
        break;
    }
}

bool Panels::drawValue(entt::entity entity, const std::string& key, const kast::FieldDesc& field, const char* label,
                       Json& value)
{
    using kast::FieldKind;
    const bool bounded = field.min || field.max;
    const ImGuiSliderFlags clamp = bounded ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None;
    switch (field.kind) {
    case FieldKind::Float: {
        float v = value.get<float>();
        const float lo = field.min ? static_cast<float>(*field.min) : -FLT_MAX;
        const float hi = field.max ? static_cast<float>(*field.max) : FLT_MAX;
        if (!ImGui::DragFloat(label, &v, 0.05f, bounded ? lo : 0.0f, bounded ? hi : 0.0f, "%.3f", clamp))
            return false;
        value = kast::jsonNumber(v);
        return true;
    }
    case FieldKind::Int: {
        int v = value.get<int>();
        const int lo = field.min ? static_cast<int>(*field.min) : INT_MIN;
        const int hi = field.max ? static_cast<int>(*field.max) : INT_MAX;
        if (!ImGui::DragInt(label, &v, 1.0f, bounded ? lo : 0, bounded ? hi : 0, "%d", clamp))
            return false;
        value = v;
        return true;
    }
    case FieldKind::Bool: {
        bool v = value.get<bool>();
        if (!ImGui::Checkbox(label, &v))
            return false;
        value = v;
        return true;
    }
    case FieldKind::String: {
        std::string v = value.get<std::string>();
        if (!ImGui::InputText(label, &v))
            return false;
        value = v;
        return true;
    }
    case FieldKind::Vec3: {
        float v[3] = {value[0].get<float>(), value[1].get<float>(), value[2].get<float>()};
        if (!(field.color ? ImGui::ColorEdit3(label, v) : ImGui::DragFloat3(label, v, 0.05f)))
            return false;
        value = Json::array({kast::jsonNumber(v[0]), kast::jsonNumber(v[1]), kast::jsonNumber(v[2])});
        return true;
    }
    case FieldKind::Quat: {
        // Edited as Euler degrees, cached while unchanged so they don't jump between equivalent forms.
        bx::Quaternion rotation{value[0].get<float>(), value[1].get<float>(), value[2].get<float>(), value[3].get<float>()};
        if (eulerEntity_ != entity || eulerField_ != key || !sameBits(eulerSource_, rotation)) {
            eulerEntity_ = entity;
            eulerField_ = key;
            eulerSource_ = rotation;
            euler_ = kast::eulerDegFromQuat(rotation);
        }
        if (!ImGui::DragFloat3(label, &euler_.x, 0.5f))
            return false;
        rotation = kast::quatFromEulerDeg(euler_);
        eulerSource_ = rotation;
        value = Json::array({kast::jsonNumber(rotation.x), kast::jsonNumber(rotation.y), kast::jsonNumber(rotation.z),
                             kast::jsonNumber(rotation.w)});
        return true;
    }
    case FieldKind::Enum: {
        const auto current = std::ranges::find(field.enumValues, value.get<std::string>());
        const size_t index = static_cast<size_t>(current - field.enumValues.begin());
        bool changed = false;
        if (ImGui::BeginCombo(label, index < field.enumLabels.size() ? field.enumLabels[index].c_str() : "")) {
            for (size_t i = 0; i < field.enumValues.size(); ++i) {
                if (ImGui::Selectable(field.enumLabels[i].c_str(), i == index) && i != index) {
                    value = field.enumValues[i];
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    }
    }
    return false;
}

bool Panels::drawField(entt::entity entity, const kast::ComponentType& type, const kast::FieldDesc& field, Json& value)
{
    const std::string key = std::format("{}.{}", type.name(), field.name);
    if (!field.array)
        return drawValue(entity, key, field, field.displayName.c_str(), value);

    // Arrays: one row per element (value, move up/down, remove), then an Add button.
    bool changed = false;
    ImGui::PushID(field.name.c_str());
    const std::string title = std::format("{} [{}]", field.displayName, value.size());
    if (ImGui::TreeNodeEx("##array", ImGuiTreeNodeFlags_DefaultOpen, "%s", title.c_str())) {
        enum class Op { None, Up, Down, Remove } op = Op::None;
        size_t target = 0;
        const float buttons = ImGui::GetFrameHeight() * 3.0f + ImGui::GetStyle().ItemSpacing.x * 3.0f;
        for (size_t i = 0; i < value.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::SetNextItemWidth(bx::max(ImGui::GetContentRegionAvail().x - buttons, 1.0f));
            changed |= drawValue(entity, std::format("{}[{}]", key, i), field, "##value", value[i]);
            ImGui::SameLine();
            if (ImGui::ArrowButton("##up", ImGuiDir_Up))
                op = Op::Up, target = i;
            ImGui::SameLine();
            if (ImGui::ArrowButton("##down", ImGuiDir_Down))
                op = Op::Down, target = i;
            ImGui::SameLine();
            if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0.0f)))
                op = Op::Remove, target = i;
            ImGui::PopID();
        }
        const auto at = [&](size_t i) { return value.begin() + static_cast<ptrdiff_t>(i); };
        if (op == Op::Up && target > 0)
            std::iter_swap(at(target), at(target - 1)), changed = true;
        else if (op == Op::Down && target + 1 < value.size())
            std::iter_swap(at(target), at(target + 1)), changed = true;
        else if (op == Op::Remove)
            value.erase(at(target)), changed = true;
        if (ImGui::Button("Add Element")) {
            value.push_back(kast::zeroFieldValue(field, true));
            changed = true;
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
    return changed;
}

void Panels::drawInspector(Document& doc)
{
    if (ImGui::Begin("Inspector")) {
        if (!doc.scene.valid(doc.selected)) {
            ImGui::TextDisabled("No entity selected");
        } else {
            entt::registry& registry = doc.scene.registry();
            const entt::entity entity = doc.selected;
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.65f); // leave room for the labels
            bool changed = ImGui::InputText("Name", &registry.get<kast::Name>(entity).value);

            const std::vector<kast::ComponentType> types = kast::allComponentTypes();
            kast::ComponentType removed;
            for (const kast::ComponentType& type : types) {
                if (!kast::hasComponent(registry, entity, type))
                    continue;
                ImGui::PushID(type.name()); // field labels repeat across components
                bool keep = true;           // the header's x button (removable types only) clears it
                if (ImGui::CollapsingHeader(type.displayName(), type.removable() ? &keep : nullptr,
                                            ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (type.meta() && type.meta() == entt::resolve<kast::StateMachine>()) {
                        changed = drawStateMachineComponent(doc, entity) || changed;
                    } else if (type.meta() && type.meta() == entt::resolve<kast::UIDocument>()) {
                        changed = drawUIDocumentComponent(doc, entity) || changed;
                    } else {
                        Json json = kast::componentToJson(registry, entity, type);
                        for (const kast::FieldDesc& field : type.fields()) {
                            if (!drawField(entity, type, field, json[field.name]))
                                continue;
                            // Rejected edits (e.g. Camera ranges) are not applied: the last valid value stays, so
                            // the scene stays loadable.
                            if (std::string error; kast::setComponentJson(registry, entity, type, json, error))
                                changed = true;
                            else
                                json = kast::componentToJson(registry, entity, type);
                        }
                    }
                }
                if (!keep)
                    removed = type;
                ImGui::PopID();
            }
            if (removed) {
                kast::removeComponent(registry, entity, removed);
                changed = true;
            }

            // Data of components with no valid definition: read-only, saved unchanged, removable.
            std::string removedMissing;
            if (const auto* preserved = registry.try_get<kast::PreservedComponents>(entity)) {
                for (const auto& [name, value] : preserved->byName) {
                    ImGui::PushID(("missing/" + name).c_str());
                    bool keep = true;
                    const std::string title = "Missing: " + name;
                    if (ImGui::CollapsingHeader(title.c_str(), &keep, ImGuiTreeNodeFlags_DefaultOpen)) {
                        ImGui::TextDisabled("No valid definition: kept as-is and saved unchanged.");
                        ImGui::TextWrapped("%s", value.dump(2).c_str());
                    }
                    if (!keep)
                        removedMissing = name;
                    ImGui::PopID();
                }
            }
            if (!removedMissing.empty()) {
                kast::removePreservedComponent(registry, entity, removedMissing);
                changed = true;
            }
            ImGui::PopItemWidth();

            ImGui::Spacing();
            if (ImGui::Button("Add Component", ImVec2(-FLT_MIN, 0.0f)))
                ImGui::OpenPopup("##addComponent");
            if (std::exchange(addComponentRequested_, false)) { // --add-component-popup: below the button
                ImGui::OpenPopup("##addComponent");
                ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y));
            }
            if (ImGui::BeginPopup("##addComponent")) {
                const char* blocked = componentEditBlocker(doc);
                ImGui::BeginDisabled(blocked != nullptr);
                if (ImGui::Selectable("New Lua Component..."))
                    openNewComponent("");
                ImGui::EndDisabled();
                disabledTooltip(blocked);
                ImGui::Separator();
                bool any = false;
                for (const kast::ComponentType& type : types) {
                    if (kast::hasComponent(registry, entity, type))
                        continue;
                    any = true;
                    if (ImGui::Selectable(type.displayName())) {
                        kast::addComponent(registry, entity, type);
                        changed = true;
                    }
                }
                if (!any)
                    ImGui::TextDisabled("Every component is already added");
                ImGui::EndPopup();
            }
            if (changed)
                markDirty(doc);
        }
    }
    drawNewComponentModal(doc); // outside the selection branch, so --new-component-prompt shows it without one
    ImGui::End();
}

void Panels::openNewComponent(std::string name)
{
    newComponentName_ = std::move(name);
    newComponentError_.clear();
    newComponentRequested_ = true;
}

void Panels::drawNewComponentModal(Document& doc)
{
    if (std::exchange(newComponentRequested_, false))
        ImGui::OpenPopup("New Lua Component");
    if (!ImGui::BeginPopupModal("New Lua Component", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
    const bool enter = ImGui::InputText("Name", &newComponentName_, ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemEdited())
        newComponentError_.clear(); // a failed Create's message stays until the name changes

    const std::string error = !doc.scene.valid(doc.selected) ? "Select an entity"
                              : !newComponentError_.empty()  ? newComponentError_
                                                             : newComponentNameError(doc, newComponentName_);
    if (error.empty())
        ImGui::Text("Will create: %s/%s", kast::kComponentsDir,
                    luaComponentFile(doc.project->root, newComponentName_).filename().string().c_str());
    else
        ImGui::TextColored(kErrorColor, "%s", error.c_str());
    ImGui::Spacing();
    ImGui::BeginDisabled(!error.empty());
    const bool create = ImGui::Button("Create") || (enter && error.empty());
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (create) {
        if (newLuaComponent(doc, doc.selected, newComponentName_, newComponentError_))
            ImGui::CloseCurrentPopup();
        else
            kast::log::error(newComponentError_);
    } else if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void Panels::drawComponents(Document& doc)
{
    if (ImGui::Begin("Components")) {
        const std::vector<kast::ComponentDefinitionStatus>& status = kast::componentDefinitionStatus();
        if (!doc.project)
            ImGui::TextDisabled("No project: built-in components only");
        else if (status.empty())
            ImGui::TextDisabled("No Lua components (components/*.lua)");
        for (size_t i = 0; i < status.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::TextColored(status[i].ok ? kOkColor : kErrorColor, status[i].ok ? "OK" : "Error");
            ImGui::SameLine();
            ImGui::TextWrapped("%s  %s", status[i].name.empty() ? "?" : status[i].name.c_str(), status[i].path.c_str());
            if (!status[i].ok) {
                ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
                ImGui::TextWrapped("%s", status[i].error.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::Separator();
            ImGui::PopID();
        }

        ImGui::Spacing();
        const char* blocked = componentEditBlocker(doc);
        ImGui::BeginDisabled(blocked != nullptr);
        if (ImGui::Button("New from Template", ImVec2(-FLT_MIN, 0.0f))) {
            if (std::string error; !newComponentFromTemplate(doc, error))
                kast::log::error(error);
        }
        ImGui::EndDisabled();
        disabledTooltip(blocked);
    }
    ImGui::End();
}

bool Panels::drawStateMachineComponent(Document& doc, entt::entity entity)
{
    entt::registry& registry = doc.scene.registry();
    kast::StateMachine& component = registry.get<kast::StateMachine>(entity);
    const std::vector<kast::StateMachineDef>& defs = kast::stateMachineDefinitions().valid;
    const auto definition = [&]() -> const kast::StateMachineDef* {
        const auto it = std::ranges::find(defs, component.machine, &kast::StateMachineDef::name);
        return it == defs.end() ? nullptr : &*it;
    };
    bool changed = false;

    // A name with no valid definition is kept as-is (and saved) until another machine is chosen.
    const kast::StateMachineDef* def = definition();
    const std::string machineLabel = component.machine.empty() ? "(none)"
                                     : def                     ? component.machine
                                                               : "(missing) " + component.machine;
    if (ImGui::BeginCombo("Machine", machineLabel.c_str())) {
        if (defs.empty())
            ImGui::TextDisabled("No state machines (%s/*.lua)", kast::kStateMachinesDir);
        for (const kast::StateMachineDef& candidate : defs) {
            if (ImGui::Selectable(candidate.name.c_str(), candidate.name == component.machine) &&
                candidate.name != component.machine) {
                component.machine = candidate.name;
                if (!candidate.states.contains(component.initialState))
                    component.initialState.clear(); // the new machine's initial
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    if (!component.machine.empty() && !definition()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        ImGui::TextWrapped("No valid definition \"%s\": kept as-is", component.machine.c_str());
        ImGui::PopStyleColor();
    }

    def = definition();
    const std::string defaultLabel = def ? std::format("(initial: {})", def->initial) : std::string("(initial)");
    const std::string stateLabel = component.initialState.empty() ? defaultLabel
                                   : def && !def->states.contains(component.initialState)
                                       ? "(missing) " + component.initialState
                                       : component.initialState;
    if (ImGui::BeginCombo("Initial", stateLabel.c_str())) {
        if (!def) {
            ImGui::TextDisabled("Choose a machine first");
        } else {
            if (ImGui::Selectable(defaultLabel.c_str(), component.initialState.empty()) && !component.initialState.empty()) {
                component.initialState.clear();
                changed = true;
            }
            for (const std::string& state : def->stateNames()) {
                if (ImGui::Selectable(state.c_str(), state == component.initialState) && state != component.initialState) {
                    component.initialState = state;
                    changed = true;
                }
            }
        }
        ImGui::EndCombo();
    }

    if (doc.play != PlayState::Stopped) {
        const std::optional<std::string> current = doc.lua.fsm().peek(registry.get<kast::PersistentId>(entity).value);
        ImGui::LabelText("Current", "%s", current ? current->c_str() : "(not running)");
    }
    return changed;
}

namespace {

bool hasExtension(const std::string& name, std::string_view extension); // below, with the other Assets helpers

void collectRmlFiles(const std::vector<AssetEntry>& entries, std::vector<std::string>& out)
{
    for (const AssetEntry& entry : entries) {
        if (entry.folder)
            collectRmlFiles(entry.children, out);
        else if (hasExtension(entry.name, ".rml"))
            out.push_back(entry.path);
    }
}

} // namespace

bool Panels::drawUIDocumentComponent(Document& doc, entt::entity entity)
{
    kast::UIDocument& component = doc.scene.registry().get<kast::UIDocument>(entity);
    if ((doc.project ? doc.project->root : std::filesystem::path()) != assetsRoot_)
        refreshAssets(doc); // the Assets panel hasn't scanned this project yet
    std::vector<std::string> files;
    collectRmlFiles(assets_, files);
    const bool listed = std::ranges::find(files, component.path) != files.end();
    const std::string label = component.path.empty() ? "(none)" : listed ? component.path : "(missing) " + component.path;
    bool changed = false;
    if (ImGui::BeginCombo("Path", label.c_str())) {
        if (files.empty())
            ImGui::TextDisabled("No .rml files in the project (Assets > Create > GUI)");
        for (const std::string& file : files)
            if (ImGui::Selectable(file.c_str(), file == component.path) && file != component.path) {
                component.path = file;
                changed = true;
            }
        ImGui::EndCombo();
    }
    // A path that isn't listed is kept as-is (and saved) until another file is chosen.
    if (!component.path.empty() && !listed) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        ImGui::TextWrapped("No file \"%s\" in the project: kept as-is", component.path.c_str());
        ImGui::PopStyleColor();
    }
    changed = ImGui::Checkbox("Visible", &component.visible) || changed;
    return changed;
}

void Panels::openNewStateMachine(std::string name)
{
    newStateMachineName_ = std::move(name);
    newStateMachineError_.clear();
    newStateMachineRequested_ = true;
}

void Panels::drawNewStateMachineModal(Document& doc)
{
    if (std::exchange(newStateMachineRequested_, false))
        ImGui::OpenPopup("New State Machine");
    if (!ImGui::BeginPopupModal("New State Machine", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
    const bool enter = ImGui::InputText("Name", &newStateMachineName_, ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemEdited())
        newStateMachineError_.clear(); // a failed Create's message stays until the name changes

    const std::string error =
        !newStateMachineError_.empty() ? newStateMachineError_ : newStateMachineNameError(doc, newStateMachineName_);
    if (error.empty())
        ImGui::Text("Will create: %s/%s", kast::kStateMachinesDir,
                    stateMachineFile(doc.project->root, newStateMachineName_).filename().string().c_str());
    else
        ImGui::TextColored(kErrorColor, "%s", error.c_str());
    ImGui::Spacing();
    ImGui::BeginDisabled(!error.empty());
    const bool create = ImGui::Button("Create") || (enter && error.empty());
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (create) {
        if (newStateMachine(doc, newStateMachineName_, newStateMachineError_))
            ImGui::CloseCurrentPopup();
        else
            kast::log::error(newStateMachineError_);
    } else if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void Panels::drawStateMachines(Document& doc)
{
    if (ImGui::Begin("State Machines")) {
        const std::vector<kast::ComponentDefinitionStatus>& status = kast::stateMachineDefinitions().status;
        const char* blocked = stateMachineEditBlocker(doc);
        const bool playing = doc.play != PlayState::Stopped;
        if (!doc.project)
            ImGui::TextDisabled("No project: state machines live in a project's %s/", kast::kStateMachinesDir);
        else if (status.empty())
            ImGui::TextDisabled("No state machines (%s/*.lua)", kast::kStateMachinesDir);
        for (size_t i = 0; i < status.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::TextColored(status[i].ok ? kOkColor : kErrorColor, status[i].ok ? "OK" : "Error");
            ImGui::SameLine();
            ImGui::TextWrapped("%s  %s", status[i].name.empty() ? "?" : status[i].name.c_str(), status[i].path.c_str());
            if (status[i].ok) {
                bool global = isGlobalStateMachine(doc, status[i].name);
                ImGui::BeginDisabled(blocked != nullptr);
                if (ImGui::Checkbox("Global", &global)) {
                    if (std::string error; !setGlobalStateMachine(doc, status[i].name, global, error))
                        kast::log::error(error);
                }
                ImGui::EndDisabled();
                disabledTooltip(blocked);
                if (playing && global) {
                    const std::optional<std::string> current = doc.lua.fsm().peek(status[i].name);
                    ImGui::SameLine();
                    ImGui::Text("State: %s", current ? current->c_str() : "(unavailable)");
                }
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
                ImGui::TextWrapped("%s", status[i].error.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::Separator();
            ImGui::PopID();
        }
        // Game machines project.kast lists that have no valid definition: unavailable in Play.
        if (doc.project && doc.project->stateMachines) {
            const std::vector<std::string> listed = *doc.project->stateMachines; // a copy: Remove edits it
            for (const std::string& name : listed) {
                if (std::ranges::any_of(status, [&](const auto& s) { return s.ok && s.name == name; }))
                    continue;
                ImGui::PushID(("listed/" + name).c_str());
                ImGui::TextColored(kErrorColor, "Global \"%s\": no valid definition", name.c_str());
                ImGui::SameLine();
                ImGui::BeginDisabled(blocked != nullptr);
                if (ImGui::SmallButton("Remove")) {
                    if (std::string error; !setGlobalStateMachine(doc, name, false, error))
                        kast::log::error(error);
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
        }

        ImGui::Spacing();
        ImGui::BeginDisabled(blocked != nullptr);
        if (ImGui::Button("New State Machine...", ImVec2(-FLT_MIN, 0.0f)))
            openNewStateMachine("");
        ImGui::EndDisabled();
        disabledTooltip(blocked);
    }
    drawNewStateMachineModal(doc); // outside the Begin branch, so --new-state-machine-prompt shows it on a hidden tab
    ImGui::End();
}

void Panels::onScriptChosen(void* userdata, const char* const* files, int /*filter*/)
{
    auto* self = static_cast<Panels*>(userdata);
    std::lock_guard lock(self->scriptMutex_);
    self->scriptChosen_ = true;
    self->scriptPath_ = files && files[0] ? files[0] : "";
    if (!files)
        kast::log::error(std::format("File dialog failed: {}", SDL_GetError()));
}

void Panels::drawSystems(Document& doc)
{
    const bool editable = canEditSystems(doc);
    {
        std::lock_guard lock(scriptMutex_);
        if (std::exchange(scriptChosen_, false)) {
            scriptDialogOpen_ = false;
            if (!scriptPath_.empty() && editable && !addSystem(doc, scriptPath_, systemsError_)) {
                kast::log::error(systemsError_);
                systemsErrorRequested_ = true; // Play may have started while the dialog was open: then nothing is added
            }
        }
    }

    if (ImGui::Begin("Systems")) {
        const std::vector<kast::LuaSystemStatus> status = doc.lua.status(); // empty unless playing
        const std::vector<std::string>& systems = doc.scene.systems;
        enum class Op { None, Up, Down, Remove } op = Op::None;
        size_t target = 0;
        if (systems.empty())
            ImGui::TextDisabled("No Lua systems");
        for (size_t i = 0; i < systems.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (i < status.size()) {
                ImGui::TextColored(status[i].ok ? kOkColor : kErrorColor, status[i].ok ? "OK" : "Disabled");
                ImGui::SameLine();
            }
            ImGui::TextWrapped("%s", systems[i].c_str());
            if (i < status.size() && !status[i].ok) {
                ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
                ImGui::TextWrapped("%s", status[i].error.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::BeginDisabled(!editable);
            if (ImGui::ArrowButton("##up", ImGuiDir_Up))
                op = Op::Up, target = i;
            ImGui::SameLine();
            if (ImGui::ArrowButton("##down", ImGuiDir_Down))
                op = Op::Down, target = i;
            ImGui::SameLine();
            if (ImGui::Button("Remove"))
                op = Op::Remove, target = i;
            ImGui::EndDisabled();
            ImGui::Separator();
            ImGui::PopID();
        }

        ImGui::Spacing();
        const char* addBlocked = !editable      ? "Stop Play to edit systems"
                                 : !doc.project ? "Open a scene inside a project"
                                                : nullptr;
        const char* templateBlocked = addBlocked ? addBlocked : doc.path.empty() ? "Save the scene first" : nullptr;
        ImGui::BeginDisabled(addBlocked != nullptr || scriptDialogOpen_);
        if (ImGui::Button("Add Script...", ImVec2(-FLT_MIN, 0.0f))) {
            scriptDialogOpen_ = true;
            const std::string dir = doc.project->root.string();
            SDL_ShowOpenFileDialog(onScriptChosen, this, nullptr, &kLuaFilter, 1, dir.c_str(), false);
        }
        ImGui::EndDisabled();
        disabledTooltip(addBlocked);
        ImGui::BeginDisabled(templateBlocked != nullptr);
        if (ImGui::Button("New from Template", ImVec2(-FLT_MIN, 0.0f))) {
            if (!newSystemFromTemplate(doc, systemsError_)) {
                kast::log::error(systemsError_);
                systemsErrorRequested_ = true;
            }
        }
        ImGui::EndDisabled();
        disabledTooltip(templateBlocked);

        if (std::exchange(systemsErrorRequested_, false))
            ImGui::OpenPopup("Systems Error");
        if (ImGui::BeginPopupModal("Systems Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(systemsError_.c_str());
            ImGui::Spacing();
            if (ImGui::Button("OK") || ImGui::IsKeyPressed(ImGuiKey_Escape))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        switch (op) {
        case Op::Up: moveSystem(doc, target, -1); break;
        case Op::Down: moveSystem(doc, target, 1); break;
        case Op::Remove: removeSystem(doc, target); break;
        case Op::None: break;
        }
    }
    ImGui::End();
}

void Panels::drawEvents(Document& doc)
{
    if (ImGui::Begin("Events")) {
        kast::EventBus& bus = doc.lua.bus();
        if (ImGui::Button("Clear"))
            bus.clearHistory();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##filter", "Filter by event name", &eventsFilter_);
        constexpr ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                          ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings;
        if (ImGui::BeginTable("events", 4, flags)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            const float charWidth = ImGui::CalcTextSize("0").x; // starting widths; columns are resizable
            ImGui::TableSetupColumn("Frame", ImGuiTableColumnFlags_WidthFixed, charWidth * 5);
            ImGui::TableSetupColumn("Event", ImGuiTableColumnFlags_WidthFixed, charWidth * 10);
            ImGui::TableSetupColumn("Payload", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Publisher", ImGuiTableColumnFlags_WidthFixed, charWidth * 12);
            ImGui::TableHeadersRow();
            // ponytail: draws every matching row (at most EventBus::kHistorySize); clip if the history grows.
            for (const kast::EventBus::Delivered& event : bus.history()) {
                if (event.name.find(eventsFilter_) == std::string::npos)
                    continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(event.frame));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(event.name.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(event.payload.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(event.publisher.c_str());
            }
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) // follow new events while at the bottom
                ImGui::SetScrollHereY(1.0f);
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

namespace {

constexpr const char* kAssetPayload = "KAST_ASSET"; // a project-relative path, null-terminated

std::string parentAsset(const std::string& path)
{
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? "" : path.substr(0, slash);
}

std::string joinAsset(const std::string& folder, const std::string& name)
{
    return folder.empty() ? name : folder + '/' + name;
}

// name ends in extension (".scene"), ignoring case.
bool hasExtension(const std::string& name, std::string_view extension)
{
    std::string actual = utf8(fromUtf8(name).extension());
    std::ranges::transform(actual, actual.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return actual == extension;
}

// Files the external editor opens (File > Preferences...).
bool isTextAsset(const std::string& name)
{
    return hasExtension(name, ".lua") || hasExtension(name, ".rcss") || hasExtension(name, ".rml") ||
           hasExtension(name, ".json");
}

bool acceptAssetDrop(std::string& dropped)
{
    bool accepted = false;
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload)) {
            dropped = static_cast<const char*>(payload->Data);
            accepted = true;
        }
        ImGui::EndDragDropTarget();
    }
    return accepted;
}

// file as the narrow string doc.path and openFile use; nullopt + error if the code page can't hold its characters.
std::optional<std::string> narrowPath(const std::filesystem::path& file, std::string& error)
{
    try {
        return file.string();
    } catch (const std::system_error&) {
        error = std::format("'{}' can't be used as a scene path: it has characters outside the system code page",
                            utf8(file));
        return std::nullopt;
    }
}

// Every state machine definition file's name (even one in error), after a rescan.
std::vector<std::string> stateMachineNames(Document& doc)
{
    refreshStateMachines(doc);
    std::vector<std::string> names;
    for (const kast::ComponentDefinitionStatus& status : kast::stateMachineDefinitions().status)
        names.push_back(status.name);
    return names;
}

// The open scene as an absolute path, or empty if it was never saved.
std::filesystem::path openScenePath(const Document& doc)
{
    return doc.path.empty() ? std::filesystem::path() : std::filesystem::absolute(doc.path);
}

} // namespace

void Panels::refreshAssets(const Document& doc)
{
    assetsRoot_ = doc.project ? doc.project->root : std::filesystem::path();
    assets_ = assetsRoot_.empty() ? std::vector<AssetEntry>{} : scanAssets(assetsRoot_);
    if (!findAsset(assets_, assetSelected_))
        assetSelected_.clear();
    if (!findAsset(assets_, assetRenaming_))
        assetRenaming_.clear();
}

void Panels::drawAssets(Document& doc, FileActions& files, GuiBuilder& gui, ExternalEditor& editor)
{
    if ((doc.project ? doc.project->root : std::filesystem::path()) != assetsRoot_)
        refreshAssets(doc);
    if (ImGui::Begin("Assets")) {
        if (!doc.project) {
            ImGui::TextDisabled("No project: open a project to browse its assets");
        } else {
            const char* blocker = doc.play != PlayState::Stopped ? "Stop Play mode to change assets"
                                  : !files.idle()                ? "Close the open dialog first"
                                                                 : nullptr;
            for (const AssetEntry& entry : assets_)
                drawAssetNode(doc, files, gui, editor, entry, blocker);

            // The rest of the panel: click clears selection, drop moves to the root, right-click creates at the root.
            ImVec2 rest = ImGui::GetContentRegionAvail();
            rest.x = bx::max(rest.x, 1.0f);
            rest.y = bx::max(rest.y, ImGui::GetFrameHeight());
            ImGui::InvisibleButton("##empty", rest);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
                assetSelected_.clear();
            if (std::string dropped; acceptAssetDrop(dropped))
                assetEdit_ = {AssetEdit::Op::Move, dropped, ""};
            if (assetMenuRequest_ == "")
                openRequestedAssetMenu("##rootMenu");
            if (ImGui::BeginPopupContextItem("##rootMenu")) {
                if (ImGui::MenuItem("New Folder", nullptr, false, !blocker))
                    assetEdit_ = {AssetEdit::Op::NewFolder, ""};
                disabledTooltip(blocker);
                drawAssetCreateMenu("", blocker);
                ImGui::EndPopup();
            }

            const AssetEntry* selected = findAsset(assets_, assetSelected_);
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput &&
                selected && !blocker) {
                if (ImGui::IsKeyPressed(ImGuiKey_F2, false))
                    startAssetRename(selected->path, selected->name);
                if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
                    requestAssetDelete(doc, selected->path);
            }

            if (std::exchange(assetDeleteRequested_, false))
                ImGui::OpenPopup("Delete Asset");
            if (ImGui::BeginPopupModal("Delete Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                const AssetEntry* target = findAsset(assets_, assetDeletePath_);
                if (target) {
                    ImGui::Text("Delete '%s'%s?", target->name.c_str(), target->folder ? " and all its contents" : "");
                    ImGui::TextUnformatted("It will be moved to the Recycle Bin.");
                } else {
                    ImGui::TextUnformatted("It no longer exists.");
                }
                ImGui::Spacing();
                ImGui::BeginDisabled(!target);
                if (ImGui::Button("Delete")) {
                    assetEdit_ = {AssetEdit::Op::Delete, assetDeletePath_};
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }

            if (std::exchange(assetsErrorRequested_, false))
                ImGui::OpenPopup("Assets Error");
            if (ImGui::BeginPopupModal("Assets Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted(assetsError_.c_str());
                ImGui::Spacing();
                if (ImGui::Button("OK") || ImGui::IsKeyPressed(ImGuiKey_Escape))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        }
    }
    ImGui::End();
    applyAssetEdit(doc, gui);
}

void Panels::drawAssetNode(const Document& doc, FileActions& files, GuiBuilder& gui, ExternalEditor& editor,
                           const AssetEntry& entry, const char* blocker)
{
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    if (!entry.folder)
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (assetSelected_ == entry.path)
        flags |= ImGuiTreeNodeFlags_Selected;

    ImGui::PushID(entry.path.c_str()); // keyed on the path, so open folders stay open across rescans
    if (entry.folder && entry.path == assetExpand_) {
        ImGui::SetNextItemOpen(true);
        assetExpand_.clear();
    }
    const bool renaming = assetRenaming_ == entry.path;
    const bool open = ImGui::TreeNodeEx("##node", flags, "%s", renaming ? "" : entry.name.c_str());

    if ((ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) &&
        !ImGui::IsItemToggledOpen())
        assetSelected_ = entry.path;
    if (!entry.folder && hasExtension(entry.name, ".rml") && ImGui::IsItemHovered() &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        gui.open(assetsRoot_ / fromUtf8(entry.path));
    if (!entry.folder && isTextAsset(entry.name) && !hasExtension(entry.name, ".rml") && ImGui::IsItemHovered() &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) // .rml: the GUI Builder above; its menu opens it here
        editor.open(assetsRoot_ / fromUtf8(entry.path));
    if (!entry.folder && !blocker && hasExtension(entry.name, ".scene") && ImGui::IsItemHovered() &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        std::string error;
        if (const std::optional<std::string> file = narrowPath(assetsRoot_ / fromUtf8(entry.path), error))
            files.openScene(*file);
        else
            assetsFailed(error);
    }
    if (!blocker && !renaming && ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(kAssetPayload, entry.path.c_str(), entry.path.size() + 1);
        ImGui::TextUnformatted(entry.name.c_str());
        ImGui::EndDragDropSource();
    }
    if (std::string dropped; entry.folder && acceptAssetDrop(dropped))
        assetEdit_ = {AssetEdit::Op::Move, dropped, entry.path};
    if (assetMenuRequest_ == entry.path)
        openRequestedAssetMenu("##assetMenu");
    if (ImGui::BeginPopupContextItem("##assetMenu")) {
        drawAssetItems(doc, editor, entry, blocker);
        ImGui::EndPopup();
    }

    if (renaming) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (std::exchange(focusAssetRename_, false))
            ImGui::SetKeyboardFocusHere();
        bool commit = ImGui::InputText("##rename", &assetRenameBuffer_,
                                       ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        bool cancel = false;
        if (ImGui::IsItemDeactivated()) {
            cancel = ImGui::IsKeyPressed(ImGuiKey_Escape);
            commit = !cancel;
        }
        if (commit && assetRenameBuffer_ != entry.name)
            assetEdit_ = {AssetEdit::Op::Rename, entry.path, assetRenameBuffer_};
        else if ((commit || cancel) && assetNewFsm_ == entry.path)
            assetNewFsm_.clear(); // kept its default name: later renames are plain
        if (commit || cancel)
            assetRenaming_.clear();
    }

    if (open && entry.folder) {
        for (const AssetEntry& child : entry.children)
            drawAssetNode(doc, files, gui, editor, child, blocker);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void Panels::drawAssetItems(const Document& doc, ExternalEditor& editor, const AssetEntry& entry, const char* blocker)
{
    if (!entry.folder && isTextAsset(entry.name)) {
        if (ImGui::MenuItem("Open in External Editor"))
            editor.open(assetsRoot_ / fromUtf8(entry.path));
        ImGui::Separator();
    }
    if (entry.folder) {
        if (ImGui::MenuItem("New Folder", nullptr, false, !blocker))
            assetEdit_ = {AssetEdit::Op::NewFolder, entry.path};
        disabledTooltip(blocker);
        drawAssetCreateMenu(entry.path, blocker);
        ImGui::Separator();
    }
    if (ImGui::MenuItem("Rename", "F2", false, !blocker))
        startAssetRename(entry.path, entry.name);
    disabledTooltip(blocker);
    if (ImGui::MenuItem("Delete", "Del", false, !blocker))
        requestAssetDelete(doc, entry.path);
    disabledTooltip(blocker);
}

void Panels::drawAssetCreateMenu(const std::string& folder, const char* blocker)
{
    if (std::exchange(assetCreateMenuRequested_, false))
        ImGui::OpenPopup("Create"); // BeginMenu's popup id
    if (!ImGui::BeginMenu("Create"))
        return;
    if (ImGui::MenuItem("Lua System", nullptr, false, !blocker))
        assetEdit_ = {AssetEdit::Op::NewSystem, folder};
    disabledTooltip(blocker);
    const char* fsmBlocker = blocker ? blocker
                             : !isInStateMachinesDir(assetsRoot_, assetsRoot_ / fromUtf8(folder))
                                 ? "State machines must be inside fsms/"
                                 : nullptr;
    if (ImGui::MenuItem("State Machine", nullptr, false, !fsmBlocker))
        assetEdit_ = {AssetEdit::Op::NewStateMachine, folder};
    disabledTooltip(fsmBlocker);
    if (ImGui::MenuItem("GUI", nullptr, false, !blocker))
        assetEdit_ = {AssetEdit::Op::NewGui, folder};
    disabledTooltip(blocker);
    ImGui::EndMenu();
}

void Panels::openRequestedAssetMenu(const char* popup)
{
    assetMenuRequest_.reset();
    assetCreateMenuRequested_ = true;
    ImGui::OpenPopup(popup);
    // Grows up from the item's top-left (kept inside the window) rather than from the (absent) mouse.
    const float bottom = ImGui::GetWindowPos().y + ImGui::GetWindowHeight();
    ImGui::SetNextWindowPos({ImGui::GetItemRectMin().x + ImGui::GetFontSize() * 4.0f,
                             bx::min(ImGui::GetItemRectMin().y, bottom)},
                            ImGuiCond_Always, {0.0f, 1.0f});
}

void Panels::startAssetRename(const std::string& path, const std::string& name)
{
    if (path != assetNewFsm_)
        assetNewFsm_.clear(); // its first rename has ended without renaming it
    assetSelected_ = path;
    assetRenaming_ = path;
    assetRenameBuffer_ = name;
    focusAssetRename_ = true;
}

void Panels::requestAssetDelete(const Document& doc, const std::string& path)
{
    if (std::string error = deleteBlocker(assetsRoot_ / fromUtf8(path), openScenePath(doc)); !error.empty()) {
        assetsFailed(std::move(error));
        return;
    }
    assetDeletePath_ = path;
    assetDeleteRequested_ = true;
}

void Panels::assetsFailed(std::string error)
{
    assetsError_ = std::move(error);
    kast::log::error(assetsError_);
    assetsErrorRequested_ = true;
}

void Panels::applyAssetEdit(Document& doc, GuiBuilder& gui)
{
    const AssetEdit edit = std::exchange(assetEdit_, AssetEdit{});
    if (edit.op == AssetEdit::Op::None || !doc.project || doc.play != PlayState::Stopped)
        return;
    const std::filesystem::path& root = doc.project->root;
    const std::filesystem::path source = root / fromUtf8(edit.path);
    std::string error;
    std::string selected; // the entry to select afterwards
    std::filesystem::path dest; // Rename / Move: where source goes

    switch (edit.op) {
    case AssetEdit::Op::NewFolder: {
        const std::filesystem::path dir = edit.path.empty() ? root : source;
        const std::string name = uniqueFolderName(dir);
        if (createAssetFolder(dir, name, error)) {
            assetExpand_ = edit.path;
            selected = joinAsset(edit.path, name);
            kast::log::info(std::format("Created folder '{}'", selected));
            refreshAssets(doc);
            startAssetRename(selected, name);
            return;
        }
        break;
    }
    case AssetEdit::Op::NewSystem:
    case AssetEdit::Op::NewStateMachine: {
        const bool fsm = edit.op == AssetEdit::Op::NewStateMachine;
        const std::filesystem::path dir = edit.path.empty() ? root : source;
        std::filesystem::path file;
        if (fsm && !isInStateMachinesDir(root, dir))
            error = "State machines must be inside fsms/";
        else if (fsm ? writeNewStateMachine(dir, stateMachineNames(doc), file, error) : writeNewSystem(dir, file, error)) {
            const std::string name = utf8(file.filename());
            assetExpand_ = edit.path;
            selected = joinAsset(edit.path, name);
            kast::log::info(std::format("Created {} '{}'", fsm ? "state machine" : "Lua system", selected));
            if (fsm)
                refreshStateMachines(doc); // loads it now rather than at the next poll
            else if (doc.path.empty())
                kast::log::info(std::format("'{}' was not added to the scene's systems: the scene is unsaved", selected));
            else
                addSystem(doc, file, error); // ponytail: can't fail, the project is open and file is inside it
            refreshAssets(doc);
            startAssetRename(selected, name);
            if (fsm)
                assetNewFsm_ = selected;
            if (!error.empty())
                assetsFailed(std::move(error));
            return;
        }
        break;
    }
    case AssetEdit::Op::NewGui: {
        const std::filesystem::path dir = edit.path.empty() ? root : source;
        std::filesystem::path file;
        if (writeNewGui(dir, file, error)) {
            const std::string name = utf8(file.filename());
            assetExpand_ = edit.path;
            selected = joinAsset(edit.path, name);
            kast::log::info(std::format("Created GUI '{}' and its .rcss", selected));
            refreshAssets(doc);
            startAssetRename(selected, name);
            return;
        }
        break;
    }
    case AssetEdit::Op::Delete:
        error = deleteBlocker(source, openScenePath(doc)); // the scene may have changed since the prompt opened
        if (error.empty() && recycleAsset(source, error))
            kast::log::info(std::format("Moved '{}' to the Recycle Bin", edit.path));
        break;
    case AssetEdit::Op::Rename:
        dest = source.parent_path() / fromUtf8(edit.target);
        selected = joinAsset(parentAsset(edit.path), edit.target);
        break;
    case AssetEdit::Op::Move:
        if (parentAsset(edit.path) == edit.target)
            return; // dropped onto its own folder
        dest = (edit.target.empty() ? root : root / fromUtf8(edit.target)) / source.filename();
        selected = joinAsset(edit.target, utf8(source.filename()));
        break;
    case AssetEdit::Op::None:
        break;
    }

    if (!dest.empty()) {
        // The open scene follows its file: work out its new path first, so a path doc.path can't hold refuses the
        // operation before anything changes on disk.
        const std::filesystem::path scene = openScenePath(doc);
        const std::optional<std::filesystem::path> movedScene = scene.empty() ? std::nullopt : remapPath(scene, source, dest);
        std::optional<std::string> scenePath = movedScene ? narrowPath(*movedScene, error) : std::nullopt;
        // Kept after a refusal, so retrying is still the first rename.
        const bool newFsm = edit.op == AssetEdit::Op::Rename && edit.path == assetNewFsm_;
        const bool ok = (!movedScene || scenePath) &&
                        (newFsm ? renameNewStateMachine(source, edit.target, stateMachineNames(doc), error)
                         : edit.op == AssetEdit::Op::Rename && hasExtension(edit.path, ".rml")
                             ? renameGui(source, edit.target, error) // its linked .rcss follows
                         : edit.op == AssetEdit::Op::Rename ? renameAsset(source, edit.target, error)
                                                            : moveAsset(source, dest.parent_path(), error));
        if (ok) {
            kast::log::info(std::format("{} '{}' to '{}'", edit.op == AssetEdit::Op::Rename ? "Renamed" : "Moved",
                                        edit.path, selected));
            assetSelected_ = selected;
            gui.remap(source, dest);
            if (scenePath)
                doc.path = std::move(*scenePath); // same project; the title follows, the dirty flag stays
            if (remapProjectPaths(doc.scene.systems, edit.path, selected))
                markDirty(doc);
            if (newFsm) {
                assetNewFsm_.clear();
                refreshStateMachines(doc);
            }
        }
    }
    if (!error.empty())
        assetsFailed(std::move(error));
    refreshAssets(doc);
}
