#define IMGUI_DEFINE_MATH_OPERATORS // before imgui.h: imgui_node_editor_internal.h needs ImVec2 math
#include "shader_graph_editor.h"

#include "assets.h"
#include "ui_common.h"

#include <app.h>
#include <log.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <imgui-node-editor/imgui_node_editor.h>
#include <imgui-node-editor/imgui_node_editor_internal.h> // a comment frame's group size (no public getter)

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <charconv>
#include <cmath>
#include <format>
#include <set>

namespace ed = ax::NodeEditor;
using kast::GraphComment;
using kast::GraphLink;
using kast::GraphNode;
using kast::GraphNodeDef;
using kast::GraphPin;
using kast::GraphPinDef;
using kast::PinType;
using kast::ShaderParam;
using kast::ShaderParamType;

namespace {

constexpr double kRefusedSeconds = 5.0;
// The canvas' ids. Nodes: the graph node id. Comment frames, pins and links in ranges of their own.
constexpr uintptr_t kCommentIds = uintptr_t(1) << 40;
constexpr uintptr_t kPinIds = uintptr_t(1) << 48;
constexpr uintptr_t kLinkIds = uintptr_t(1) << 50;
constexpr uintptr_t kRefusedLinkId = uintptr_t(1) << 52;

// The editor palette (CLAUDE.md), and pin colors readable on #000000 and #313131.
constexpr ImU32 kHeader = IM_COL32(0xff, 0xaa, 0x00, 0xff);
constexpr ImU32 kHighlight = IM_COL32(0xff, 0xcc, 0x00, 0xff);
constexpr ImU32 kDarken = IM_COL32(0xff, 0x99, 0x00, 0xff);
constexpr ImU32 kCard = IM_COL32(0x31, 0x31, 0x31, 0xff);
constexpr ImU32 kFieldHighlight = IM_COL32(0x81, 0x81, 0x81, 0xff);
constexpr ImU32 kRefusedColor = IM_COL32(0xff, 0x40, 0x40, 0xff);

ImU32 pinColor(PinType type)
{
    switch (type) {
    case PinType::Float: return IM_COL32(156, 204, 101, 255);     // green
    case PinType::Vec2: return IM_COL32(77, 208, 225, 255);       // cyan
    case PinType::Vec3: return IM_COL32(186, 104, 200, 255);      // purple
    case PinType::Vec4: return IM_COL32(100, 181, 246, 255);      // blue
    case PinType::Texture2D: return IM_COL32(244, 143, 177, 255); // pink
    case PinType::Dynamic: break;
    }
    return IM_COL32(189, 189, 189, 255); // grey: takes the widest number linked in
}

const char* pinTypeLabel(PinType type)
{
    constexpr const char* kNames[] = {"float", "vec2", "vec3", "vec4", "texture2D", "float..vec4 (widest input)"};
    return kNames[static_cast<int>(type)];
}

ed::NodeId commentId(size_t index) { return ed::NodeId(kCommentIds + index); }
ed::PinId pinId(int node, bool output, size_t index)
{
    return ed::PinId(kPinIds | (static_cast<uintptr_t>(node) << 8) | (output ? 0x80u : 0u) | index);
}
ed::LinkId linkId(int node, size_t input) { return ed::LinkId(kLinkIds | (static_cast<uintptr_t>(node) << 8) | input); }

const GraphNode* findNode(const kast::ShaderGraph& graph, int id)
{
    const auto it = std::ranges::find(graph.nodes, id, &GraphNode::id);
    return it == graph.nodes.end() ? nullptr : &*it;
}

size_t inputIndex(const GraphNodeDef& def, std::string_view pin)
{
    return static_cast<size_t>(std::ranges::find(def.inputs, pin, &GraphPinDef::name) - def.inputs.begin());
}

size_t outputIndex(const GraphNodeDef& def, std::string_view pin)
{
    return static_cast<size_t>(std::ranges::find_if(def.outputs, [&](const auto& o) { return o.first.name == pin; }) -
                               def.outputs.begin());
}

// The canvas pin id of a model pin; 0 if it doesn't exist.
ed::PinId canvasPin(const kast::ShaderGraph& graph, const GraphPin& pin, bool output)
{
    const GraphNode* node = findNode(graph, pin.node);
    const GraphNodeDef* def = node ? kast::graphNodeDef(*node) : nullptr;
    if (!def)
        return ed::PinId(0);
    const size_t index = output ? outputIndex(*def, pin.pin) : inputIndex(*def, pin.pin);
    return index < (output ? def->outputs.size() : def->inputs.size()) ? pinId(pin.node, output, index) : ed::PinId(0);
}

// A canvas pin id back to the model pin, and whether it is an output.
std::optional<std::pair<GraphPin, bool>> modelPin(const kast::ShaderGraph& graph, ed::PinId id)
{
    const uintptr_t value = id.Get();
    if ((value & kPinIds) == 0)
        return std::nullopt;
    const int nodeId = static_cast<int>((value & (kPinIds - 1)) >> 8);
    const bool output = (value & 0x80u) != 0;
    const size_t index = value & 0x7fu;
    const GraphNode* node = findNode(graph, nodeId);
    const GraphNodeDef* def = node ? kast::graphNodeDef(*node) : nullptr;
    if (!def || index >= (output ? def->outputs.size() : def->inputs.size()))
        return std::nullopt;
    return std::pair{GraphPin{nodeId, std::string(output ? def->outputs[index].first.name : def->inputs[index].name)}, output};
}

ImVec2 vec(std::array<float, 2> v) { return {v[0], v[1]}; }

// A comment frame's size: its group's, which imgui-node-editor keeps without a public getter.
ImVec2 groupSize(ed::NodeId id)
{
    auto* editor = reinterpret_cast<ed::Detail::EditorContext*>(ed::GetCurrentEditor());
    const ed::Detail::Node* node = editor->FindNode(id);
    return node ? node->m_GroupBounds.GetSize() : ImVec2(-1.0f, -1.0f);
}

// An unlinked input's default as numbers ("0.0", "vec3_splat(1.0)"); nullopt if it is code ("UV").
std::optional<float> numericFallback(std::string_view code)
{
    if (code.ends_with(")") && code.find("_splat(") != std::string_view::npos)
        code = code.substr(code.find('(') + 1, code.size() - code.find('(') - 2);
    float value = 0.0f;
    const auto [end, ec] = std::from_chars(code.data(), code.data() + code.size(), value);
    return ec == std::errc{} && end == code.data() + code.size() ? std::optional(value) : std::nullopt;
}

std::string stageList(unsigned stages)
{
    std::string out;
    for (const auto& [bit, name] : {std::pair{kast::kStageVertex, "vertex()"}, {kast::kStageFragment, "fragment()"},
                                    {kast::kStageLight, "light()"}})
        if (stages & bit)
            out += (out.empty() ? "" : ", ") + std::string(name);
    return out;
}

bool containsNoCase(std::string_view text, std::string_view part)
{
    return std::ranges::search(text, part, [](char a, char b) { return std::tolower(a) == std::tolower(b); }).begin() !=
               text.end() ||
           part.empty();
}

void tooltip(const std::string& text, ImU32 color)
{
    ed::Suspend();
    ImGui::BeginTooltip();
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopStyleColor();
    ImGui::EndTooltip();
    ed::Resume();
}

} // namespace

