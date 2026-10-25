#include "gui_builder.h"

#include "asset_ids.h"
#include "assets.h"
#include "ui_common.h"

#include <app.h>
#include <gui_richtext.h>
#include <log.h>
#include <project.h>

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
    const bool prefab = rml.extension() == kast::kGuiPrefabExtension;
    GuiDoc doc;
    const std::filesystem::path root = kast::findProjectDir(rml.parent_path()).value_or(rml.parent_path());
    const std::string documentPath = kast::toProjectPath(root, rml).value_or(utf8(rml.filename()));
    const size_t stale = staleAssetRefs();
    const bool read = readText(rml, text);
    if (const std::optional<std::string> fresh = read ? editAssetRefs(documentPath, text, [](std::string& path, std::string& id) {
            if (!id.empty())
                path = resolveAssetRef(id, path); // by id first
        }) : std::nullopt)
        text = *fresh;
    if (!read || !(prefab ? doc.parsePrefab(text, error) : doc.parse(text, error))) {
        kast::log::error(std::format("Cannot open GUI '{}': {}", rml.string(), error));
        return false;
    }
    // A reload frees the old nodes: keep the selection by id where it has one.
    const bool reload = rml == rml_;
    const std::string selectedId = reload && selected_ ? GuiDoc::attribute(selected_, "id") : std::string();
    rml_ = rml;
    doc_.setPrefabContext(root, documentPath);
    prefab ? doc_.parsePrefab(text, error) : doc_.parse(text, error); // parsed fine above (now with its instances)
    rcss_ = prefab ? guiPrefabSheet(rml) : linkedStylesheet(rml);
    rcssText_.clear();
    if (prefab)
        readText(rcss_, rcssText_); // none yet: Save writes one once it has rules
    else if (rcss_.empty() && !doc_.stylesheetHref().empty())
        kast::log::warn(std::format("GUI '{}' links '{}', outside its project: the stylesheet is ignored",
                                    utf8(rml.filename()), doc_.stylesheetHref()));
    else if (!rcss_.empty() && !readText(rcss_, rcssText_))
        kast::log::warn(std::format("GUI '{}' links '{}', which is missing: Save creates it", utf8(rml.filename()),
                                    utf8(rcss_.filename())));
    rmlDirty_ = staleAssetRefs() != stale; // Save writes the fresh hints
    rcssDirty_ = false;
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
    std::error_code ec;
    const bool sheet = !rcss_.empty() && (!doc_.isPrefab() || !rcssText_.empty() || std::filesystem::exists(rcss_, ec));
    std::string text = doc_.serialize();
    const std::filesystem::path root = projectRoot();
    if (const std::optional<std::string> withIds =
            editAssetRefs(kast::toProjectPath(root, rml_).value_or(utf8(rml_.filename())), text,
                          [](std::string& path, std::string& id) { id = assetRefId(path).value_or(id); }))
        text = *withIds;
    if (!writeTextFile(rml_, text, error) || (sheet && !writeTextFile(rcss_, rcssText_, error))) {
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
    editStep(std::format("Edit {} {}", guiKindLabel(guiKind(selected_)), name), [&] {
        if (name == "id" && GuiDoc::instanceRoot(selected_) == selected_)
            GuiDoc::setInstanceId(selected_, value); // the ids inside follow
        else
            GuiDoc::setAttribute(GuiDoc::widgetElement(selected_), name.c_str(), value);
    });
    return true;
}

bool GuiBuilder::requestReparent(const std::string& parentId)
{
    std::vector<pugi::xml_node> nodes;
    doc_.previewRml("", nodes);
    const auto it = std::ranges::find_if(nodes, [&](pugi::xml_node node) { return GuiDoc::attribute(node, "id") == parentId; });
    const pugi::xml_node parent = parentId.empty() ? doc_.body() : it == nodes.end() ? pugi::xml_node() : *it;
    if (!parent || !selected_)
        return false;
    edit_ = {Edit::Op::Reparent, selected_, parent};
    return true;
}

std::filesystem::path GuiBuilder::projectRoot() const
{
    return kast::findProjectDir(rml_.parent_path()).value_or(rml_.parent_path());
}

void GuiBuilder::prefabFailed(std::string error)
{
    kast::log::error(error);
    prefabError_ = std::move(error);
    prefabErrorRequested_ = true;
}

void GuiBuilder::reselect(const std::vector<size_t>& path)
{
    selected_ = path.empty() ? pugi::xml_node() : guiNodeAt(doc_.body().root(), path);
    inspectorNode_ = {};
    previewStale_ = true;
}

