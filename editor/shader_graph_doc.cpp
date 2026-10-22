#include "shader_graph_doc.h"

#include "assets.h" // remapPath, writeTextFile

#include <files.h>

#include <algorithm>
#include <format>
#include <map>
#include <regex>
#include <set>

using kast::GraphComment;
using kast::GraphLink;
using kast::GraphNode;
using kast::GraphNodeDef;
using kast::GraphPin;
using kast::GraphPinDef;
using kast::GraphSettingDef;
using kast::LinkCheck;
using kast::PinType;
using kast::ShaderGraph;
using kast::ShaderParam;
using kast::ShaderParamType;

namespace {

using Json = nlohmann::ordered_json;

// A number pin's width (1..4); 0 for a texture. Dynamic takes 1 to 4.
int pinWidth(PinType type)
{
    return type == PinType::Texture2D ? 0 : type == PinType::Dynamic ? 1 : static_cast<int>(type) + 1;
}

std::filesystem::file_time_type modified(const std::filesystem::path& file)
{
    std::error_code ec;
    return file.empty() ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(file, ec);
}

int nextNodeId(const ShaderGraph& graph)
{
    int id = 0;
    for (const GraphNode& node : graph.nodes)
        id = std::max(id, node.id);
    return id + 1;
}

} // namespace

bool ShaderGraphDoc::open(const std::filesystem::path& file, std::string& error)
{
    std::string text;
    ShaderGraph graph;
    if (!kast::readFile(file, text)) {
        error = "cannot read it";
        return false;
    }
    if (!kast::parseShaderGraph(text, graph, error))
        return false;
    file_ = file;
    graph_ = std::move(graph);
    ++revision_;
    gesture_.reset();
    history_.clear(true);
    rememberFileTime();
    return true;
}

void ShaderGraphDoc::close()
{
    file_.clear();
    graph_ = {};
    ++revision_;
    gesture_.reset();
    history_.clear(true);
}

bool ShaderGraphDoc::save(std::string& error)
{
    endGesture(); // the save point comes after a field edit in progress
    if (!writeTextFile(file_, kast::shaderGraphText(graph_), error))
        return false;
    history_.markSaved();
    rememberFileTime();
    return true;
}

ShaderGraphDoc::DiskChange ShaderGraphDoc::poll()
{
    if (!isOpen())
        return DiskChange::None;
    const auto time = modified(file_);
    if (time == fileTime_)
        return DiskChange::None;
    fileTime_ = time; // asked once per change
    std::error_code ec;
    if (!std::filesystem::exists(file_, ec)) {
        if (!dirty())
            close();
        return DiskChange::Removed; // dirty: kept open, Save writes it back
    }
    if (dirty())
        return DiskChange::Conflict;
    std::string error;
    return open(std::filesystem::path(file_), error) ? DiskChange::Reloaded : DiskChange::None;
}

void ShaderGraphDoc::remap(const std::filesystem::path& from, const std::filesystem::path& to)
{
    if (const auto moved = isOpen() ? remapPath(file_, from, to) : std::nullopt) {
        file_ = *moved;
        rememberFileTime();
    }
}

void ShaderGraphDoc::rememberFileTime() { fileTime_ = modified(file_); }

void ShaderGraphDoc::undo()
{
    endGesture();
    history_.undo();
    ++revision_;
}

void ShaderGraphDoc::redo()
{
    endGesture();
    history_.redo();
    ++revision_;
}

void ShaderGraphDoc::beginGesture(std::string name)
{
    endGesture();
    gesture_ = Gesture{std::move(name), graph_};
}

void ShaderGraphDoc::endGesture()
{
    if (std::optional<Gesture> gesture = std::exchange(gesture_, std::nullopt))
        commit(std::move(gesture->name), std::move(gesture->before));
}

bool ShaderGraphDoc::commit(std::string name, ShaderGraph before)
{
    if (gesture_)
        return true; // endGesture records it
    if (kast::shaderGraphText(before) == kast::shaderGraphText(graph_))
        return false;
    history_.push({std::move(name), [this, before] { graph_ = before; }, [this, after = graph_] { graph_ = after; }});
    return true;
}