void ShaderGraphEditor::shutdown()
{
    if (canvas_)
        ed::DestroyEditor(canvas_);
    canvas_ = nullptr;
}

void ShaderGraphEditor::open(const std::filesystem::path& file)
{
    if (file == doc_.file()) { // already open: keep its edits
        focusPanels();
        return;
    }
    if (doc_.dirty()) {
        pendingOpen_ = file;
        openPromptRequested_ = true;
        return;
    }
    if (load(file))
        focusPanels();
}

bool ShaderGraphEditor::load(const std::filesystem::path& file)
{
    std::string error;
    if (!doc_.open(file, error)) {
        kast::log::error(std::format("Cannot open shader graph '{}': {}", file.string(), error));
        return false;
    }
    resetCanvas_ = true;
    selection_.clear();
    selectedComments_.clear();
    refused_.reset();
    fieldErrors_.clear();
    kast::log::info(std::format("Opened shader graph '{}'", file.string()));
    return true;
}

bool ShaderGraphEditor::save()
{
    if (!doc_.isOpen())
        return true;
    std::string error;
    if (!doc_.save(error)) {
        kast::log::error(std::format("Cannot save shader graph: {}", error));
        return false;
    }
    kast::log::info(std::format("Saved shader graph '{}'", doc_.file().string()));
    return true;
}

void ShaderGraphEditor::undo()
{
    if (doc_.history().canUndo())
        kast::log::info(std::format("Undo {}", doc_.history().undoName()));
    doc_.undo();
}

void ShaderGraphEditor::redo()
{
    if (doc_.history().canRedo())
        kast::log::info(std::format("Redo {}", doc_.history().redoName()));
    doc_.redo();
}

void ShaderGraphEditor::poll()
{
    const std::string name = doc_.isOpen() ? doc_.file().string() : std::string();
    switch (doc_.poll()) {
    case ShaderGraphDoc::DiskChange::Reloaded:
        resetCanvas_ = true;
        kast::log::info(std::format("Shader graph '{}' changed on disk: reloaded", name));
        break;
    case ShaderGraphDoc::DiskChange::Conflict:
        reloadPromptRequested_ = true;
        kast::log::info(std::format("Shader graph '{}' changed on disk while it has unsaved changes: Reload / Keep mine?", name));
        break;
    case ShaderGraphDoc::DiskChange::Removed:
        kast::log::info(std::format("Shader graph '{}' was removed{}", name, doc_.isOpen() ? "" : ": closed"));
        break;
    case ShaderGraphDoc::DiskChange::None:
        break;
    }
}

bool ShaderGraphEditor::onQuitRequested()
{
    if (quitConfirmed_ || !doc_.dirty())
        return true;
    quitPromptRequested_ = true;
    return false;
}

void ShaderGraphEditor::openAddMenu(std::optional<GraphPin> from)
{
    // An output or an input of that name (outputs first).
    const GraphNode* node = from ? findNode(doc_.graph(), from->node) : nullptr;
    const bool input = node && !canvasPin(doc_.graph(), *from, true).Get();
    requestAddMenu(std::move(from), input, viewCenter_);
    addMenuAtCenter_ = true;
}

void ShaderGraphEditor::showRefusedLink(const GraphPin& from, const GraphPin& to, std::string why)
{
    refused_ = Refused{from, to, std::move(why), ImGui::GetTime() + kRefusedSeconds};
}

std::string ShaderGraphEditor::title(const char* name, const char* window) const
{
    if (!doc_.isOpen())
        return std::format("{}{}", name, window);
    return std::format("{} - {}{}{}", name, utf8(doc_.file().filename()), doc_.dirty() ? "*" : "", window);
}

void ShaderGraphEditor::saveShortcut()
{
    if (doc_.isOpen() && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S)) // focused route: wins over the scene's Ctrl+S
        save();
}

// --- Canvas ---

