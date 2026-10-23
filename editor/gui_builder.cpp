#include "gui_builder.h"

#include "assets.h"

#include <app.h>
#include <log.h>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Factory.h>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>

namespace {

constexpr double kRcssApplyDelaySeconds = 0.3;
constexpr const char* kNodePayload = "GUI_NODE";
constexpr ImU32 kOutlineColor = IM_COL32(255, 200, 0, 255);

std::filesystem::file_time_type modified(const std::filesystem::path& file)
{
    std::error_code ec;
    return file.empty() ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(file, ec);
}

bool readText(const std::filesystem::path& file, std::string& out)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return false;
    out.assign(std::istreambuf_iterator<char>(in), {});
    return !in.bad();
}

std::string nodeLabel(pugi::xml_node node)
{
    const GuiKind kind = guiKind(node);
    std::string label = kind == GuiKind::Unknown ? std::format("<{}>", node.name()) : guiKindLabel(kind);
    if (const std::string id = GuiDoc::attribute(GuiDoc::widgetElement(node), "id"); !id.empty())
        label += " #" + id;
    const bool labelled = kind == GuiKind::Toggle && GuiDoc::widgetElement(node) != node;
    if (GuiDoc::hasEditableText(node) || labelled) {
        std::string text = labelled ? GuiDoc::widgetProperty(node, "label") : GuiDoc::text(node);
        if (text.size() > 24)
            text = text.substr(0, 24) + "...";
        label += std::format(" \"{}\"", text);
    }
    return label;
}

bool isPanelOrBody(pugi::xml_node node, pugi::xml_node body) { return node == body || guiKind(node) == GuiKind::Panel; }

void collectElements(Rml::Element* element, std::vector<Rml::Element*>& out)
{
    const int index = element->GetAttribute<int>("data-kast-node", -1);
    if (index >= 0 && static_cast<size_t>(index) < out.size())
        out[static_cast<size_t>(index)] = element;
    for (int i = 0; i < element->GetNumChildren(); ++i)
        collectElements(element->GetChild(i), out);
}

} // namespace

void GuiBuilder::shutdown()
{
    previewDoc_ = nullptr;
    preview_.release();
    target_.destroy();
}

void GuiBuilder::open(const std::filesystem::path& rml)
{
    if (dirty()) {
        pendingOpen_ = rml;
        openPromptRequested_ = true;
        return;
    }
    if (load(rml))
        focusPanels();
}

bool GuiBuilder::load(const std::filesystem::path& rml)
{
    std::string text;
    std::string error = "cannot read it";
    GuiDoc doc;
    if (!readText(rml, text) || !doc.parse(text, error)) {
        kast::log::error(std::format("Cannot open GUI '{}': {}", rml.string(), error));
        return false;
    }
    // A reload frees the old nodes: keep the selection by id where it has one.
    const bool reload = rml == rml_;
    const std::string selectedId = reload && selected_ ? GuiDoc::attribute(selected_, "id") : std::string();
    rml_ = rml;
    doc_.parse(text, error); // parsed fine above
    rcss_ = linkedStylesheet(rml);
    rcssText_.clear();
    if (rcss_.empty() && !doc_.stylesheetHref().empty())
        kast::log::warn(std::format("GUI '{}' links '{}', outside its project: the stylesheet is ignored",
                                    utf8(rml.filename()), doc_.stylesheetHref()));
    else if (!rcss_.empty() && !readText(rcss_, rcssText_))
        kast::log::warn(std::format("GUI '{}' links '{}', which is missing: Save creates it", utf8(rml.filename()),
                                    utf8(rcss_.filename())));
    rmlDirty_ = rcssDirty_ = false;
    rcssEditedAt_ = -1.0;
    if (!reload)
        goodRcss_.clear(); // a reload keeps the last good style if the changed sheet has errors
    applyRcss();
    rememberFileTimes();
    selected_ = {};
    inspectorNode_ = {};
    history_.clear(true);
    gesture_.reset();
    rcssCommitted_ = rcssText_;
    if (!selectedId.empty())
        selectById(selectedId);
    previewStale_ = true;
    kast::log::info(std::format("Opened GUI '{}'", rml.string()));
    return true;
}

