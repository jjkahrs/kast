#include "panels.h"

#include <components.h>
#include <log.h>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_stdlib.h>

#include <cstring>
#include <format>

namespace {

constexpr const char* kEntityPayload = "KAST_ENTITY";
constexpr SDL_DialogFileFilter kLuaFilter{"Lua script (*.lua)", "lua"};
constexpr ImVec4 kOkColor{0.4f, 0.85f, 0.4f, 1.0f};
constexpr ImVec4 kErrorColor{1.0f, 0.45f, 0.4f, 1.0f};

// Explains why the last (disabled) item is disabled, if it is.
void disabledTooltip(const char* reason)
{
    if (reason && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", reason);
}

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

bool Panels::drawField(entt::entity entity, entt::meta_any& component, const entt::meta_data& field)
{
    const auto* info = static_cast<const kast::FieldInfo*>(field.custom());
    const char* label = info ? info->displayName : field.name();
    const entt::meta_type type = field.type();
    entt::meta_any value = field.get(component);
    bool changed = false;
    if (kast::isType<float>(type)) {
        changed = ImGui::DragFloat(label, &value.cast<float&>(), 0.05f);
    } else if (kast::isType<int>(type)) {
        changed = ImGui::DragInt(label, &value.cast<int&>());
    } else if (kast::isType<bool>(type)) {
        changed = ImGui::Checkbox(label, &value.cast<bool&>());
    } else if (kast::isType<std::string>(type)) {
        changed = ImGui::InputText(label, &value.cast<std::string&>());
    } else if (kast::isType<bx::Vec3>(type)) {
        float* v = &value.cast<bx::Vec3&>().x;
        changed = info && info->color ? ImGui::ColorEdit3(label, v) : ImGui::DragFloat3(label, v, 0.05f);
    } else if (kast::isType<bx::Quaternion>(type)) {
        // Edited as Euler degrees, cached while unchanged so they don't jump between equivalent forms.
        auto& rotation = value.cast<bx::Quaternion&>();
        if (eulerEntity_ != entity || eulerField_ != field.name() || !sameBits(eulerSource_, rotation)) {
            eulerEntity_ = entity;
            eulerField_ = field.name();
            eulerSource_ = rotation;
            euler_ = kast::eulerDegFromQuat(rotation);
        }
        if (ImGui::DragFloat3(label, &euler_.x, 0.5f)) {
            rotation = kast::quatFromEulerDeg(euler_);
            eulerSource_ = rotation;
            changed = true;
        }
    } else if (type.is_enum()) {
        const char* preview = "";
        for (auto [id, constant] : type.data())
            if (constant.get({}) == value)
                preview = static_cast<const kast::FieldInfo&>(constant.custom()).displayName;
        if (ImGui::BeginCombo(label, preview)) {
            for (auto [id, constant] : type.data()) {
                entt::meta_any option = constant.get({});
                if (ImGui::Selectable(static_cast<const kast::FieldInfo&>(constant.custom()).displayName, option == value)) {
                    value = std::move(option);
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
    } else {
        ImGui::TextDisabled("%s: unsupported field type", label);
    }
    return changed && field.set(component, value);
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

            entt::meta_type removed;
            for (const entt::meta_type& type : kast::componentTypes()) {
                if (!kast::hasComponent(registry, entity, type))
                    continue;
                const kast::ComponentInfo& info = kast::componentInfo(type);
                ImGui::PushID(type.name()); // field labels repeat across components
                bool keep = true;           // the header's x button (removable types only) clears it
                if (ImGui::CollapsingHeader(info.displayName, info.removable ? &keep : nullptr,
                                            ImGuiTreeNodeFlags_DefaultOpen)) {
                    entt::meta_any component = kast::getComponent(registry, entity, type);
                    const entt::meta_any before = component; // owning copy: undoes edits validate rejects
                    for (auto [id, field] : type.data()) {
                        if (!drawField(entity, component, field))
                            continue;
                        if (info.validate && info.validate(component))
                            component.assign(before); // keep the last valid value so the scene stays loadable
                        else
                            changed = true;
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
            ImGui::PopItemWidth();

            ImGui::Spacing();
            if (ImGui::Button("Add Component", ImVec2(-FLT_MIN, 0.0f)))
                ImGui::OpenPopup("##addComponent");
            if (ImGui::BeginPopup("##addComponent")) {
                bool any = false;
                for (const entt::meta_type& type : kast::componentTypes()) {
                    if (kast::hasComponent(registry, entity, type))
                        continue;
                    any = true;
                    if (ImGui::Selectable(kast::componentInfo(type).displayName)) {
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