bool GuiBuilder::createPrefab(const std::filesystem::path& dir)
{
    endGesture();
    if (!selected_ || selected_ == doc_.body()) {
        prefabFailed("Create GUI Prefab: select an element under Body first");
        return false;
    }
    GuiElementCopy before = copyGuiElement(selected_);
    std::filesystem::path file;
    std::string error;
    const pugi::xml_node instance = doc_.createPrefab(selected_, dir, file, error);
    if (!instance) {
        prefabFailed(std::format("Create GUI Prefab: {}", error));
        return false;
    }
    selected_ = instance;
    record("Create GUI Prefab", std::move(before), copyGuiElement(instance));
    inspectorNode_ = {};
    markRmlDirty();
    rememberFileTimes();
    kast::log::info(std::format("Created GUI prefab '{}' (and its .rcss); #{} is an instance of it", utf8(file),
                                GuiDoc::attribute(instance, "id")));
    return true;
}

bool GuiBuilder::placePrefab(const std::string& path)
{
    endGesture();
    std::string error;
    pugi::xml_node parent = addParent();
    if (!doc_.canAddInto(parent) && GuiDoc::instanceRoot(parent))
        parent = GuiDoc::instanceRoot(parent).parent(); // a selected instance: beside it (placing twice in a row)
    const pugi::xml_node instance = doc_.placePrefab(path, parent, error);
    if (!instance) {
        prefabFailed(std::format("Place GUI Prefab: {}", error));
        return false;
    }
    selected_ = instance;
    record("Place GUI Prefab", std::nullopt, copyGuiElement(instance));
    inspectorNode_ = {};
    markRmlDirty();
    rememberFileTimes();
    kast::log::info(std::format("Placed GUI prefab '{}' as #{}", path, GuiDoc::attribute(instance, "id")));
    return true;
}

bool GuiBuilder::revertField(const std::string& name)
{
    endGesture();
    const pugi::xml_node root = GuiDoc::instanceRoot(selected_);
    if (!root) {
        prefabFailed("Revert: the selection is not part of a prefab instance");
        return false;
    }
    if (name != "*") {
        if (!doc_.overridden(selected_, name))
            return false;
        return editStep(std::format("Revert {}", name), [&] { doc_.revert(selected_, name); });
    }
    const std::vector<size_t> path = copyGuiElement(selected_).path;
    GuiElementCopy before = copyGuiElement(root);
    const pugi::xml_node reverted = doc_.revertAll(root);
    reselect(path);
    if (record("Revert All", std::move(before), copyGuiElement(reverted)))
        markRmlDirty();
    return true;
}

bool GuiBuilder::applyField(const std::string& name)
{
    endGesture();
    std::string error;
    const std::vector<size_t> path = copyGuiElement(selected_).path;
    if (!selected_ || !doc_.apply(selected_, name == "*" ? "" : name, error)) {
        prefabFailed(std::format("Apply: {}", selected_ ? error : "nothing selected"));
        return false;
    }
    history_.clear(!dirty()); // the prefab file was written: as a scene prefab's Apply, no undo across it
    reselect(path);
    rememberFileTimes();
    return true;
}

bool GuiBuilder::unpack()
{
    endGesture();
    const pugi::xml_node root = GuiDoc::instanceRoot(selected_);
    if (!root) {
        prefabFailed("Unpack: the selection is not part of a prefab instance");
        return false;
    }
    GuiElementCopy before = copyGuiElement(root);
    GuiDoc::unpack(root);
    selected_ = root;
    record("Unpack GUI Prefab", std::move(before), copyGuiElement(root));
    inspectorNode_ = {};
    markRmlDirty();
    return true;
}

void GuiBuilder::acceptElementDrop(const std::filesystem::path& dir)
{
    if (rml_.empty() || !ImGui::BeginDragDropTarget())
        return;
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kNodePayload)) {
        edit_ = {Edit::Op::CreatePrefab, pugi::xml_node(*static_cast<pugi::xml_node_struct* const*>(payload->Data))};
        edit_.dir = dir;
    }
    ImGui::EndDragDropTarget();
}