void GuiBuilder::close()
{
    rml_.clear();
    rcss_.clear();
    rcssText_.clear();
    goodRcss_.clear();
    rmlDirty_ = rcssDirty_ = false;
    selected_ = {};
    inspectorNode_ = {};
    history_.clear(true);
    gesture_.reset();
    rcssCommitted_.clear();
    previewStale_ = true;
}

bool GuiBuilder::save()
{
    if (rml_.empty())
        return true;
    endGesture(); // the save point comes after a text field edit in progress
    std::string error;
    if (!writeTextFile(rml_, doc_.serialize(), error) || (!rcss_.empty() && !writeTextFile(rcss_, rcssText_, error))) {
        kast::log::error(std::format("Cannot save GUI: {}", error));
        return false;
    }
    rmlDirty_ = rcssDirty_ = false;
    history_.markSaved();
    rememberFileTimes();
    kast::log::info(std::format("Saved GUI '{}'", rml_.string()));
    return true;
}

bool GuiBuilder::applyRcss()
{
    rcssEditedAt_ = -1.0;
    if (!kast::gui::ready())
        return false;
    const uint64_t problems = kast::gui::problemCount();
    const bool parsed = Rml::Factory::InstanceStyleSheetString(rcssText_) != nullptr;
    sheetFailed_ = !parsed || kast::gui::problemCount() != problems;
    if (sheetFailed_) {
        kast::log::error(std::format("GUI Style: '{}' has errors (see above); the preview keeps the last good style",
                                     utf8(rcss_.filename())));
        return false;
    }
    goodRcss_ = rcssText_;
    previewStale_ = true;
    return true;
}

void GuiBuilder::markRmlDirty()
{
    rmlDirty_ = true;
    previewStale_ = true;
}

bool GuiBuilder::record(std::string name, std::optional<GuiElementCopy> before, std::optional<GuiElementCopy> after)
{
    return pushGuiStep(history_, doc_.body().root(), std::move(name), std::move(before), std::move(after),
                       [this](pugi::xml_node node) { selected_ = node; });
}

void GuiBuilder::endGesture()
{
    if (const std::optional<Gesture> gesture = std::exchange(gesture_, std::nullopt))
        if (!record(gesture->name, gesture->before, copyGuiElement(gesture->node)) && history_.atSavePoint())
            rmlDirty_ = false; // the field ended where it started: still the saved document
    if (rcssText_ == rcssCommitted_)
        return;
    const auto setRcss = [this](const std::string& text) {
        rcssText_ = rcssCommitted_ = text;
        applyRcss();
    };
    history_.push({"Edit Style", [setRcss, before = rcssCommitted_] { setRcss(before); },
                   [setRcss, after = rcssText_] { setRcss(after); }});
    rcssCommitted_ = rcssText_;
}

bool GuiBuilder::setAttribute(const std::string& name, const std::string& value)
{
    if (!selected_ || selected_ == doc_.body())
        return false;
    editStep(std::format("Edit {} {}", guiKindLabel(guiKind(selected_)), name),
             [&] { GuiDoc::setAttribute(GuiDoc::widgetElement(selected_), name.c_str(), value); });
    return true;
}

bool GuiBuilder::setWidgetProperty(const std::string& name, const std::string& value)
{
    const std::vector<std::string> names = selected_ ? GuiDoc::widgetProperties(selected_) : std::vector<std::string>();
    if (std::ranges::find(names, name) == names.end())
        return false;
    editStep(std::format("Edit {} {}", guiKindLabel(guiKind(selected_)), name),
             [&] { GuiDoc::setWidgetProperty(selected_, name, value); });
    return true;
}

bool GuiBuilder::editStep(const std::string& name, const std::function<void()>& edit)
{
    endGesture();
    GuiElementCopy before = copyGuiElement(selected_);
    edit();
    if (!record(name, std::move(before), copyGuiElement(selected_)))
        return false;
    inspectorNode_ = {}; // refill the Inspector's fields
    markRmlDirty();
    return true;
}

void GuiBuilder::undo()
{
    endGesture();
    if (history_.canUndo())
        kast::log::info(std::format("Undo {}", history_.undoName()));
    history_.undo();
    afterUndoRedo();
}

void GuiBuilder::redo()
{
    endGesture();
    if (history_.canRedo())
        kast::log::info(std::format("Redo {}", history_.redoName()));
    history_.redo();
    afterUndoRedo();
}