bool ShaderGraphDoc::apply(std::string name, ShaderGraph trial, std::string& error)
{
    // ponytail: every edit re-validates the whole graph through its JSON (a few hundred nodes is still well under a
    // millisecond); validate only the touched nodes if graphs get huge.
    ShaderGraph checked;
    if (!kast::shaderGraphFromJson(kast::shaderGraphToJson(trial), checked, error))
        return false;
    ShaderGraph before = std::exchange(graph_, std::move(checked));
    ++revision_;
    commit(std::move(name), std::move(before));
    return true;
}

GraphNode* ShaderGraphDoc::find(ShaderGraph& graph, int id)
{
    const auto it = std::ranges::find(graph.nodes, id, &GraphNode::id);
    return it == graph.nodes.end() ? nullptr : &*it;
}

void ShaderGraphDoc::dropBroken(ShaderGraph& graph)
{
    for (GraphNode& node : graph.nodes) {
        const GraphNodeDef* def = kast::graphNodeDef(node);
        if (!def)
            continue;
        std::erase_if(node.inputs, [&](const auto& input) {
            const auto pin = std::ranges::find(def->inputs, input.first, &GraphPinDef::name);
            if (pin == def->inputs.end() || pin->type == PinType::Texture2D)
                return true;
            const size_t width = input.second.size();
            return pin->type == PinType::Dynamic ? width < 1 || width > 4 : width != static_cast<size_t>(pinWidth(pin->type));
        });
    }
    // One at a time: canLink judges each against the links still there.
    for (size_t i = graph.links.size(); i-- > 0;) {
        const GraphLink link = graph.links[i];
        graph.links.erase(graph.links.begin() + static_cast<std::ptrdiff_t>(i));
        if (kast::canLink(graph, link.from, link.to).allowed)
            graph.links.insert(graph.links.begin() + static_cast<std::ptrdiff_t>(i), link);
    }
}

int ShaderGraphDoc::addNode(std::string_view type, std::array<float, 2> position, const Json& settings, std::string* error)
{
    // A one-node graph through the strict parse: the type's default settings, and the given ones checked.
    const int id = nextNodeId(graph_);
    Json node = {{"id", id}, {"type", type}, {"position", Json::array({position[0], position[1]})}};
    if (settings.is_object())
        node["settings"] = settings;
    ShaderGraph one;
    std::string why;
    if (!kast::shaderGraphFromJson({{"version", kast::kShaderGraphVersion}, {"nodes", Json::array({node})}}, one, why)) {
        if (error)
            *error = why;
        return 0;
    }
    ShaderGraph trial = graph_;
    trial.nodes.push_back(std::move(one.nodes[0]));
    const GraphNodeDef* def = kast::findGraphNodeDef(type);
    return apply(std::format("Add {}", def->title), std::move(trial), why) ? id : 0;
}

bool ShaderGraphDoc::link(const GraphPin& from, const GraphPin& to, std::string& error)
{
    const LinkCheck check = kast::canLink(graph_, from, to);
    if (!check.allowed) {
        error = check.message;
        return false;
    }
    ShaderGraph trial = graph_;
    std::erase_if(trial.links, [&](const GraphLink& link) { return link.to == to; });
    trial.links.push_back({from, to});
    return apply("Link", std::move(trial), error);
}

bool ShaderGraphDoc::unlink(const GraphPin& to)
{
    ShaderGraph trial = graph_;
    if (!std::erase_if(trial.links, [&](const GraphLink& link) { return link.to == to; }))
        return false;
    std::string error;
    return apply("Unlink", std::move(trial), error);
}

