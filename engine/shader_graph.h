#pragma once

#include "shader.h"

#include <nlohmann/json.hpp>

#include <array>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Shader graphs: .kshadergraph files (strict JSON, docs/engine/shader-graph.html), node graphs that generate a .kshader's
// text (generateKShader). Everything after that is the .kshader path: ShaderLibrary, the compile cache, Build and the
// player all take a graph's generated text. GPU-free.
namespace kast {

constexpr int kShaderGraphVersion = 1;
constexpr std::string_view kShaderGraphExtension = ".kshadergraph";

// A pin's value type. Dynamic (math node inputs and outputs): the widest number type among the node's dynamic inputs.
enum class PinType { Float, Vec2, Vec3, Vec4, Texture2D, Dynamic };
// The functions a node can be emitted into, as a bit mask.
enum GraphStage : unsigned { kStageVertex = 1, kStageFragment = 2, kStageLight = 4, kStageAll = 7 };

struct GraphPinDef {
    std::string_view name;
    PinType type = PinType::Float;
    // An input's code when nothing is linked and the node has no constant for it ("0.0", "UV"); empty: required.
    std::string_view fallback = {};
};

// A node setting: a string (count 0) or that many numbers (constant values). fallback: a string setting's default.
struct GraphSettingDef {
    std::string_view name;
    int count = 0;
    std::string_view fallback = {};
};

// What a node type is. Each output is a temp variable of the generated code, from code with "{pin}" replaced by an
// input's (or an earlier output's, or a setting's) code.
struct GraphNodeDef {
    std::string_view type; // the id .kshadergraph files use, e.g. "multiply"
    std::string_view title;
    std::string_view category;
    unsigned stages = kStageAll; // where it may be emitted
    std::vector<GraphPinDef> inputs;
    std::vector<std::pair<GraphPinDef, std::string_view>> outputs; // pin, code
    std::vector<GraphSettingDef> settings; // {"param"}, {"value", 3}
    unsigned output = 0; // an output node: the stage whose built-ins its inputs write (0: not one)
    std::string_view helper = {}; // functions its code calls, written once before fragment() when the node is used
};

// Every node type, and one by its id (null if unknown).
const std::vector<GraphNodeDef>& shaderGraphNodeDefs();
const GraphNodeDef* findGraphNodeDef(std::string_view type);

// The Custom Expression node: typed pins from its "inputs" / "outputs" settings ("vec3 a, float b"; inputs may also be
// texture2D) and a GLSL "code" body that assigns the outputs. The body is .kshader source, only ever handed to shaderc.
constexpr std::string_view kCustomExpression = "custom_expression";

struct GraphNode {
    int id = 0; // > 0, unique in the graph
    std::string type;
    std::array<float, 2> position{};
    std::map<std::string, nlohmann::ordered_json> settings; // per GraphNodeDef::settings
    std::map<std::string, std::vector<float>> inputs;        // constants for unlinked number inputs: 1 to 4 numbers
};

struct GraphPin {
    int node = 0;
    std::string pin;
    bool operator==(const GraphPin&) const = default;
};

struct GraphLink {
    GraphPin from; // an output
    GraphPin to;   // an input; at most one link per input
};

struct GraphComment {
    std::string title;
    std::array<float, 4> rect{}; // x, y, width, height on the canvas
};

struct ShaderGraph {
    std::vector<ShaderParam> params; // the .kshader's params, in order (line and slot unused)
    std::vector<GraphNode> nodes;
    std::vector<GraphLink> links;
    std::vector<GraphComment> comments;
};

// A node's type with its pins: findGraphNodeDef's, except a Custom Expression's pins come from its settings. Null for an
// unknown type or settings that are wrong (graphNodeSettingsError says why).
const GraphNodeDef* graphNodeDef(const GraphNode& node);
// What is wrong with a node's settings, as {setting, message} ({"mask", "must be 1 to 4 of x, y, z, w ..."}); empty if
// nothing (a swizzle mask, a noise kind, a Custom Expression's pin lists).
std::pair<std::string, std::string> graphNodeSettingsError(const GraphNode& node);

// False + error ("nodes[2].inputs.b: ...", "node 4: the links form a cycle") if json is not a valid graph: unknown keys,
// a wrong version, bad values, unknown node types or pins, duplicate ids, texture / number mismatches, cycles. Missing
// optional fields get their defaults.
bool shaderGraphFromJson(const nlohmann::ordered_json& json, ShaderGraph& out, std::string& error);
bool parseShaderGraph(std::string_view text, ShaderGraph& out, std::string& error);
// Keys in a fixed order and every field written: saving what parseShaderGraph read gives the same bytes.
nlohmann::ordered_json shaderGraphToJson(const ShaderGraph& graph);
std::string shaderGraphText(const ShaderGraph& graph); // the file: shaderGraphToJson, 2-space indent, '\n' at the end

// A node id (0: the graph itself, e.g. a param no Param node uses) and what is wrong there.
struct GraphIssue {
    int node = 0;
    std::string message;
    bool warning = false; // generates anyway (e.g. a vec4 truncated into a float pin)
    bool operator==(const GraphIssue&) const = default;
};

// The type an output pin has in graph (Dynamic and Param outputs resolved); Dynamic if the pin doesn't exist.
PinType outputPinType(const ShaderGraph& graph, const GraphPin& pin);

// Whether a link from an output to an input may be made. Refused: no such pins, a texture into a number or a number into
// a texture, or a cycle. Allowed with a warning: a wider vector into a narrower pin (truncated). A link the input already
// has doesn't count: linking replaces it.
struct LinkCheck {
    bool allowed = false;
    bool warning = false;
    std::string message; // why refused, or the warning
};
LinkCheck canLink(const ShaderGraph& graph, const GraphPin& from, const GraphPin& to);

// The .kshader text of graph: its params, its nodes' helper functions, then fragment(), then vertex() and light() only
// when their output node has a linked input. A temp variable per node output in topological order (ties by id) after a
// "// @node <id> <type>" line, so the text is the same for the same graph in any node order. A Custom Expression's code
// goes in a block of its own after its marker. Empty if issues gets an error (warnings still generate).
std::string generateKShader(const ShaderGraph& graph, std::vector<GraphIssue>& issues);
// The node whose code is at a line (from 1) of generateKShader's text; 0 outside every node (params, signatures).
int graphNodeAtLine(std::string_view kshaderText, int line);
// The same node as "node <id> (<title>)", or "" outside every node.
std::string graphNodeNameAtLine(std::string_view kshaderText, int line);
// errors (mapShaderErrors' "line <n>: ..." lines, for generateKShader's text) with "node <id>: " before each mapped line.
std::string mapGraphErrors(std::string_view kshaderText, std::string_view errors);

bool isShaderGraphPath(std::string_view path);
// A shader file's .kshader text, in place: a .kshader's as it is, a .kshadergraph's generated. False + error (the JSON
// error, or "node <id>: <message>" lines) for a graph that doesn't parse or validate; warnings get its warnings.
bool shaderFileText(std::string_view path, std::string& text, std::string& error, std::vector<std::string>* warnings = nullptr);

} // namespace kast