void GuiBuilder::afterUndoRedo()
{
    rmlDirty_ = !history_.atSavePoint(); // the history covers the .rcss too: one flag stands for both
    rcssDirty_ = false;
    inspectorNode_ = {};
    previewStale_ = true;
}

void GuiBuilder::rememberFileTimes()
{
    rmlTime_ = modified(rml_);
    rcssTime_ = modified(rcss_);
}

bool GuiBuilder::selectById(const std::string& id)
{
    std::vector<pugi::xml_node> nodes;
    doc_.previewRml("", nodes);
    const auto it = std::ranges::find_if(nodes, [&](pugi::xml_node node) { return GuiDoc::attribute(node, "id") == id; });
    if (it == nodes.end())
        return false;
    selected_ = GuiDoc::widgetRoot(*it);
    return true;
}

bool GuiBuilder::pick(float x, float y)
{
    if (!previewDoc_ || !preview_.context())
        return false;
    int index = -1;
    for (Rml::Element* element = preview_.context()->GetElementAtPoint({x, y}); element && index < 0;
         element = element->GetParentNode())
        index = element->GetAttribute<int>("data-kast-node", -1);
    selected_ = index >= 0 && static_cast<size_t>(index) < previewNodes_.size()
                    ? GuiDoc::widgetRoot(previewNodes_[static_cast<size_t>(index)])
                    : doc_.body();
    return true;
}

void GuiBuilder::setPreviewSize(int width, int height)
{
    resolutions_.insert(resolutions_.end() - 1, {width, height});
    resolution_ = static_cast<int>(resolutions_.size()) - 2;
}

void GuiBuilder::remap(const std::filesystem::path& from, const std::filesystem::path& to)
{
    if (rml_.empty())
        return;
    if (const auto moved = remapPath(rml_, from, to))
        rml_ = *moved;
    if (const auto moved = rcss_.empty() ? std::nullopt : remapPath(rcss_, from, to))
        rcss_ = *moved;
    // renameGui renamed the sibling .rcss and rewrote the link on disk: follow it, so Save keeps the link.
    std::error_code ec;
    if (const std::filesystem::path linked = linkedStylesheet(rml_);
        !linked.empty() && linked != rcss_ && !rcss_.empty() && !std::filesystem::exists(rcss_, ec)) {
        rcss_ = linked;
        doc_.setStylesheetHref(utf8(linked.filename()));
    }
    rememberFileTimes();
}

void GuiBuilder::poll()
{
    if (rml_.empty())
        return;
    const auto rmlTime = modified(rml_);
    const auto rcssTime = modified(rcss_);
    if (rmlTime == rmlTime_ && rcssTime == rcssTime_)
        return;
    std::error_code ec;
    if (!std::filesystem::exists(rml_, ec)) {
        if (!dirty()) {
            kast::log::info(std::format("GUI '{}' was removed: closed in the builder", rml_.string()));
            close();
        }
        rmlTime_ = rmlTime; // dirty: kept open, Save writes it back
        return;
    }
    rmlTime_ = rmlTime;
    rcssTime_ = rcssTime;
    if (dirty())
        reloadPromptRequested_ = true;
    else
        load(rml_);
}

bool GuiBuilder::onQuitRequested()
{
    if (quitConfirmed_ || !dirty())
        return true;
    quitPromptRequested_ = true;
    return false;
}

std::string GuiBuilder::title(const char* name, const char* window) const
{
    if (rml_.empty())
        return std::format("{}{}", name, window);
    return std::format("{} - {}{}{}", name, utf8(rml_.filename()), dirty() ? "*" : "", window);
}

pugi::xml_node GuiBuilder::addParent() const
{
    return selected_ && guiKind(selected_) == GuiKind::Panel ? selected_ : doc_.body();
}

void GuiBuilder::saveShortcut()
{
    if (!rml_.empty() && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S)) // focused route: wins over the scene's Ctrl+S
        save();
}