void ShaderGraphEditor::drawCanvas()
{
    if (focusStep_ == 1) // last: the canvas keeps the focus
        ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool visible = ImGui::Begin(title("Shader Graph", kCanvasWindow).c_str());
    ImGui::PopStyleVar();
    if (!visible || !doc_.isOpen()) {
        if (visible)
            ImGui::TextDisabled("Double-click a .kshadergraph in Assets to open it.");
        ImGui::End();
        return;
    }
    saveShortcut();
    if (resetCanvas_ && canvas_) {
        ed::DestroyEditor(canvas_);
        canvas_ = nullptr;
    }
    if (!canvas_) {
        ed::Config config;
        config.SettingsFile = nullptr; // node positions live in the .kshadergraph
        canvas_ = ed::CreateEditor(&config);
        navigateIn_ = 2;
    }
    resetCanvas_ = false;
    const ImVec2 center(ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x * 0.5f,
                        ImGui::GetCursorScreenPos().y + ImGui::GetContentRegionAvail().y * 0.5f);
    ed::SetCurrentEditor(canvas_);
    ed::Style& style = ed::GetStyle();
    style.Colors[ed::StyleColor_Bg] = ImColor(0, 0, 0);
    style.Colors[ed::StyleColor_Grid] = ImColor(kCard);
    style.Colors[ed::StyleColor_NodeBg] = ImColor(kCard);
    style.Colors[ed::StyleColor_NodeBorder] = ImColor(kFieldHighlight);
    style.Colors[ed::StyleColor_HovNodeBorder] = ImColor(kDarken);
    style.Colors[ed::StyleColor_SelNodeBorder] = ImColor(kHighlight);
    style.Colors[ed::StyleColor_HovLinkBorder] = ImColor(kDarken);
    style.Colors[ed::StyleColor_SelLinkBorder] = ImColor(kHighlight);
    style.Colors[ed::StyleColor_NodeSelRect] = ImColor(255, 204, 0, 48);
    style.Colors[ed::StyleColor_NodeSelRectBorder] = ImColor(kHighlight);
    style.NodeRounding = 4.0f;
    style.NodePadding = ImVec4(8.0f, 4.0f, 8.0f, 8.0f);

    ed::Begin("Shader Graph Canvas");
    const ImVec2 mouse = ImGui::GetMousePos(); // canvas coordinates while the canvas isn't suspended
    mouse_ = {mouse.x, mouse.y};
    const ImVec2 view = ed::ScreenToCanvas(center);
    viewCenter_ = {std::round(view.x), std::round(view.y)};
    drawComments();
    drawNodes();
    const kast::ShaderGraph& graph = doc_.graph();
    for (const GraphLink& link : graph.links) {
        const GraphNode* to = findNode(graph, link.to.node);
        const GraphNodeDef* def = to ? kast::graphNodeDef(*to) : nullptr;
        if (def)
            ed::Link(linkId(link.to.node, inputIndex(*def, link.to.pin)), canvasPin(graph, link.from, true),
                     canvasPin(graph, link.to, false), ImColor(pinColor(kast::outputPinType(graph, link.from))), 2.0f);
    }
    if (refused_ && ImGui::GetTime() > refused_->until)
        refused_.reset();
    if (refused_) {
        ed::Link(ed::LinkId(kRefusedLinkId), canvasPin(graph, refused_->from, true), canvasPin(graph, refused_->to, false),
                 ImColor(kRefusedColor), 3.0f);
        const ImVec2 at = ed::GetNodePosition(refused_->to.node);
        ImGui::GetWindowDrawList()->AddText(ImVec2(at.x, at.y - ImGui::GetTextLineHeightWithSpacing()), kRefusedColor,
                                            std::format("Can't link: {}", refused_->why).c_str());
    }
    handleCreate();
    handleDelete();
    handleShortcuts();
    if (pendingSelect_) {
        ed::ClearSelection();
        for (int id : *pendingSelect_)
            ed::SelectNode(id, true);
        pendingSelect_.reset();
    }
    // Right-click or Space on the canvas: the add menu at the cursor.
    ed::Suspend();
    if (ed::ShowBackgroundContextMenu() ||
        (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !ImGui::IsAnyItemActive() &&
         ImGui::IsKeyPressed(ImGuiKey_Space, false)))
        requestAddMenu(std::nullopt, false, mouse_);
    ed::Resume();
    drawAddMenu();
    if (navigateIn_ > 0 && --navigateIn_ == 0)
        ed::NavigateToContent(0.0f);
    ed::End();
    syncFromCanvas();
    ed::SetCurrentEditor(nullptr);
    ImGui::End();
}

void ShaderGraphEditor::drawComments()
{
    const kast::ShaderGraph& graph = doc_.graph();
    for (size_t i = 0; i < graph.comments.size(); ++i) {
        const GraphComment& comment = graph.comments[i];
        const ed::NodeId id = commentId(i);
        // The model is the truth: after an undo, a load or a CLI edit the canvas follows it.
        if (ed::GetNodePosition(id) != ImVec2(comment.rect[0], comment.rect[1]))
            ed::SetNodePosition(id, ImVec2(comment.rect[0], comment.rect[1]));
        if (groupSize(id) != ImVec2(comment.rect[2], comment.rect[3]))
            ed::SetGroupSize(id, ImVec2(comment.rect[2], comment.rect[3]));
        ed::PushStyleColor(ed::StyleColor_NodeBg, ImColor(255, 170, 0, 24));
        ed::PushStyleColor(ed::StyleColor_NodeBorder, ImColor(255, 170, 0, 160));
        ed::BeginNode(id);
        ImGui::PushID(static_cast<int>(i));
        ImGui::TextUnformatted(comment.title.empty() ? "Comment" : comment.title.c_str());
        ed::Group(ImVec2(comment.rect[2], comment.rect[3]));
        ImGui::PopID();
        ed::EndNode();
        ed::PopStyleColor(2);
    }
}