bool ShaderGraphDoc::remove(const std::vector<int>& nodes, const std::vector<GraphPin>& links, std::vector<size_t> comments)
{
    ShaderGraph trial = graph_;
    const std::set<int> gone(nodes.begin(), nodes.end());
    std::erase_if(trial.nodes, [&](const GraphNode& node) { return gone.contains(node.id); });
    std::erase_if(trial.links, [&](const GraphLink& link) {
        return gone.contains(link.from.node) || gone.contains(link.to.node) || std::ranges::find(links, link.to) != links.end();
    });
    std::ranges::sort(comments, std::greater{});
    for (size_t index : comments)
        if (index < trial.comments.size())
            trial.comments.erase(trial.comments.begin() + static_cast<std::ptrdiff_t>(index));
    std::string error;
    return apply("Delete", std::move(trial), error);
}

bool ShaderGraphDoc::move(const std::vector<std::pair<int, std::array<float, 2>>>& nodes,
                          const std::vector<std::pair<size_t, std::array<float, 4>>>& comments)
{
    ShaderGraph trial = graph_;
    for (const auto& [id, position] : nodes)
        if (GraphNode* node = find(trial, id))
            node->position = position;
    for (const auto& [index, rect] : comments)
        if (index < trial.comments.size())
            trial.comments[index].rect = rect;
    std::string error;
    return apply("Move", std::move(trial), error);
}

bool ShaderGraphDoc::setSetting(int node, const std::string& name, const Json& value, std::string& error)
{
    ShaderGraph trial = graph_;
    GraphNode* target = find(trial, node);
    const GraphNodeDef* def = target ? kast::findGraphNodeDef(target->type) : nullptr;
    if (!def || std::ranges::none_of(def->settings, [&](const GraphSettingDef& s) { return s.name == name; })) {
        error = std::format("node {} has no setting '{}'", node, name);
        return false;
    }
    target->settings[name] = value;
    if (const auto [setting, message] = kast::graphNodeSettingsError(*target); !setting.empty()) {
        error = std::format("{}: {}", setting, message);
        return false;
    }
    dropBroken(trial); // a Custom Expression's pins may have changed
    return apply(std::format("Edit {} {}", def->title, name), std::move(trial), error);
}

bool ShaderGraphDoc::setInput(int node, const std::string& pin, const std::vector<float>& values, std::string& error)
{
    ShaderGraph trial = graph_;
    GraphNode* target = find(trial, node);
    if (!target) {
        error = std::format("no node {}", node);
        return false;
    }
    if (values.empty())
        target->inputs.erase(pin);
    else
        target->inputs[pin] = values;
    return apply(std::format("Edit {}", pin), std::move(trial), error);
}

size_t ShaderGraphDoc::addParam(ShaderParamType type)
{
    ShaderGraph trial = graph_;
    ShaderParam param;
    param.name = "param";
    for (int n = 2; std::ranges::any_of(trial.params, [&](const ShaderParam& p) { return p.name == param.name; }); ++n)
        param.name = std::format("param_{}", n);
    param.type = type;
    if (type == ShaderParamType::Color)
        param.value = {1.0f, 1.0f, 1.0f, 1.0f};
    trial.params.push_back(param);
    std::string error;
    apply("Add Param", std::move(trial), error);
    return graph_.params.size() - 1;
}

bool ShaderGraphDoc::setParam(size_t index, const ShaderParam& param, std::string& error)
{
    static const std::regex identifier("^[A-Za-z_][A-Za-z0-9_]*$");
    if (index >= graph_.params.size()) {
        error = std::format("no param {}", index);
        return false;
    }
    if (!std::regex_match(param.name, identifier)) {
        error = "a param name is letters, digits and _, not starting with a digit";
        return false;
    }
    for (size_t i = 0; i < graph_.params.size(); ++i)
        if (i != index && graph_.params[i].name == param.name) {
            error = std::format("there already is a param '{}'", param.name);
            return false;
        }
    ShaderGraph trial = graph_;
    const std::string old = trial.params[index].name;
    trial.params[index] = param;
    for (GraphNode& node : trial.nodes)
        if (node.type == "param" && node.settings["param"] == old)
            node.settings["param"] = param.name;
    dropBroken(trial); // a texture2D param can't feed the number pins its Param nodes fed, and back
    return apply("Edit Param", std::move(trial), error);
}