void GuiBuilder::drawHierarchy()
{
    if (focusStep_ == 4)
        ImGui::SetNextWindowFocus();
    if (ImGui::Begin(title("GUI Hierarchy", kHierarchyWindow).c_str()) && !rml_.empty()) {
        saveShortcut();
        if (ImGui::Button("Save"))
            save();
        for (GuiKind kind : kGuiCreatableKinds) { // wrapped to the panel's width
            const std::string label = std::format("+ {}", guiKindLabel(kind));
            const float width = ImGui::CalcTextSize(label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width <=
                ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x)
                ImGui::SameLine();
            if (ImGui::Button(label.c_str()))
                edit_ = {Edit::Op::Add, addParent(), {}, kind};
        }
        const bool movable = selected_ && selected_ != doc_.body();
        ImGui::BeginDisabled(!movable);
        if (ImGui::Button("Delete"))
            edit_ = {Edit::Op::Delete, selected_};
        ImGui::SameLine();
        if (ImGui::Button("Up"))
            edit_ = {Edit::Op::Move, selected_, {}, GuiKind::Panel, -1};
        ImGui::SameLine();
        if (ImGui::Button("Down"))
            edit_ = {Edit::Op::Move, selected_, {}, GuiKind::Panel, 1};
        ImGui::EndDisabled();
        ImGui::Separator();
        drawNode(doc_.body());
        applyEdit();
    } else if (rml_.empty()) {
        ImGui::TextDisabled("Double-click a .rml file in Assets to open it.");
    }
    ImGui::End();
}

void GuiBuilder::drawNode(pugi::xml_node node)
{
    const bool body = node == doc_.body();
    const std::vector<pugi::xml_node> children = GuiDoc::composite(node) ? std::vector<pugi::xml_node>() : GuiDoc::elements(node);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    if (children.empty())
        flags |= ImGuiTreeNodeFlags_Leaf;
    if (node == selected_)
        flags |= ImGuiTreeNodeFlags_Selected;
    const std::string label = body ? std::string("Body") : nodeLabel(node);
    const bool unknown = !body && guiKind(node) == GuiKind::Unknown;
    if (unknown)
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(node.internal_object()), flags, "%s", label.c_str());
    if (unknown)
        ImGui::PopStyleColor();
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        selected_ = node;
    if (!body && ImGui::BeginDragDropSource()) {
        pugi::xml_node_struct* payload = node.internal_object();
        ImGui::SetDragDropPayload(kNodePayload, &payload, sizeof(payload));
        ImGui::TextUnformatted(label.c_str());
        ImGui::EndDragDropSource();
    }
    if (isPanelOrBody(node, doc_.body()) && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kNodePayload))
            edit_ = {Edit::Op::Reparent, pugi::xml_node(*static_cast<pugi::xml_node_struct* const*>(payload->Data)), node};
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem()) {
        selected_ = node;
        if (isPanelOrBody(node, doc_.body()))
            for (GuiKind kind : kGuiCreatableKinds)
                if (ImGui::MenuItem(std::format("Add {}", guiKindLabel(kind)).c_str()))
                    edit_ = {Edit::Op::Add, node, {}, kind};
        if (!body) {
            if (ImGui::MenuItem("Move Up"))
                edit_ = {Edit::Op::Move, node, {}, GuiKind::Panel, -1};
            if (ImGui::MenuItem("Move Down"))
                edit_ = {Edit::Op::Move, node, {}, GuiKind::Panel, 1};
            if (ImGui::MenuItem("Delete"))
                edit_ = {Edit::Op::Delete, node};
        }
        ImGui::EndPopup();
    }
    if (open) {
        for (pugi::xml_node child : children)
            drawNode(child);
        ImGui::TreePop();
    }
}

void GuiBuilder::applyEdit()
{
    const Edit edit = std::exchange(edit_, Edit{});
    if (edit.op == Edit::Op::None)
        return;
    endGesture();
    const std::string kind = guiKindLabel(guiKind(edit.node));
    std::optional<GuiElementCopy> before = edit.op == Edit::Op::Add ? std::nullopt : std::optional(copyGuiElement(edit.node));
    std::optional<GuiElementCopy> after;
    std::string name;
    switch (edit.op) {
    case Edit::Op::Add:
        if (pugi::xml_node added = doc_.add(edit.kind, edit.node)) {
            selected_ = added;
            after = copyGuiElement(added);
            name = std::format("Add {}", guiKindLabel(edit.kind));
        }
        break;
    case Edit::Op::Delete:
        for (pugi::xml_node node = selected_; node; node = node.parent())
            if (node == edit.node)
                selected_ = {}; // going with its subtree
        if (!doc_.remove(edit.node))
            before.reset();
        name = "Delete " + kind;
        break;
    case Edit::Op::Move:
        if (doc_.move(edit.node, edit.delta))
            after = copyGuiElement(edit.node);
        name = std::format("Move {} {}", kind, edit.delta < 0 ? "Up" : "Down");
        break;
    case Edit::Op::Reparent:
        if (doc_.reparent(edit.node, edit.parent))
            after = copyGuiElement(edit.node);
        else
            kast::log::warn("GUI Hierarchy: can't move an element into itself");
        name = "Reparent " + kind;
        break;
    case Edit::Op::None:
        break;
    }
    // Delete has no after; the others have one only if they happened.
    const bool changed = (edit.op == Edit::Op::Delete ? before.has_value() : after.has_value()) &&
                         record(name, std::move(before), std::move(after));
    if (changed) {
        inspectorNode_ = {}; // an edit may free nodes; refill the Inspector from the selection
        markRmlDirty();
    }
}