void ShaderGraphEditor::drawNodes()
{
    const kast::ShaderGraph& graph = doc_.graph();
    std::set<std::pair<int, std::string>> linkedInputs;
    std::set<std::pair<int, std::string>> linkedOutputs;
    for (const GraphLink& link : graph.links) {
        linkedInputs.emplace(link.to.node, link.to.pin);
        linkedOutputs.emplace(link.from.node, link.from.pin);
    }
    const float pin = ImGui::GetTextLineHeight() * 0.6f;
    // A pin's colored dot (filled when linked), the link's anchor.
    const auto dot = [&](PinType type, bool linked) {
        ImGui::Dummy(ImVec2(pin, ImGui::GetTextLineHeight()));
        const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        const ImVec2 c((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        if (linked)
            draw->AddCircleFilled(c, pin * 0.5f, pinColor(type));
        else
            draw->AddCircle(c, pin * 0.5f - 0.75f, pinColor(type), 0, 1.5f);
        ed::PinPivotRect(c, c);
    };
    for (const GraphNode& node : graph.nodes) {
        if (ed::GetNodePosition(node.id) != vec(node.position))
            ed::SetNodePosition(node.id, vec(node.position));
        const GraphNodeDef* def = kast::graphNodeDef(node);
        const GraphNodeDef* base = kast::findGraphNodeDef(node.type);
        std::string heading = base ? std::string(base->title) : node.type;
        if (node.type == "param")
            heading += ": " + node.settings.at("param").get<std::string>();
        else if (node.type == "swizzle")
            heading += " ." + node.settings.at("mask").get<std::string>();
        ed::BeginNode(node.id);
        ImGui::PushID(node.id);
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32_BLACK);
        ImGui::TextUnformatted(heading.c_str());
        ImGui::PopStyleColor();
        const float headerBottom = ImGui::GetItemRectMax().y + 2.0f;
        const float headerWidth = ImGui::GetItemRectSize().x;
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        if (def) {
            ImGui::BeginGroup();
            for (size_t i = 0; i < def->inputs.size(); ++i) {
                const GraphPinDef& input = def->inputs[i];
                const bool linked = linkedInputs.contains({node.id, std::string(input.name)});
                ed::BeginPin(pinId(node.id, false, i), ed::PinKind::Input);
                dot(linked ? kast::outputPinType(graph, std::ranges::find_if(graph.links, [&](const GraphLink& l) {
                                                       return l.to.node == node.id && l.to.pin == input.name;
                                                   })->from)
                           : input.type,
                    linked);
                ImGui::SameLine();
                ImGui::TextUnformatted(input.name.data(), input.name.data() + input.name.size());
                ed::EndPin();
            }
            ImGui::EndGroup();
            float widest = 0.0f;
            for (const auto& [output, code] : def->outputs)
                widest = std::max(widest, ImGui::CalcTextSize(output.name.data(), output.name.data() + output.name.size()).x);
            if (!def->outputs.empty()) {
                ImGui::SameLine(0.0f, def->inputs.empty() ? 0.0f : 24.0f);
                ImGui::BeginGroup();
                const float left = ImGui::GetCursorPosX();
                for (size_t i = 0; i < def->outputs.size(); ++i) {
                    const GraphPinDef& output = def->outputs[i].first;
                    const float width = ImGui::CalcTextSize(output.name.data(), output.name.data() + output.name.size()).x;
                    ImGui::SetCursorPosX(left + std::max(widest, headerWidth - (def->inputs.empty() ? 0.0f : 60.0f)) - width);
                    ed::BeginPin(pinId(node.id, true, i), ed::PinKind::Output);
                    ImGui::TextUnformatted(output.name.data(), output.name.data() + output.name.size());
                    ImGui::SameLine();
                    dot(kast::outputPinType(graph, {node.id, std::string(output.name)}),
                        linkedOutputs.contains({node.id, std::string(output.name)}));
                    ed::EndPin();
                }
                ImGui::EndGroup();
            }
        } else {
            ImGui::TextColored(kErrorColor, "invalid settings");
        }
        ImGui::PopID();
        ed::EndNode();
        // The header strip, under the title.
        const ImVec2 min = ed::GetNodePosition(node.id);
        const ImVec2 size = ed::GetNodeSize(node.id);
        ed::GetNodeBackgroundDrawList(node.id)->AddRectFilled(
            ImVec2(min.x + 1.0f, min.y + 1.0f), ImVec2(min.x + size.x - 1.0f, headerBottom), kHeader,
            ed::GetStyle().NodeRounding, ImDrawFlags_RoundCornersTop);
    }
}

void ShaderGraphEditor::syncFromCanvas()
{
    const kast::ShaderGraph& graph = doc_.graph();
    std::vector<std::pair<int, std::array<float, 2>>> moved;
    for (const GraphNode& node : graph.nodes) {
        const ImVec2 at = ed::GetNodePosition(node.id);
        const std::array position{std::round(at.x), std::round(at.y)};
        if (at.x != FLT_MAX && position != node.position)
            moved.emplace_back(node.id, position);
    }
    std::vector<std::pair<size_t, std::array<float, 4>>> frames;
    for (size_t i = 0; i < graph.comments.size(); ++i) {
        const ImVec2 at = ed::GetNodePosition(commentId(i));
        const ImVec2 size = groupSize(commentId(i));
        const std::array rect{std::round(at.x), std::round(at.y), std::round(size.x), std::round(size.y)};
        if (at.x != FLT_MAX && size.x >= 0.0f && rect != graph.comments[i].rect)
            frames.emplace_back(i, rect);
    }
    if (!moved.empty() || !frames.empty()) {
        if (!doc_.inGesture()) // main ends it once the drag lets go: one step per drag
            doc_.beginGesture("Move");
        doc_.move(moved, frames);
    }
    selection_.clear();
    selectedComments_.clear();
    std::vector<ed::NodeId> selected(static_cast<size_t>(ed::GetSelectedObjectCount()));
    selected.resize(static_cast<size_t>(ed::GetSelectedNodes(selected.data(), static_cast<int>(selected.size()))));
    for (ed::NodeId id : selected) {
        if (id.Get() >= kCommentIds)
            selectedComments_.push_back(id.Get() - kCommentIds);
        else
            selection_.push_back(static_cast<int>(id.Get()));
    }
}

void ShaderGraphEditor::handleCreate()
{
    if (ed::BeginCreate(ImColor(kHighlight), 2.0f)) {
        ed::PinId a, b;
        if (ed::QueryNewLink(&a, &b)) {
            auto start = modelPin(doc_.graph(), a), end = modelPin(doc_.graph(), b);
            if (start && end && !start->second && end->second)
                std::swap(start, end); // dragged from an input
            if (!start || !end || start->second == end->second) {
                tooltip("Link an output to an input", kRefusedColor);
                ed::RejectNewItem(ImColor(kRefusedColor), 2.0f);
            } else if (const kast::LinkCheck check = kast::canLink(doc_.graph(), start->first, end->first); !check.allowed) {
                tooltip("Can't link: " + check.message, kRefusedColor); // why, while the drag is over the pin
                ed::RejectNewItem(ImColor(kRefusedColor), 3.0f);
            } else {
                if (check.warning)
                    tooltip(check.message, kHighlight);
                if (ed::AcceptNewItem(ImColor(check.warning ? kHighlight : pinColor(PinType::Float)), 2.0f)) {
                    std::string error;
                    if (!doc_.link(start->first, end->first, error))
                        kast::log::warn(std::format("Shader Graph: can't link: {}", error));
                }
            }
        }
        ed::PinId from;
        if (ed::QueryNewNode(&from)) {
            tooltip("+ Add a node", kHighlight);
            if (ed::AcceptNewItem())
                if (const auto pin = modelPin(doc_.graph(), from))
                    requestAddMenu(pin->first, !pin->second, mouse_);
        }
    }
    ed::EndCreate();
}

void ShaderGraphEditor::handleDelete()
{
    std::vector<int> nodes;
    std::vector<GraphPin> links;
    std::vector<size_t> comments;
    if (ed::BeginDelete()) {
        ed::LinkId link;
        ed::PinId end;
        while (ed::QueryDeletedLink(&link, nullptr, &end))
            if (ed::AcceptDeletedItem())
                if (const auto pin = modelPin(doc_.graph(), end))
                    links.push_back(pin->first);
        ed::NodeId node;
        while (ed::QueryDeletedNode(&node))
            if (ed::AcceptDeletedItem()) {
                if (node.Get() >= kCommentIds)
                    comments.push_back(node.Get() - kCommentIds);
                else
                    nodes.push_back(static_cast<int>(node.Get()));
            }
    }
    ed::EndDelete();
    if (!nodes.empty() || !links.empty() || !comments.empty())
        doc_.remove(nodes, links, comments);
}

void ShaderGraphEditor::handleShortcuts()
{
    if (!ed::BeginShortcut()) {
        ed::EndShortcut();
        return;
    }
    const bool cut = ed::AcceptCut();
    if (cut || ed::AcceptCopy())
        ImGui::SetClipboardText(doc_.copy(selection_).c_str());
    if (cut)
        doc_.remove(selection_);
    const bool duplicate = ed::AcceptDuplicate();
    if (duplicate || ed::AcceptPaste()) {
        const std::string text = duplicate ? doc_.copy(selection_) : std::string(ImGui::GetClipboardText() ? ImGui::GetClipboardText() : "");
        std::string error;
        const std::vector<int> pasted = doc_.paste(text, mouse_, error);
        if (pasted.empty())
            kast::log::warn(std::format("Shader Graph: can't paste: {}", error));
        else
            pendingSelect_ = pasted;
    }
    ed::EndShortcut();
}

// --- Add menu ---

void ShaderGraphEditor::requestAddMenu(std::optional<GraphPin> from, bool fromInput, std::array<float, 2> at)
{
    addMenuRequested_ = true;
    addMenuAtCenter_ = false;
    addFrom_ = std::move(from);
    addFromInput_ = fromInput;
    addAt_ = at;
    addSearch_.clear();
    addEntries_.clear();
}

std::vector<ShaderGraphEditor::AddEntry> ShaderGraphEditor::addEntries() const
{
    std::vector<AddEntry> entries;
    const kast::ShaderGraph& graph = doc_.graph();
    for (const ShaderParam& param : graph.params)
        entries.push_back({"Param: " + param.name, "Params", "param", {{"param", param.name}}, {}});
    for (const GraphNodeDef& def : kast::shaderGraphNodeDefs())
        if (def.type != "param")
            entries.push_back({std::string(def.title), std::string(def.category), std::string(def.type), {}, {}});
    if (!addFrom_)
        return entries;
    // From a pin: the nodes with a pin canLink takes, tried on the graph with the node added.
    const GraphPin& pin = *addFrom_;
    int nextId = 1;
    for (const GraphNode& node : graph.nodes)
        nextId = std::max(nextId, node.id + 1);
    std::vector<AddEntry> fitting;
    for (AddEntry& entry : entries) {
        kast::ShaderGraph trial = graph;
        GraphNode& node = trial.nodes.emplace_back(GraphNode{nextId, entry.type});
        for (const auto& [name, value] : entry.settings.items())
            node.settings[name] = value;
        const GraphNodeDef* def = kast::graphNodeDef(node);
        if (!def)
            continue;
        // The first pin that links without a warning, else the first that links.
        std::optional<std::string> best;
        const auto consider = [&](std::string_view name, const kast::LinkCheck& check) {
            if (check.allowed && (!best || !check.warning))
                best = std::string(name);
            return check.allowed && !check.warning;
        };
        if (addFromInput_) {
            for (const auto& [output, code] : def->outputs)
                if (consider(output.name, kast::canLink(trial, {nextId, std::string(output.name)}, pin)))
                    break;
        } else {
            for (const GraphPinDef& input : def->inputs)
                if (consider(input.name, kast::canLink(trial, pin, {nextId, std::string(input.name)})))
                    break;
        }
        if (best) {
            entry.pin = *best;
            fitting.push_back(std::move(entry));
        }
    }
    return fitting;
}

void ShaderGraphEditor::drawAddMenu()
{
    ed::Suspend();
    if (std::exchange(addMenuRequested_, false)) {
        addEntries_ = addEntries();
        ImGui::OpenPopup("Add Node");
    }
    if (addMenuAtCenter_ && !ImGui::IsPopupOpen("Add Node"))
        addMenuAtCenter_ = false;
    if (addMenuAtCenter_) {
        const ImVec2 at = ed::CanvasToScreen(vec(addAt_));
        ImGui::SetNextWindowPos(at, ImGuiCond_Always);
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(240.0f, 0.0f), ImVec2(320.0f, 460.0f));
    if (ImGui::BeginPopup("Add Node")) {
        if (addFrom_)
            ImGui::TextDisabled("Nodes that link %s %d.%s", addFromInput_ ? "into" : "from", addFrom_->node,
                                addFrom_->pin.c_str());
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##search", "Search", &addSearch_);
        const AddEntry* picked = nullptr;
        bool comment = false;
        ImGui::BeginChild("entries", ImVec2(0.0f, 360.0f));
        if (!addFrom_ && containsNoCase("Comment frame", addSearch_)) {
            ImGui::SeparatorText("Frame");
            comment = ImGui::Selectable("Comment");
        }
        std::string category;
        for (const AddEntry& entry : addEntries_) {
            if (!containsNoCase(entry.label, addSearch_) && !containsNoCase(entry.type, addSearch_))
                continue;
            if (entry.category != category)
                ImGui::SeparatorText((category = entry.category).c_str());
            if (ImGui::Selectable(entry.label.c_str()))
                picked = &entry;
            ImGui::SetItemTooltip("%s", entry.type.c_str());
        }
        ImGui::EndChild();
        if (picked) {
            doc_.beginGesture("Add " + picked->label); // adding and linking it is one step
            std::string error;
            const int id = doc_.addNode(picked->type, addAt_, picked->settings, &error);
            if (id && addFrom_) {
                if (addFromInput_)
                    doc_.link({id, picked->pin}, *addFrom_, error);
                else
                    doc_.link(*addFrom_, {id, picked->pin}, error);
            }
            doc_.endGesture();
            if (id)
                pendingSelect_ = std::vector{id};
            else
                kast::log::warn(std::format("Shader Graph: can't add {}: {}", picked->label, error));
            ImGui::CloseCurrentPopup();
        }
        if (comment) {
            // Around the selected nodes, else at the cursor.
            std::array<float, 4> rect{addAt_[0], addAt_[1], 300.0f, 200.0f};
            if (!selection_.empty()) {
                ImVec2 lo(FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX);
                for (int id : selection_) {
                    const ImVec2 at = ed::GetNodePosition(id), size = ed::GetNodeSize(id);
                    lo = ImMin(lo, at);
                    hi = ImMax(hi, ImVec2(at.x + size.x, at.y + size.y));
                }
                rect = {std::round(lo.x - 20.0f), std::round(lo.y - 40.0f), std::round(hi.x - lo.x + 40.0f),
                        std::round(hi.y - lo.y + 60.0f)};
            }
            doc_.addComment({"Comment", rect});
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ed::Resume();
}

// --- Graph Inspector ---

std::optional<std::string> ShaderGraphEditor::textField(const char* label, const std::string& key, const std::string& value,
                                                        bool multiline)
{
    std::string& buffer = buffers_[key];
    if (editingKey_ != key) {
        const auto failed = fieldErrors_.find(key);
        buffer = failed != fieldErrors_.end() ? failed->second.first : value;
    }
    if (multiline)
        ImGui::InputTextMultiline(label, &buffer, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 8.0f),
                                  ImGuiInputTextFlags_AllowTabInput);
    else
        ImGui::InputText(label, &buffer);
    if (ImGui::IsItemActive())
        editingKey_ = key;
    if (ImGui::IsItemDeactivated()) {
        editingKey_.clear();
        if (buffer != value || fieldErrors_.contains(key))
            return buffer;
    }
    if (const auto failed = fieldErrors_.find(key); failed != fieldErrors_.end())
        ImGui::TextColored(kErrorColor, "%s", failed->second.second.c_str());
    return std::nullopt;
}

void ShaderGraphEditor::showFieldError(const std::string& key, bool ok, const std::string& error)
{
    if (ok)
        fieldErrors_.erase(key);
    else
        fieldErrors_[key] = {buffers_[key], error};
}

void ShaderGraphEditor::drawInspector()
{
    if (focusStep_ == 2)
        ImGui::SetNextWindowFocus();
    if (ImGui::Begin(title("Graph Inspector", kInspectorWindow).c_str()) && doc_.isOpen()) {
        saveShortcut();
        if (ImGui::Button("Save"))
            save();
        ImGui::SameLine();
        ImGui::TextDisabled("%zu nodes, %zu links", doc_.graph().nodes.size(), doc_.graph().links.size());
        ImGui::Separator();
        const kast::ShaderGraph& graph = doc_.graph();
        if (selection_.size() == 1 && selectedComments_.empty()) {
            if (const GraphNode* node = findNode(graph, selection_[0]))
                drawNodeInspector(*node);
        } else if (selection_.empty() && selectedComments_.size() == 1 && selectedComments_[0] < graph.comments.size()) {
            const size_t index = selectedComments_[0];
            GraphComment comment = graph.comments[index];
            ImGui::TextUnformatted("Comment frame");
            if (const auto text = textField("Title", std::format("comment.{}", index), comment.title)) {
                comment.title = *text;
                doc_.setComment(index, comment);
            }
            ImGui::TextDisabled("Drag its edges to resize it; dragging it moves the nodes inside.");
        } else if (!selection_.empty() || !selectedComments_.empty()) {
            ImGui::TextDisabled("%zu nodes and %zu frames selected.", selection_.size(), selectedComments_.size());
        } else {
            drawParams();
        }
    } else if (!doc_.isOpen()) {
        ImGui::TextDisabled("No shader graph open.");
    }
    ImGui::End();
}

void ShaderGraphEditor::drawNodeInspector(const GraphNode& node)
{
    const GraphNodeDef* base = kast::findGraphNodeDef(node.type);
    if (!base)
        return;
    ImGui::Text("%.*s", static_cast<int>(base->title.size()), base->title.data());
    ImGui::TextDisabled("node %d (%s) - %s", node.id, node.type.c_str(), stageList(base->stages).c_str());
    const auto key = [&](std::string_view what) { return std::format("{}.{}", node.id, what); };
    // A number field: one step from activation to release.
    const auto numbers = [&](const char* label, std::vector<float> values, bool color, const std::string& step, auto set) {
        bool changed = false;
        if (color && values.size() == 4)
            changed = ImGui::ColorEdit4(label, values.data(), ImGuiColorEditFlags_Float);
        else
            changed = ImGui::DragScalarN(label, ImGuiDataType_Float, values.data(), static_cast<int>(values.size()), 0.01f);
        if (ImGui::IsItemActive() && !doc_.inGesture())
            doc_.beginGesture(step);
        if (changed)
            set(values);
        if (ImGui::IsItemDeactivated())
            doc_.endGesture();
    };
    if (!base->settings.empty())
        ImGui::SeparatorText("Settings");
    for (const kast::GraphSettingDef& setting : base->settings) {
        const std::string name(setting.name);
        const nlohmann::ordered_json& value = node.settings.at(name);
        std::string error;
        if (name == "param") {
            const std::string current = value.get<std::string>();
            if (ImGui::BeginCombo("param", current.empty() ? "(none)" : current.c_str())) {
                for (const ShaderParam& param : doc_.graph().params)
                    if (ImGui::Selectable(param.name.c_str(), param.name == current))
                        doc_.setSetting(node.id, name, param.name, error);
                ImGui::EndCombo();
            }
        } else if (setting.count == 0) {
            if (const auto text = textField(name.c_str(), key(name), value.get<std::string>(), name == "code"))
                showFieldError(key(name), doc_.setSetting(node.id, name, *text, error), error);
        } else {
            std::vector<float> values = value.is_array() ? value.get<std::vector<float>>() : std::vector{value.get<float>()};
            numbers(name.c_str(), values, node.type == "constant_color", std::format("Edit {} {}", base->title, name),
                    [&](const std::vector<float>& v) {
                        doc_.setSetting(node.id, name, v.size() == 1 ? nlohmann::ordered_json(v[0]) : nlohmann::ordered_json(v),
                                        error);
                    });
        }
    }
    if (node.type == kast::kCustomExpression)
        ImGui::TextDisabled("Pins: \"vec3 a, float b\" (inputs may be texture2D). The code assigns the outputs.");
    const GraphNodeDef* def = kast::graphNodeDef(node);
    if (!def || def->output || def->inputs.empty())
        return;
    ImGui::SeparatorText("Inputs");
    for (const GraphPinDef& input : def->inputs) {
        const std::string name(input.name);
        const bool linked = std::ranges::any_of(doc_.graph().links, [&](const GraphLink& l) {
            return l.to.node == node.id && l.to.pin == name;
        });
        ImGui::PushID(name.c_str());
        const auto constant = node.inputs.find(name);
        const std::optional<float> fallback = numericFallback(input.fallback);
        if (linked) {
            ImGui::TextDisabled("%s: linked", name.c_str());
        } else if (input.type == PinType::Texture2D) {
            ImGui::TextDisabled("%s: link a texture2D", name.c_str());
        } else if (constant == node.inputs.end() && !fallback) {
            ImGui::TextDisabled("%s: %.*s when unlinked", name.c_str(), static_cast<int>(input.fallback.size()),
                                input.fallback.data());
        } else {
            const size_t width = input.type == PinType::Dynamic
                                     ? (constant != node.inputs.end() ? constant->second.size() : 1)
                                     : static_cast<size_t>(input.type) + 1;
            std::vector<float> values = constant != node.inputs.end() ? constant->second : std::vector(width, *fallback);
            std::string error;
            numbers(name.c_str(), values, false, std::format("Edit {}", name),
                    [&](const std::vector<float>& v) { doc_.setInput(node.id, name, v, error); });
            ImGui::SetItemTooltip("%s, used while nothing is linked", pinTypeLabel(input.type));
            if (constant != node.inputs.end()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Reset"))
                    doc_.setInput(node.id, name, {}, error);
            }
        }
        ImGui::PopID();
    }
}

void ShaderGraphEditor::drawParams()
{
    ImGui::SeparatorText("Params");
    ImGui::TextDisabled("Select a node to edit it. Params are the .kshader's, set per material in the Inspector.");
    const auto& params = doc_.graph().params;
    std::optional<size_t> remove;
    for (size_t i = 0; i < params.size(); ++i) {
        ShaderParam param = params[i];
        ImGui::PushID(static_cast<int>(i));
        std::string error;
        const std::string nameKey = std::format("param.{}.name", i);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
        if (const auto name = textField("##name", nameKey, param.name)) {
            param.name = *name;
            showFieldError(nameKey, doc_.setParam(i, param, error), error);
            param = params[i];
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        if (ImGui::BeginCombo("##type", kast::shaderParamTypeName(param.type).data())) {
            for (ShaderParamType type : {ShaderParamType::Float, ShaderParamType::Vec2, ShaderParamType::Vec3,
                                         ShaderParamType::Vec4, ShaderParamType::Color, ShaderParamType::Texture2D})
                if (ImGui::Selectable(kast::shaderParamTypeName(type).data(), type == param.type) && type != param.type) {
                    ShaderParam retyped{.name = param.name, .type = type};
                    if (type == ShaderParamType::Color)
                        retyped.value = {1.0f, 1.0f, 1.0f, 1.0f};
                    doc_.setParam(i, retyped, error);
                }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove"))
            remove = i;
        // The default, one step from activation to release.
        const auto edited = [&](bool changed) {
            if (ImGui::IsItemActive() && !doc_.inGesture())
                doc_.beginGesture("Edit Param");
            if (changed)
                doc_.setParam(i, param, error);
            if (ImGui::IsItemDeactivated())
                doc_.endGesture();
        };
        if (param.type == ShaderParamType::Texture2D) {
            constexpr std::pair<kast::TextureDefault, const char*> kDefaults[] = {
                {kast::TextureDefault::White, "white"}, {kast::TextureDefault::Black, "black"}, {kast::TextureDefault::Normal, "normal"}};
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
            const auto shown = std::ranges::find_if(kDefaults, [&](const auto& d) { return d.first == param.texture; });
            if (ImGui::BeginCombo("default", shown->second)) {
                for (const auto& [value, label] : kDefaults)
                    if (ImGui::Selectable(label, value == param.texture)) {
                        param.texture = value;
                        doc_.setParam(i, param, error);
                    }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Checkbox("sRGB", &param.srgb))
                doc_.setParam(i, param, error);
        } else if (param.type == ShaderParamType::Color) {
            edited(ImGui::ColorEdit4("default", param.value.data(), ImGuiColorEditFlags_Float));
        } else {
            edited(ImGui::DragScalarN("default", ImGuiDataType_Float, param.value.data(), kast::componentCount(param.type), 0.01f));
        }
        if (param.type == ShaderParamType::Float) {
            bool ranged = param.range.has_value();
            if (ImGui::Checkbox("range", &ranged)) {
                param.range = ranged ? std::optional(std::array{0.0f, 1.0f}) : std::nullopt;
                doc_.setParam(i, param, error);
            }
            if (param.range) {
                ImGui::SameLine();
                edited(ImGui::DragFloat2("##range", param.range->data(), 0.01f));
            }
        }
        if (!error.empty())
            ImGui::TextColored(kErrorColor, "%s", error.c_str());
        ImGui::Separator();
        ImGui::PopID();
    }
    if (remove)
        doc_.removeParam(*remove);
    if (ImGui::Button("+ Param"))
        ImGui::OpenPopup("Add Param");
    if (ImGui::BeginPopup("Add Param")) {
        for (ShaderParamType type : {ShaderParamType::Float, ShaderParamType::Vec2, ShaderParamType::Vec3,
                                     ShaderParamType::Vec4, ShaderParamType::Color, ShaderParamType::Texture2D})
            if (ImGui::MenuItem(kast::shaderParamTypeName(type).data()))
                doc_.addParam(type);
        ImGui::EndPopup();
    }
}

// --- Modals ---

void ShaderGraphEditor::drawModals()
{
    if (focusStep_ > 0)
        --focusStep_;
    const std::string name = utf8(doc_.file().filename());
    if (std::exchange(openPromptRequested_, false))
        ImGui::OpenPopup("Unsaved shader graph changes##open");
    if (ImGui::BeginPopupModal("Unsaved shader graph changes##open", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Save changes to %s before opening %s?", name.c_str(), utf8(pendingOpen_.filename()).c_str());
        ImGui::Spacing();
        const bool saveIt = ImGui::Button("Save");
        ImGui::SameLine();
        const bool discard = ImGui::Button("Discard");
        ImGui::SameLine();
        const bool cancel = ImGui::Button("Cancel");
        if ((saveIt && save()) || discard) {
            if (load(pendingOpen_))
                focusPanels();
        }
        if (saveIt || discard || cancel)
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (std::exchange(reloadPromptRequested_, false))
        ImGui::OpenPopup("Shader graph changed on disk");
    if (ImGui::BeginPopupModal("Shader graph changed on disk", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s changed on disk, and the Shader Graph has unsaved changes.", name.c_str());
        ImGui::Spacing();
        if (ImGui::Button("Reload")) {
            load(std::filesystem::path(doc_.file()));
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep mine"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (std::exchange(quitPromptRequested_, false))
        ImGui::OpenPopup("Unsaved shader graph changes##quit");
    if (ImGui::BeginPopupModal("Unsaved shader graph changes##quit", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Save changes to %s before quitting?", name.c_str());
        ImGui::Spacing();
        const bool saveIt = ImGui::Button("Save");
        ImGui::SameLine();
        const bool discard = ImGui::Button("Discard");
        ImGui::SameLine();
        const bool cancel = ImGui::Button("Cancel");
        if ((saveIt && save()) || discard) {
            quitConfirmed_ = true;
            kast::requestQuit(); // asks again; the next prompt (GUI Builder, scene) comes if it is dirty
        }
        if (saveIt || discard || cancel)
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