void GuiBuilder::refreshPrefabs()
{
    const std::vector<size_t> path = selected_ ? copyGuiElement(selected_).path : std::vector<size_t>();
    doc_.refreshPrefabs();
    history_.clear(false); // node copies of the old instances would put stale prefab content back
    gesture_.reset();
    reselect(path);
    kast::log::info(std::format("A GUI prefab changed on disk: instances in '{}' refreshed (unsaved edits kept)",
                                utf8(rml_.filename())));
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

bool GuiBuilder::wrapRichText(const std::string& tag, const std::string& value, size_t start, size_t end)
{
    if (!selected_ || guiKind(selected_) != GuiKind::RichText || !GuiDoc::hasEditableText(selected_))
        return false;
    editStep(std::format("Rich Text [{}]", tag), [&] {
        GuiDoc::setText(selected_, guiWrapRichText(GuiDoc::text(selected_), start, end, tag, value));
    });
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
    prefabTimes_.clear();
    for (const std::string& path : doc_.prefabFiles()) {
        const std::filesystem::path file = projectRoot() / fromUtf8(path);
        prefabTimes_.emplace_back(file, modified(file));
    }
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

void GuiBuilder::setPreviewScale(int percent)
{
    if (std::ranges::find(scales_, percent) == scales_.end())
        scales_.push_back(percent);
    scale_ = static_cast<int>(std::ranges::find(scales_, percent) - scales_.begin());
}

void GuiBuilder::remap(const std::filesystem::path& from, const std::filesystem::path& to)
{
    if (rml_.empty())
        return;
    bool moved = false;
    if (const auto path = remapPath(rml_, from, to))
        rml_ = *path, moved = true;
    if (const auto path = rcss_.empty() ? std::nullopt : remapPath(rcss_, from, to))
        rcss_ = *path, moved = true;
    // Instances follow their prefab's new path (the files on disk were rewritten too).
    const std::filesystem::path root = projectRoot();
    const std::optional<std::string> fromPath = kast::toProjectPath(root, from);
    const std::optional<std::string> toPath = kast::toProjectPath(root, to);
    if (fromPath && toPath && doc_.remapPrefabPaths(*fromPath, *toPath))
        previewStale_ = true;
    std::error_code ec;
    if (doc_.isPrefab()) { // renameGuiPrefab / moveGuiPrefab took its sheet along
        rcss_ = guiPrefabSheet(rml_);
        doc_.setPrefabContext(root, kast::toProjectPath(root, rml_).value_or(utf8(rml_.filename())));
        rememberFileTimes();
        return;
    }
    // renameGui renamed the sibling .rcss and rewrote the link on disk: follow it. The link follows the sheet or the
    // .rml moving away from it (as the rename pass rewrote it on disk), so Save keeps it.
    if (const std::filesystem::path linked = linkedStylesheet(rml_);
        !linked.empty() && linked != rcss_ && !rcss_.empty() && !std::filesystem::exists(rcss_, ec))
        rcss_ = linked, moved = true;
    if (moved && !rcss_.empty())
        doc_.setStylesheetHref(utf8(rcss_.lexically_relative(rml_.parent_path())));
    rememberFileTimes();
}

void GuiBuilder::poll()
{
    if (rml_.empty())
        return;
    const auto rmlTime = modified(rml_);
    const auto rcssTime = modified(rcss_);
    if (rmlTime == rmlTime_ && rcssTime == rcssTime_) {
        // A prefab it holds instances of was saved (by the builder, or elsewhere): they show it, unsaved edits or not.
        if (std::ranges::any_of(prefabTimes_, [](const auto& file) { return modified(file.first) != file.second; })) {
            if (dirty())
                refreshPrefabs();
            else
                load(rml_);
            rememberFileTimes();
        }
        return;
    }
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
            ImGui::BeginDisabled(!doc_.canAddInto(addParent())); // nothing goes into a prefab instance
            if (ImGui::Button(label.c_str()))
                edit_ = {Edit::Op::Add, addParent(), {}, kind};
            ImGui::EndDisabled();
        }
        const bool movable = selected_ && selected_ != doc_.body() && !GuiDoc::prefabLocked(selected_);
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
    const bool missing = GuiDoc::missingPrefab(node);
    const bool locked = GuiDoc::prefabLocked(node); // listed and editable, but fixed in place
    const bool instance = !body && GuiDoc::instanceRoot(node) == node;
    const std::vector<pugi::xml_node> children =
        GuiDoc::composite(node) || missing ? std::vector<pugi::xml_node>() : GuiDoc::elements(node);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    if (children.empty())
        flags |= ImGuiTreeNodeFlags_Leaf;
    if (node == selected_)
        flags |= ImGuiTreeNodeFlags_Selected;
    const std::string prefab = GuiDoc::attribute(node, kast::kGuiPrefabAttribute);
    std::string label = body ? std::string("Body") : nodeLabel(node);
    if (missing)
        label = std::format("missing prefab {}{}", prefab, label.substr(std::min(label.find(' '), label.size())));
    const bool unknown = !body && !missing && guiKind(node) == GuiKind::Unknown;
    const bool tinted = unknown || (missing && node != selected_); // a selected row's text is darkened on the accent
    if (tinted)
        ImGui::PushStyleColor(ImGuiCol_Text, missing ? kErrorColor : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(node.internal_object()), flags, "%s", label.c_str());
    if (tinted)
        ImGui::PopStyleColor();
    if (instance) // the prefab marker after its label, as the scene Hierarchy's
        drawPrefabIcon(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing() + ImGui::CalcTextSize(label.c_str()).x +
                           ImGui::GetStyle().ItemSpacing.x,
                       ImGui::GetItemRectMin().y, ImGui::GetItemRectSize().y, node == selected_);
    if ((instance || missing) && ImGui::IsItemHovered())
        ImGui::SetTooltip(missing ? "Missing GUI prefab %s: kept (with its overrides) and saved unchanged" : "Instance of %s",
                          prefab.c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        selected_ = node;
    if (!body && !locked && ImGui::BeginDragDropSource()) {
        pugi::xml_node_struct* payload = node.internal_object();
        ImGui::SetDragDropPayload(kNodePayload, &payload, sizeof(payload));
        ImGui::TextUnformatted(label.c_str());
        ImGui::EndDragDropSource();
    }
    if (isPanelOrBody(node, doc_.body()) && doc_.canAddInto(node)) {
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kNodePayload))
                edit_ = {Edit::Op::Reparent, pugi::xml_node(*static_cast<pugi::xml_node_struct* const*>(payload->Data)), node};
            ImGui::EndDragDropTarget();
        }
        if (std::string dropped; acceptGuiPrefabDrop(dropped)) {
            edit_ = {Edit::Op::PlacePrefab, node};
            edit_.path = dropped;
        }
    }
    if (ImGui::BeginPopupContextItem()) {
        selected_ = node;
        if (isPanelOrBody(node, doc_.body()) && doc_.canAddInto(node))
            for (GuiKind kind : kGuiCreatableKinds)
                if (ImGui::MenuItem(std::format("Add {}", guiKindLabel(kind)).c_str()))
                    edit_ = {Edit::Op::Add, node, {}, kind};
        if (!body && !locked) {
            if (ImGui::MenuItem("Move Up"))
                edit_ = {Edit::Op::Move, node, {}, GuiKind::Panel, -1};
            if (ImGui::MenuItem("Move Down"))
                edit_ = {Edit::Op::Move, node, {}, GuiKind::Panel, 1};
            if (ImGui::MenuItem("Delete"))
                edit_ = {Edit::Op::Delete, node};
        }
        if (!body && !GuiDoc::instanceRoot(node) && !missing && ImGui::MenuItem("Create GUI Prefab")) {
            edit_ = {Edit::Op::CreatePrefab, node};
            edit_.dir = assetFolder ? assetFolder() : projectRoot();
        }
        if (instance && ImGui::MenuItem("Unpack Prefab"))
            prefabOp_ = PrefabOp{PrefabOp::Kind::Unpack, ""};
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
    if (edit.op == Edit::Op::PlacePrefab || edit.op == Edit::Op::CreatePrefab) {
        if (edit.op == Edit::Op::CreatePrefab) {
            selected_ = edit.node;
            createPrefab(edit.dir);
        } else {
            selected_ = edit.node; // the parent: placePrefab puts it in the selected Panel, else <body>
            placePrefab(edit.path);
        }
        return;
    }
    if (GuiDoc::prefabLocked(edit.node) || (edit.op == Edit::Op::Add && !doc_.canAddInto(edit.node))) {
        kast::log::warn(std::format("GUI Hierarchy: {} refused: {}", edit.op == Edit::Op::Add ? "Add" : "the edit",
                                    doc_.isPrefab() && edit.node == doc_.body() ? "a GUI prefab holds exactly one element"
                                                                                 : "a prefab instance's elements stay as the prefab has them"));
        return;
    }
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
            kast::log::warn("GUI Hierarchy: can't move an element into itself or into a prefab instance");
        name = "Reparent " + kind;
        break;
    case Edit::Op::None:
    case Edit::Op::PlacePrefab:
    case Edit::Op::CreatePrefab:
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
        } else if (GuiDoc::missingPrefab(selected_)) {
            drawPrefabCard({}); // a placeholder: nothing of it is edited
        } else {
            const GuiKind kind = guiKind(selected_);
            // A labelled Toggle's id, class, data-event and bindings are its <input>'s.
            const pugi::xml_node element = GuiDoc::widgetElement(selected_);
            if (inspectorNode_ != selected_) {
                inspectorNode_ = selected_;
                inspectorBuffer_[0] = GuiDoc::attribute(element, "id");
                inspectorBuffer_[1] = GuiDoc::attribute(element, "class");
                inspectorBuffer_[2] = GuiDoc::text(selected_);
                richSelection_ = {static_cast<int>(inspectorBuffer_[2].size()), static_cast<int>(inspectorBuffer_[2].size())};
                inspectorBuffer_[3] = GuiDoc::attribute(element, "data-event");
                bindingBuffers_.clear();
                for (pugi::xml_attribute attribute : element.attributes())
                    if (guiBindingAttribute(attribute.name()) && !GuiDoc::widgetAttribute(selected_, attribute.name()))
                        bindingBuffers_[attribute.name()] = attribute.value(); // a Virtual List's data-model is a property
                widgetBuffers_.clear();
                for (const std::string& name : GuiDoc::widgetProperties(selected_))
                    widgetBuffers_.emplace_back(name, GuiDoc::widgetProperty(selected_, name));
                optionBuffers_.clear();
                const bool rows = kind == GuiKind::ListBox;
                for (pugi::xml_node option : rows ? GuiDoc::rows(selected_) : GuiDoc::options(selected_))
                    optionBuffers_.emplace_back(GuiDoc::attribute(option, rows ? "data-value" : "value"), GuiDoc::text(option));
                pristine_ = doc_.pristine(selected_, pristineHolder_); // null outside instances
            }
            if (GuiDoc::instanceRoot(selected_))
                drawPrefabCard(GuiDoc::instanceRoot(selected_));
            ImGui::Text("%s  <%s>", guiKindLabel(kind), selected_.name());
            // One step per field edit: the element is copied before its first change, recorded when it lets go.
            const auto field = [&](const char* label, std::string& buffer, auto set) {
                const bool changed = ImGui::InputText(label, &buffer);
                const std::string_view name = std::string_view(label).substr(0, std::string_view(label).find("##"));
                overrideMarker(std::string(name));
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
                               kind == GuiKind::Spinbox || kind == GuiKind::ListBox || kind == GuiKind::VirtualList;
            if (kind == GuiKind::Unknown) {
                ImGui::TextDisabled("Not editable in the builder (but for its data binding); kept as written. It can be "
                                    "moved or deleted.");
            } else {
                field("id", inspectorBuffer_[0], [this, element](const std::string& value) {
                    if (GuiDoc::instanceRoot(selected_) == selected_)
                        GuiDoc::setInstanceId(selected_, value); // an instance's id: the ids inside it follow
                    else
                        GuiDoc::setAttribute(element, "id", value);
                });
                field("class", inspectorBuffer_[1], attribute("class"));
                if (kind == GuiKind::RichText && GuiDoc::hasEditableText(selected_)) {
                    drawRichText();
                } else if (kind == GuiKind::Text || kind == GuiKind::Button || kind == GuiKind::RichText) {
                    if (GuiDoc::hasEditableText(selected_))
                        field("text", inspectorBuffer_[2],
                              [this](const std::string& value) { GuiDoc::setText(selected_, value); });
                    else
                        ImGui::TextDisabled("text: holds other elements; edit it in the .rml");
                }
                if (kind == GuiKind::Button || input) {
                    field("data-event", inspectorBuffer_[3], attribute("data-event"));
                    ImGui::TextDisabled(kind == GuiKind::Button ? "Clicks publish this event on the event bus (empty: none)."
                                        : kind == GuiKind::ListBox || kind == GuiKind::VirtualList
                                            ? "Selection changes publish this event (empty: none)."
                                            : "Value changes publish this event (empty: none).");
                }
                // Widget properties: a toggle's checked state and a list's follow are checkboxes (one step a click), the
                // others text fields.
                for (auto& [name, buffer] : widgetBuffers_) {
                    if (name == "checked" || name == "follow") {
                        bool checked = buffer == "true";
                        const bool clicked = ImGui::Checkbox(name.c_str(), &checked);
                        overrideMarker(name);
                        if (clicked)
                            editStep(std::format("Edit {} {}", guiKindLabel(kind), name),
                                     [&] { GuiDoc::setWidgetProperty(selected_, name, checked ? "true" : "false"); });
                        if (name == "follow")
                            ImGui::TextDisabled("Rows added while it shows its end keep it there.");
                    } else {
                        field(name.c_str(), buffer,
                              [this, &name](const std::string& value) { GuiDoc::setWidgetProperty(selected_, name, value); });
                    }
                    if (!inspectorNode_)
                        break; // a step refilled the buffers
                }
                if ((kind == GuiKind::Dropdown || kind == GuiKind::ListBox) && inspectorNode_)
                    drawOptions();
                if (kind == GuiKind::VirtualList)
                    ImGui::TextDisabled("Rows: the template, once per item of the data model's source array; row-height "
                                        "pixels each; key: the item field that is its value. The preview shows %d "
                                        "placeholder rows.", kGuiPreviewRows);
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
    if (const std::optional<PrefabOp> op = std::exchange(prefabOp_, std::nullopt))
        runPrefabOp(*op);
}

void GuiBuilder::drawPrefabCard(pugi::xml_node root)
{
    beginCard();
    if (!root) {
        ImGui::TextColored(kErrorColor, "Missing prefab: %s", GuiDoc::attribute(selected_, kast::kGuiPrefabAttribute).c_str());
        ImGui::TextWrapped("It didn't load (see the Console). This placeholder keeps the reference and its overrides, and "
                           "saves them unchanged.");
        endCard();
        return;
    }
    const std::string path = GuiDoc::attribute(root, kast::kGuiPrefabAttribute);
    ImGui::Text("Prefab: %s", path.c_str());
    for (pugi::xml_node at = selected_; at && at != root; at = at.parent())
        if (at.attribute(kast::kGuiPrefabAttribute)) {
            ImGui::TextDisabled("In its nested prefab %s", GuiDoc::attribute(at, kast::kGuiPrefabAttribute).c_str());
            break;
        }
    if (selected_ != root) {
        if (ImGui::Button("Select Root"))
            selected_ = root;
        ImGui::SameLine();
    }
    if (ImGui::Button("Open Prefab"))
        prefabOp_ = PrefabOp{PrefabOp::Kind::Open, path};
    if (selected_ == root) {
        if (ImGui::Button("Revert All"))
            prefabOp_ = PrefabOp{PrefabOp::Kind::Revert, "*"};
        ImGui::SameLine();
        if (ImGui::Button("Apply All"))
            prefabOp_ = PrefabOp{PrefabOp::Kind::Apply, "*"};
        ImGui::SameLine();
        if (ImGui::Button("Unpack"))
            prefabOp_ = PrefabOp{PrefabOp::Kind::Unpack, ""};
    } else {
        ImGui::TextWrapped("A prefab element: its attributes and text can be overridden; it can't move, go or take children.");
    }
    endCard();
}

void GuiBuilder::overrideMarker(const std::string& field)
{
    if (!pristine_ || (field == "id" && GuiDoc::instanceRoot(selected_) == selected_) ||
        GuiDoc::field(selected_, field) == GuiDoc::field(pristine_, field))
        return;
    markOverride(ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().y);
    if (ImGui::BeginPopupContextItem(("##override " + field).c_str())) {
        const std::string prefab = GuiDoc::attribute(GuiDoc::instanceRoot(selected_), kast::kGuiPrefabAttribute);
        if (ImGui::MenuItem("Revert"))
            prefabOp_ = PrefabOp{PrefabOp::Kind::Revert, field};
        if (ImGui::MenuItem(std::format("Apply to {}", utf8(fromUtf8(prefab).filename())).c_str()))
            prefabOp_ = PrefabOp{PrefabOp::Kind::Apply, field};
        ImGui::EndPopup();
    }
}

void GuiBuilder::runPrefabOp(const PrefabOp& op)
{
    switch (op.kind) {
    case PrefabOp::Kind::Revert: revertField(op.field); break;
    case PrefabOp::Kind::Apply: applyField(op.field); break;
    case PrefabOp::Kind::Unpack: unpack(); break;
    case PrefabOp::Kind::Open: open(projectRoot() / fromUtf8(op.field)); break;
    }
}

void GuiBuilder::drawOptions()
{
    // A Dropdown's options or a List Box's rows: the same editor, over option value="" / selected="" or row
    // data-value="" / class "selected" (a List Box may have no row selected: its radio button toggles).
    const bool rows = guiKind(selected_) == GuiKind::ListBox;
    const std::string what = rows ? "List Box row" : "Dropdown option";
    const char* valueAttribute = rows ? "data-value" : "value";
    const bool fixed = static_cast<bool>(GuiDoc::instanceRoot(selected_)); // in a prefab: fixed rows
    const std::vector<pugi::xml_node> options = rows ? GuiDoc::rows(selected_) : GuiDoc::options(selected_);
    if (options.size() != optionBuffers_.size()) {
        inspectorNode_ = {}; // changed under the buffers (undo): refill next frame
        return;
    }
    ImGui::Separator();
    ImGui::TextDisabled(rows ? "Rows (selected, value, label):" : "Options (selected, value, label):");
    for (size_t i = 0; i < options.size(); ++i) {
        const pugi::xml_node option = options[i];
        ImGui::PushID(static_cast<int>(i));
        const bool chosen = rows ? GuiDoc::rowSelected(option) : static_cast<bool>(option.attribute("selected"));
        if (ImGui::RadioButton("##selected", chosen) && (rows || !chosen))
            editStep("Select " + what, [&] {
                if (rows)
                    GuiDoc::selectRow(selected_, chosen ? pugi::xml_node() : option);
                else
                    GuiDoc::selectOption(selected_, option);
            });
        // Two text fields share what the radio button and the three buttons (each a frame high and wide) leave.
        const float width = (ImGui::GetContentRegionAvail().x - 4.0f * ImGui::GetFrameHeight() -
                             5.0f * ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        auto& [value, label] = optionBuffers_[i];
        const auto edit = [&](const char* name, std::string& buffer, auto set) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(width);
            const bool changed = ImGui::InputText(name, &buffer);
            if (ImGui::IsItemActive() && !gesture_)
                gesture_ = Gesture{"Edit " + what, selected_, copyGuiElement(selected_)};
            if (changed) {
                set(buffer);
                markRmlDirty();
            }
            if (ImGui::IsItemDeactivated())
                endGesture();
        };
        if (inspectorNode_)
            edit("##value", value, [option, valueAttribute](const std::string& text) { GuiDoc::setAttribute(option, valueAttribute, text); });
        if (inspectorNode_)
            edit("##label", label, [option](const std::string& text) { GuiDoc::setText(option, text); });
        ImGui::BeginDisabled(fixed);
        ImGui::SameLine();
        if (ImGui::ArrowButton("##up", ImGuiDir_Up) && inspectorNode_)
            editStep("Move " + what + " Up", [&] { doc_.move(option, -1); });
        ImGui::SameLine();
        if (ImGui::ArrowButton("##down", ImGuiDir_Down) && inspectorNode_)
            editStep("Move " + what + " Down", [&] { doc_.move(option, 1); });
        ImGui::SameLine();
        if (ImGui::Button("X") && inspectorNode_)
            editStep("Remove " + what, [&] { selected_.remove_child(option); });
        ImGui::EndDisabled();
        ImGui::PopID();
        if (!inspectorNode_)
            return; // a step changed the options: refill next frame
    }
    ImGui::BeginDisabled(fixed);
    if (ImGui::Button(rows ? "Add row" : "Add option"))
        editStep("Add " + what, [this, rows] {
            if (rows)
                GuiDoc::addRow(selected_);
            else
                GuiDoc::addOption(selected_);
        });
    ImGui::EndDisabled();
}

void GuiBuilder::drawRichText()
{
    // The markup: one step from activation to letting go, like the other fields; its selection kept for the toolbar.
    const auto selection = [](ImGuiInputTextCallbackData* data) {
        *static_cast<std::pair<int, int>*>(data->UserData) = std::minmax(data->SelectionStart, data->SelectionEnd);
        return 0;
    };
    std::string& markup = inspectorBuffer_[2];
    const bool changed = ImGui::InputTextMultiline("##markup", &markup, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 6.0f),
                                                   ImGuiInputTextFlags_CallbackAlways, selection, &richSelection_);
    overrideMarker("text");
    if (ImGui::IsItemActive() && !gesture_)
        gesture_ = Gesture{"Edit Rich Text markup", selected_, copyGuiElement(selected_)};
    if (changed) {
        GuiDoc::setText(selected_, markup);
        markRmlDirty();
    }
    if (ImGui::IsItemDeactivated())
        endGesture();

    // Toolbar: B I U S wrap at once; the others ask for their value in a popup first. A wrap is a step that refills
    // the Inspector (inspectorNode_ cleared): nothing more is drawn from the old buffers then.
    const auto wrap = [&](const char* tag, const std::string& value) {
        wrapRichText(tag, value, static_cast<size_t>(richSelection_.first), static_cast<size_t>(richSelection_.second));
    };
    bool first = true;
    const auto button = [&](const char* label) { // wrapped to the panel's width, as the GUI Hierarchy's
        const float width = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        if (!std::exchange(first, false) && ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width <=
                                                ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x)
            ImGui::SameLine();
        return ImGui::Button(label);
    };
    for (const char* tag : {"b", "i", "u", "s"}) {
        const std::string label(1, static_cast<char>(std::toupper(static_cast<unsigned char>(*tag))));
        if (button(label.c_str()))
            wrap(tag, "");
    }
    const auto popup = [&](const char* label, const char* tag, const auto& body, const auto& value) {
        if (button(label))
            ImGui::OpenPopup(label);
        if (ImGui::BeginPopup(label)) {
            body();
            if (ImGui::Button("Apply")) {
                wrap(tag, value());
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    };
    popup("Color", "color", [&] { ImGui::ColorPicker3("##color", richColor_, ImGuiColorEditFlags_NoAlpha); }, [&] {
        const auto byte = [](float v) { return static_cast<int>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
        return std::format("#{:02x}{:02x}{:02x}", byte(richColor_[0]), byte(richColor_[1]), byte(richColor_[2]));
    });
    popup("Size", "size", [&] { ImGui::InputInt("pixels", &richSize_); },
          [&] { return std::to_string(std::clamp(richSize_, 4, 200)); });
    popup("Font", "font", [&] { ImGui::InputText("family", &richFont_); }, [&] { return richFont_; });
    popup("Link", "link", [&] { ImGui::InputTextWithHint("target", "quest_log", &richLink_); }, [&] { return richLink_; });
    popup("Image", "img", [&] {
        ImGui::InputTextWithHint("path", "../textures/icon.png", &richImage_);
        ImGui::InputInt2("w, h (0: auto)", richImageSize_);
    }, [&] {
        std::string value = richImage_;
        if (richImageSize_[0] > 0)
            value += std::format(" w={}", richImageSize_[0]);
        if (richImageSize_[1] > 0)
            value += std::format(" h={}", richImageSize_[1]);
        return value;
    });

    // The converter's warnings: what Play would show as literal text, close or reopen.
    std::vector<kast::GuiRichTextProblem> problems;
    kast::guiRichTextRml(GuiDoc::text(selected_), &problems, kast::gui::fontFamilyLoaded);
    ImGui::PushTextWrapPos(0.0f);
    if (problems.empty())
        ImGui::TextDisabled("Markup OK. Tags: [b] [i] [u] [s] [color=] [size=] [font=] [link=] [img=]; [[ is a literal [.");
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.0f, 1.0f)); // #ffcc00, the palette's highlight
    for (const kast::GuiRichTextProblem& problem : problems)
        ImGui::Text("Warning at %zu: %s", problem.offset, problem.message.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
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
    // The source URL is the .rml's own, as RmlUi spells it (gui::toRmlPath): relative images (a Rich Text's [img])
    // resolve next to it.
    // The prefab sheets come first, as Play links them: the GUI's own rules win at equal specificity.
    // A prefab alone has no host to give its text a font: Roboto, white, unless its own sheet says otherwise.
    std::string sheets = doc_.isPrefab() ? "body { font-family: Roboto; color: #ffffff; }\n" : "";
    for (const std::string& sheet : doc_.prefabSheets())
        if (std::string text; readText(projectRoot() / fromUtf8(sheet), text))
            sheets += text + "\n";
    const std::string rml = doc_.previewRml(sheets + goodRcss_, previewNodes_);
    previewModels_.clear(context); // fresh: a name the last version bound would shadow an alias this one declares
    previewModels_.prepare(*context, rml); // templates as authored: data-for rows and {{...}} show no data
    kast::gui::loadProjectFonts(kast::findProjectDir(rml_.parent_path()).value_or(std::filesystem::path()));
    previewDoc_ = context->LoadDocumentFromMemory(rml, kast::gui::toRmlPath(rml_));
    if (!previewDoc_)
        return;
    kast::gui::addWidgetStyle(*previewDoc_);
    kast::gui::convertRichText(*previewDoc_);
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
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        const auto percent = [&](int i) { return std::format("{}%", scales_[static_cast<size_t>(i)]); };
        if (ImGui::BeginCombo("UI scale", percent(scale_).c_str())) {
            for (int i = 0; i < static_cast<int>(scales_.size()); ++i)
                if (ImGui::Selectable(percent(i).c_str(), i == scale_))
                    scale_ = i;
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
            // The dp ratio as in Play (gui_scale.h), the chosen UI scale standing for the display's and the player's;
            // the preview lays out as a dp document (its dp units follow the ratio).
            kast::GuiScale guiScale = projectScale;
            guiScale.useOsDpi = false;
            guiScale.uiScale = scales_[static_cast<size_t>(scale_)] / 100.0;
            const double ratio = kast::guiDpRatio(guiScale, width, height, 1.0);
            if (ratio != previewRatio_) {
                previewRatio_ = ratio;
                kast::log::info(std::format("GUI Preview dp ratio {:.4g} ({}x{}, UI scale {}%)", ratio, width, height,
                                            scales_[static_cast<size_t>(scale_)]));
            }
            preview_.resize(width, height, static_cast<float>(ratio));
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
            if (std::string dropped; acceptGuiPrefabDrop(dropped)) { // into the deepest Panel under the cursor, else <body>
                const ImVec2 mouse = ImGui::GetMousePos();
                Rml::Element* hit = preview_.context()->GetElementAtPoint({(mouse.x - min.x) / scale, (mouse.y - min.y) / scale});
                pugi::xml_node parent = doc_.body();
                for (; hit; hit = hit->GetParentNode()) {
                    const int index = hit->GetAttribute<int>("data-kast-node", -1);
                    const pugi::xml_node node = index >= 0 && static_cast<size_t>(index) < previewNodes_.size()
                                                    ? previewNodes_[static_cast<size_t>(index)]
                                                    : pugi::xml_node();
                    if (node && guiKind(node) == GuiKind::Panel && doc_.canAddInto(node)) {
                        parent = node;
                        break;
                    }
                }
                edit_ = {Edit::Op::PlacePrefab, parent};
                edit_.path = dropped;
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
    if (!rml_.empty())
        applyEdit(); // a drop on the Preview or the Assets panel, with the GUI Hierarchy hidden behind another tab
    if (std::exchange(prefabErrorRequested_, false))
        ImGui::OpenPopup("GUI Prefab");
    if (ImGui::BeginPopupModal("GUI Prefab", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(prefabError_.c_str());
        ImGui::Spacing();
        if (ImGui::Button("OK") || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
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