void GuiBuilder::drawInspector()
{
    if (focusStep_ == 3)
        ImGui::SetNextWindowFocus();
    if (ImGui::Begin(title("GUI Inspector", kInspectorWindow).c_str()) && !rml_.empty()) {
        saveShortcut();
        if (!selected_) {
            ImGui::TextDisabled("Select an element in the GUI Hierarchy or Preview.");
        } else if (selected_ == doc_.body()) {
            ImGui::TextDisabled("Body: the document root. Add elements to it from the GUI Hierarchy.");
        } else {
            const GuiKind kind = guiKind(selected_);
            // A labelled Toggle's id, class, data-event and bindings are its <input>'s.
            const pugi::xml_node element = GuiDoc::widgetElement(selected_);
            if (inspectorNode_ != selected_) {
                inspectorNode_ = selected_;
                inspectorBuffer_[0] = GuiDoc::attribute(element, "id");
                inspectorBuffer_[1] = GuiDoc::attribute(element, "class");
                inspectorBuffer_[2] = GuiDoc::text(selected_);
                inspectorBuffer_[3] = GuiDoc::attribute(element, "data-event");
                bindingBuffers_.clear();
                for (pugi::xml_attribute attribute : element.attributes())
                    if (guiBindingAttribute(attribute.name()))
                        bindingBuffers_[attribute.name()] = attribute.value();
                widgetBuffers_.clear();
                for (const std::string& name : GuiDoc::widgetProperties(selected_))
                    widgetBuffers_.emplace_back(name, GuiDoc::widgetProperty(selected_, name));
                optionBuffers_.clear();
                for (pugi::xml_node option : GuiDoc::options(selected_))
                    optionBuffers_.emplace_back(GuiDoc::attribute(option, "value"), GuiDoc::text(option));
            }
            ImGui::Text("%s  <%s>", guiKindLabel(kind), selected_.name());
            // One step per field edit: the element is copied before its first change, recorded when it lets go.
            const auto field = [&](const char* label, std::string& buffer, auto set) {
                const bool changed = ImGui::InputText(label, &buffer);
                const std::string_view name = std::string_view(label).substr(0, std::string_view(label).find("##"));
                if (ImGui::IsItemActive() && !gesture_)
                    gesture_ = Gesture{std::format("Edit {} {}", guiKindLabel(kind), name), selected_,
                                       copyGuiElement(selected_)};
                if (changed) {
                    set(buffer);
                    markRmlDirty();
                }
                if (ImGui::IsItemDeactivated())
                    endGesture();
            };
            const auto attribute = [&](const char* name) {
                return [element, name](const std::string& value) { GuiDoc::setAttribute(element, name, value); };
            };
            const bool input = kind == GuiKind::Toggle || kind == GuiKind::Slider || kind == GuiKind::Dropdown ||
                               kind == GuiKind::Spinbox;
            if (kind == GuiKind::Unknown) {
                ImGui::TextDisabled("Not editable in the builder (but for its data binding); kept as written. It can be "
                                    "moved or deleted.");
            } else {
                field("id", inspectorBuffer_[0], attribute("id"));
                field("class", inspectorBuffer_[1], attribute("class"));
                if (kind == GuiKind::Text || kind == GuiKind::Button) {
                    if (GuiDoc::hasEditableText(selected_))
                        field("text", inspectorBuffer_[2],
                              [this](const std::string& value) { GuiDoc::setText(selected_, value); });
                    else
                        ImGui::TextDisabled("text: holds other elements; edit it in the .rml");
                }
                if (kind == GuiKind::Button || input) {
                    field("data-event", inspectorBuffer_[3], attribute("data-event"));
                    ImGui::TextDisabled(kind == GuiKind::Button ? "Clicks publish this event on the event bus (empty: none)."
                                                                : "Value changes publish this event (empty: none).");
                }
                // Widget properties: a toggle's checked state is a checkbox (one step a click), the others text fields.
                for (auto& [name, buffer] : widgetBuffers_) {
                    if (name == "checked") {
                        bool checked = buffer == "true";
                        if (ImGui::Checkbox("checked", &checked))
                            editStep(std::format("Edit {} checked", guiKindLabel(kind)),
                                     [&] { GuiDoc::setWidgetProperty(selected_, "checked", checked ? "true" : "false"); });
                    } else {
                        field(name.c_str(), buffer,
                              [this, &name](const std::string& value) { GuiDoc::setWidgetProperty(selected_, name, value); });
                    }
                    if (!inspectorNode_)
                        break; // a step refilled the buffers
                }
                if (kind == GuiKind::Dropdown && inspectorNode_)
                    drawOptions();
            }
            // RmlUi data binding, on any element: the ones it has (emptied: removed from the file), and one to add.
            ImGui::Separator();
            ImGui::TextDisabled("Data binding (data-model, data-for, data-if, data-value, data-attr-*, ...):");
            for (auto& [name, buffer] : bindingBuffers_)
                field(name.c_str(), buffer, attribute(name.c_str()));
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
            ImGui::InputTextWithHint("##binding", "data-for, data-class-active, ...", &newBinding_);
            ImGui::SameLine();
            ImGui::BeginDisabled(!guiBindingAttribute(newBinding_) || bindingBuffers_.contains(newBinding_));
            if (ImGui::Button("Add binding"))
                bindingBuffers_[std::exchange(newBinding_, {})]; // written once it has a value
            ImGui::EndDisabled();
            bool header = false;
            for (pugi::xml_attribute other : element.attributes()) {
                const std::string_view name = other.name();
                if (guiBindingAttribute(name) || GuiDoc::widgetAttribute(selected_, name) ||
                    (kind != GuiKind::Unknown && (name == "id" || name == "class" || name == "data-event")))
                    continue;
                if (!std::exchange(header, true)) {
                    ImGui::Separator();
                    ImGui::TextDisabled("Other attributes (kept as written):");
                }
                ImGui::BulletText("%s = \"%s\"", other.name(), other.value());
            }
        }
    } else if (rml_.empty()) {
        ImGui::TextDisabled("No GUI open.");
    }
    ImGui::End();
}