bool ShaderGraphDoc::removeParam(size_t index)
{
    if (index >= graph_.params.size())
        return false;
    ShaderGraph trial = graph_;
    trial.params.erase(trial.params.begin() + static_cast<std::ptrdiff_t>(index));
    std::string error;
    return apply("Remove Param", std::move(trial), error);
}

size_t ShaderGraphDoc::addComment(GraphComment comment)
{
    ShaderGraph trial = graph_;
    trial.comments.push_back(std::move(comment));
    std::string error;
    apply("Add Comment", std::move(trial), error);
    return graph_.comments.size() - 1;
}

bool ShaderGraphDoc::setComment(size_t index, const GraphComment& comment)
{
    if (index >= graph_.comments.size())
        return false;
    ShaderGraph trial = graph_;
    trial.comments[index] = comment;
    std::string error;
    return apply("Edit Comment", std::move(trial), error);
}

std::string ShaderGraphDoc::copy(const std::vector<int>& nodes) const
{
    const std::set<int> picked(nodes.begin(), nodes.end());
    ShaderGraph part;
    for (const GraphNode& node : graph_.nodes)
        if (picked.contains(node.id)) {
            part.nodes.push_back(node);
            if (node.type != "param")
                continue;
            const auto param = std::ranges::find(graph_.params, node.settings.at("param").get<std::string>(), &ShaderParam::name);
            if (param != graph_.params.end() && std::ranges::find(part.params, param->name, &ShaderParam::name) == part.params.end())
                part.params.push_back(*param);
        }
    for (const GraphLink& link : graph_.links)
        if (picked.contains(link.from.node) && picked.contains(link.to.node))
            part.links.push_back(link);
    return kast::shaderGraphText(part);
}

std::vector<int> ShaderGraphDoc::paste(std::string_view text, std::array<float, 2> at, std::string& error)
{
    ShaderGraph part;
    if (!kast::parseShaderGraph(text, part, error) || part.nodes.empty()) {
        error = "the clipboard holds no shader graph nodes";
        return {};
    }
    std::array<float, 2> corner = part.nodes[0].position;
    for (const GraphNode& node : part.nodes)
        corner = {std::min(corner[0], node.position[0]), std::min(corner[1], node.position[1])};
    ShaderGraph trial = graph_;
    std::map<int, int> ids; // copied id -> fresh id
    std::vector<int> added;
    int next = nextNodeId(trial);
    for (GraphNode node : part.nodes) {
        ids[node.id] = next;
        node.id = next++;
        node.position = {node.position[0] - corner[0] + at[0], node.position[1] - corner[1] + at[1]};
        added.push_back(node.id);
        trial.nodes.push_back(std::move(node));
    }
    for (GraphLink link : part.links) {
        link.from.node = ids[link.from.node];
        link.to.node = ids[link.to.node];
        trial.links.push_back(std::move(link));
    }
    for (const ShaderParam& param : part.params)
        if (std::ranges::find(trial.params, param.name, &ShaderParam::name) == trial.params.end())
            trial.params.push_back(param);
    if (!apply("Paste", std::move(trial), error))
        return {};
    return added;
}

ShaderGraph starterShaderGraph()
{
    ShaderGraph graph;
    std::string error;
    // The param is a color (vec4): a Swizzle takes its rgb, so ALBEDO gets no truncation warning.
    kast::parseShaderGraph(R"({"version": 1,
        "params": [{"name": "tint", "type": "color", "default": [1.0, 1.0, 1.0, 1.0]}],
        "nodes": [{"id": 1, "type": "output_fragment", "position": [440.0, 0.0]},
                  {"id": 2, "type": "param", "position": [0.0, 0.0], "settings": {"param": "tint"}},
                  {"id": 3, "type": "swizzle", "position": [220.0, 0.0], "settings": {"mask": "rgb"}}],
        "links": [{"from": [2, "out"], "to": [3, "v"]}, {"from": [3, "out"], "to": [1, "ALBEDO"]}]})",
                           graph, error);
    return graph;
}