void GuiBuilder::drawOptions()
{
    const std::vector<pugi::xml_node> options = GuiDoc::options(selected_);
    if (options.size() != optionBuffers_.size()) {
        inspectorNode_ = {}; // changed under the buffers (undo): refill next frame
        return;
    }
    ImGui::Separator();
    ImGui::TextDisabled("Options (selected, value, label):");
    for (size_t i = 0; i < options.size(); ++i) {
        const pugi::xml_node option = options[i];
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::RadioButton("##selected", static_cast<bool>(option.attribute("selected"))) && !option.attribute("selected"))
            editStep("Select Dropdown option", [&] { GuiDoc::selectOption(selected_, option); });
        // Two text fields share what the radio button and the three buttons (each a frame high and wide) leave.
        const float width = (ImGui::GetContentRegionAvail().x - 4.0f * ImGui::GetFrameHeight() -
                             5.0f * ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        auto& [value, label] = optionBuffers_[i];
        const auto edit = [&](const char* name, std::string& buffer, auto set) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(width);
            const bool changed = ImGui::InputText(name, &buffer);
            if (ImGui::IsItemActive() && !gesture_)
                gesture_ = Gesture{"Edit Dropdown option", selected_, copyGuiElement(selected_)};
            if (changed) {
                set(buffer);
                markRmlDirty();
            }
            if (ImGui::IsItemDeactivated())
                endGesture();
        };
        if (inspectorNode_)
            edit("##value", value, [option](const std::string& text) { GuiDoc::setAttribute(option, "value", text); });
        if (inspectorNode_)
            edit("##label", label, [option](const std::string& text) { GuiDoc::setText(option, text); });
        ImGui::SameLine();
        if (ImGui::ArrowButton("##up", ImGuiDir_Up) && inspectorNode_)
            editStep("Move Dropdown option Up", [&] { doc_.move(option, -1); });
        ImGui::SameLine();
        if (ImGui::ArrowButton("##down", ImGuiDir_Down) && inspectorNode_)
            editStep("Move Dropdown option Down", [&] { doc_.move(option, 1); });
        ImGui::SameLine();
        if (ImGui::Button("X") && inspectorNode_)
            editStep("Remove Dropdown option", [&] { selected_.remove_child(option); });
        ImGui::PopID();
        if (!inspectorNode_)
            return; // a step changed the options: refill next frame
    }
    if (ImGui::Button("Add option"))
        editStep("Add Dropdown option", [this] { GuiDoc::addOption(selected_); });
}

void GuiBuilder::drawStyle()
{
    if (focusStep_ == 2)
        ImGui::SetNextWindowFocus();
    if (ImGui::Begin(title("GUI Style", kStyleWindow).c_str()) && !rml_.empty()) {
        saveShortcut();
        if (rcss_.empty()) {
            ImGui::TextDisabled("This GUI links no .rcss file (<link type=\"text/rcss\" href=\"...\"/> in its <head>).");
        } else {
            if (ImGui::Button("Save"))
                save();
            ImGui::SameLine();
            ImGui::TextUnformatted(utf8(rcss_.filename()).c_str());
            if (sheetFailed_) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Errors (see Console): preview shows the last good style");
            }
            if (ImGui::InputTextMultiline("##rcss", &rcssText_, ImGui::GetContentRegionAvail(),
                                          ImGuiInputTextFlags_AllowTabInput)) {
                rcssDirty_ = true;
                rcssEditedAt_ = ImGui::GetTime();
            }
            if (ImGui::IsItemDeactivated())
                endGesture(); // the Style edit is one step

        }
    } else if (rml_.empty()) {
        ImGui::TextDisabled("No GUI open.");
    }
    ImGui::End();
    if (rcssEditedAt_ >= 0.0 && ImGui::GetTime() - rcssEditedAt_ >= kRcssApplyDelaySeconds)
        applyRcss();
}

void GuiBuilder::rebuildPreview()
{
    previewStale_ = false;
    Rml::Context* context = preview_.context();
    if (!context)
        return;
    if (previewDoc_)
        previewDoc_->Close();
    previewDoc_ = nullptr;
    previewElements_.clear();
    if (rml_.empty())
        return;
    // ponytail: the source URL is just the file name (RmlUi's URL parser rejects "C:/..."); fine while previews load
    // no relative resources (images, @import).
    const std::string rml = doc_.previewRml(goodRcss_, previewNodes_);
    previewModels_.clear(context); // fresh: a name the last version bound would shadow an alias this one declares
    previewModels_.prepare(*context, rml); // templates as authored: data-for rows and {{...}} show no data
    previewDoc_ = context->LoadDocumentFromMemory(rml, utf8(rml_.filename()));
    if (!previewDoc_)
        return;
    kast::gui::addWidgetStyle(*previewDoc_);
    previewDoc_->Show();
    previewElements_.assign(previewNodes_.size(), nullptr);
    collectElements(previewDoc_, previewElements_);
}

void GuiBuilder::drawPreview()
{
    if (focusStep_ == 1) // last: the preview keeps the focus
        ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool visible = ImGui::Begin(title("GUI Preview", kPreviewWindow).c_str());
    ImGui::PopStyleVar();
    if (visible && !rml_.empty()) {
        saveShortcut();
        ImGui::SetNextItemWidth(160.0f);
        const auto label = [&](int i) {
            const auto [w, h] = resolutions_[static_cast<size_t>(i)];
            return w == 0 ? std::string("Panel size") : std::format("{}x{}", w, h);
        };
        if (ImGui::BeginCombo("Resolution", label(resolution_).c_str())) {
            for (int i = 0; i < static_cast<int>(resolutions_.size()); ++i)
                if (ImGui::Selectable(label(i).c_str(), i == resolution_))
                    resolution_ = i;
            ImGui::EndCombo();
        }
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float fbScale = ImGui::GetIO().DisplayFramebufferScale.x;
        auto [width, height] = resolutions_[static_cast<size_t>(resolution_)];
        if (width == 0) {
            width = static_cast<int>(avail.x * fbScale);
            height = static_cast<int>(avail.y * fbScale);
        }
        if (avail.x > 0.0f && avail.y > 0.0f && width > 0 && height > 0) {
            if (previewStale_)
                rebuildPreview();
            target_.resize(static_cast<uint16_t>(width), static_cast<uint16_t>(height));
            preview_.resize(width, height);
            preview_.render(target_.view(), target_.frameBuffer());
            const float scale = std::min(avail.x / static_cast<float>(width), avail.y / static_cast<float>(height));
            const ImVec2 size(static_cast<float>(width) * scale, static_cast<float>(height) * scale);
            const ImVec2 start = ImGui::GetCursorScreenPos();
            const ImVec2 min(start.x + (avail.x - size.x) * 0.5f, start.y + (avail.y - size.y) * 0.5f);
            ImGui::SetCursorScreenPos(min);
            if (target_.valid())
                ImGui::Image(static_cast<ImTextureID>(target_.texture().idx), size);
            if (ImGui::IsItemClicked()) {
                const ImVec2 mouse = ImGui::GetMousePos();
                pick((mouse.x - min.x) / scale, (mouse.y - min.y) / scale);
            }
            const auto found = std::ranges::find(previewNodes_, selected_);
            if (found != previewNodes_.end()) {
                const size_t index = static_cast<size_t>(found - previewNodes_.begin());
                if (Rml::Element* element = index < previewElements_.size() ? previewElements_[index] : nullptr) {
                    const Rml::Vector2f offset = element->GetAbsoluteOffset(Rml::BoxArea::Border);
                    const Rml::Vector2f box = element->GetBox().GetSize(Rml::BoxArea::Border);
                    ImGui::GetWindowDrawList()->AddRect(ImVec2(min.x + offset.x * scale, min.y + offset.y * scale),
                                                        ImVec2(min.x + (offset.x + box.x) * scale,
                                                               min.y + (offset.y + box.y) * scale),
                                                        kOutlineColor, 0.0f, 0, 2.0f);
                }
            }
        }
    } else if (visible) {
        ImGui::TextDisabled("Double-click a .rml file in Assets to open it.");
    }
    ImGui::End();
}

void GuiBuilder::drawModals()
{
    if (focusStep_ > 0)
        --focusStep_;
    if (std::exchange(openPromptRequested_, false))
        ImGui::OpenPopup("Unsaved GUI changes##open");
    if (ImGui::BeginPopupModal("Unsaved GUI changes##open", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Save changes to %s before opening %s?", utf8(rml_.filename()).c_str(),
                    utf8(pendingOpen_.filename()).c_str());
        ImGui::Spacing();
        const bool saveIt = ImGui::Button("Save");
        ImGui::SameLine();
        const bool discard = ImGui::Button("Discard");
        ImGui::SameLine();
        const bool cancel = ImGui::Button("Cancel");
        if ((saveIt && save()) || discard) {
            rmlDirty_ = rcssDirty_ = false;
            open(pendingOpen_);
        }
        if (saveIt || discard || cancel)
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (std::exchange(reloadPromptRequested_, false))
        ImGui::OpenPopup("GUI changed on disk");
    if (ImGui::BeginPopupModal("GUI changed on disk", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s changed on disk, and the builder has unsaved changes.", utf8(rml_.filename()).c_str());
        ImGui::Spacing();
        if (ImGui::Button("Reload")) {
            load(rml_);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep mine"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (std::exchange(quitPromptRequested_, false))
        ImGui::OpenPopup("Unsaved GUI changes##quit");
    if (ImGui::BeginPopupModal("Unsaved GUI changes##quit", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Save changes to %s before quitting?", utf8(rml_.filename()).c_str());
        ImGui::Spacing();
        const bool saveIt = ImGui::Button("Save");
        ImGui::SameLine();
        const bool discard = ImGui::Button("Discard");
        ImGui::SameLine();
        const bool cancel = ImGui::Button("Cancel");
        if ((saveIt && save()) || discard) {
            quitConfirmed_ = true;
            kast::requestQuit(); // asks again; the scene's own prompt comes next if it is dirty
        }
        if (saveIt || discard || cancel)
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
