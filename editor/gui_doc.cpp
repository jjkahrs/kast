#include "gui_doc.h"

#include "assets.h"

#include <files.h>
#include <log.h>
#include <project.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <regex>
#include <sstream>

namespace {

constexpr unsigned kParseOptions = pugi::parse_default | pugi::parse_comments | pugi::parse_declaration;
constexpr const char* kIndent = "    ";
constexpr const char* kNodeAttribute = "data-kast-node";

constexpr const char* kGuiRmlTemplate = R"(<rml>
    <head>
        <title>New GUI</title>
        <link type="text/rcss" href="%NAME%.rcss"/>
    </head>
    <body>
        <div id="panel" class="panel">
            <p id="title">Hello, world</p>
            <button id="button" data-event="ui.clicked">Click me</button>
        </div>
    </body>
</rml>
)";

constexpr const char* kGuiRcssTemplate = R"(/* RmlUi has no built-in styles: elements are inline until a rule makes them block. */
body {
    width: 100%;
    height: 100%;
    font-family: Roboto;
    font-size: 18px;
    color: #ffffff;
}

div, p {
    display: block;
}

.panel {
    width: 360px;
    margin: 80px auto;
    padding: 20px;
    background-color: #1e2a3acc;
    border: 2px #4a90d9;
    border-radius: 8px;
}

p {
    margin-bottom: 12px;
}

button {
    display: inline-block;
    padding: 8px 16px;
    background-color: #4a90d9;
    border-radius: 4px;
}

button:hover {
    background-color: #6aa9ec;
}

button:active {
    background-color: #2f6fb0;
}
)";

bool isStylesheetLink(pugi::xml_node node)
{
    return std::string_view(node.name()) == "link" && std::string_view(node.attribute("type").value()) == "text/rcss";
}

bool isInside(pugi::xml_node node, pugi::xml_node ancestor)
{
    for (; node; node = node.parent())
        if (node == ancestor)
            return true;
    return false;
}

void preorder(pugi::xml_node parent, std::vector<pugi::xml_node>& out)
{
    for (pugi::xml_node child : GuiDoc::elements(parent)) {
        out.push_back(child);
        preorder(child, out);
    }
}

std::string readText(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

std::string withName(std::string text, const std::string& name)
{
    for (size_t at; (at = text.find("%NAME%")) != std::string::npos;)
        text.replace(at, 6, name);
    return text;
}

bool hasClass(pugi::xml_node node, std::string_view name)
{
    std::istringstream classes(node.attribute("class").value());
    for (std::string token; classes >> token;)
        if (token == name)
            return true;
    return false;
}

bool isToggle(pugi::xml_node node)
{
    return std::string_view(node.name()) == "input" && std::string_view(node.attribute("type").value()) == "checkbox" &&
           hasClass(node, "toggle");
}

// A <label> whose only element is a Toggle: the Toggle's label is the <label>'s text.
bool labelledToggle(pugi::xml_node node)
{
    const std::vector<pugi::xml_node> children = GuiDoc::elements(node);
    return std::string_view(node.name()) == "label" && children.size() == 1 && isToggle(children[0]);
}

bool isVirtualList(pugi::xml_node node)
{
    const std::vector<pugi::xml_node> children = GuiDoc::elements(node);
    return hasClass(node, "virtuallist") && children.size() == 1 && children[0].attribute("data-for");
}

pugi::xml_node rowTemplate(pugi::xml_node virtualList) { return GuiDoc::elements(virtualList)[0]; }

// Sets or clears class token on node (the class attribute goes when it empties).
void setClassToken(pugi::xml_node node, std::string_view token, bool on)
{
    std::istringstream classes(node.attribute("class").value());
    std::string out;
    for (std::string other; classes >> other;)
        if (other != token)
            out += (out.empty() ? "" : " ") + other;
    if (on)
        out += (out.empty() ? "" : " ") + std::string(token);
    GuiDoc::setAttribute(node, "class", out);
}

// "item : items" -> {"item", "items"}; without a colon, no alias.
std::pair<std::string, std::string> dataFor(std::string_view expression)
{
    const auto trim = [](std::string_view text) {
        const size_t first = text.find_first_not_of(" \t");
        const size_t last = text.find_last_not_of(" \t");
        return first == std::string_view::npos ? std::string() : std::string(text.substr(first, last - first + 1));
    };
    const size_t colon = expression.find(':');
    if (colon == std::string_view::npos)
        return {"", trim(expression)};
    return {trim(expression.substr(0, colon)), trim(expression.substr(colon + 1))};
}

// A Virtual List's template text as a placeholder row: every {{a.b}} read as "b <n>".
std::string placeholderText(std::string_view text, int n)
{
    std::string out;
    for (size_t at = 0;;) {
        const size_t open = text.find("{{", at);
        const size_t close = open == std::string_view::npos ? open : text.find("}}", open + 2);
        if (close == std::string_view::npos) {
            out += text.substr(at);
            return out;
        }
        std::string_view binding = text.substr(open + 2, close - open - 2);
        const size_t dot = binding.find_last_of('.');
        binding = dot == std::string_view::npos ? binding : binding.substr(dot + 1);
        while (!binding.empty() && binding.front() == ' ')
            binding.remove_prefix(1);
        while (!binding.empty() && binding.back() == ' ')
            binding.remove_suffix(1);
        out += std::string(text.substr(at, open - at)) + std::format("{} {}", binding, n);
        at = close + 2;
    }
}

// fn on node and every element under it.
void forEachElement(pugi::xml_node node, const std::function<void(pugi::xml_node)>& fn)
{
    fn(node);
    for (pugi::xml_node child : GuiDoc::elements(node))
        forEachElement(child, fn);
}

// Every text under node as placeholder row n.
void fillPlaceholders(pugi::xml_node node, int n)
{
    for (pugi::xml_node child : node.children()) {
        if (child.type() == pugi::node_pcdata)
            child.set_value(placeholderText(child.value(), n).c_str());
        else if (child.type() == pugi::node_element)
            fillPlaceholders(child, n);
    }
}

bool isFlag(std::string_view property) { return property == "checked" || property == "follow"; }

bool isSpinbox(pugi::xml_node node)
{
    const std::vector<pugi::xml_node> children = GuiDoc::elements(node);
    return hasClass(node, "spinbox") && children.size() == 3 && std::string_view(children[0].name()) == "button" &&
           std::string_view(children[1].name()) == "input" && std::string_view(children[2].name()) == "button";
}

std::string_view kindMarkup(GuiKind kind)
{
    switch (kind) {
    case GuiKind::Panel: return "<div/>";
    case GuiKind::Text: return "<p>Text</p>";
    case GuiKind::RichText: return R"(<p class="rich">[b]Rich[/b] text</p>)";
    case GuiKind::Button: return "<button>Button</button>";
    case GuiKind::Toggle: return R"(<label><input type="checkbox" class="toggle"/>Toggle</label>)";
    case GuiKind::Slider: return R"(<input type="range" min="0" max="100" step="1" value="50"/>)";
    case GuiKind::Dropdown:
        return R"(<select><option value="option1" selected="">Option 1</option><option value="option2">Option 2</option>)"
               R"(<option value="option3">Option 3</option></select>)";
    case GuiKind::Spinbox:
        return R"(<div class="spinbox" data-min="0" data-max="10" data-step="1"><button>-</button>)"
               R"(<input type="text" value="0"/><button>+</button></div>)";
    case GuiKind::Progress: return R"(<progress value="0.5" max="1"/>)";
    case GuiKind::ListBox:
        return R"(<div class="listbox"><div class="row" data-value="row1">Row 1</div>)"
               R"(<div class="row" data-value="row2">Row 2</div><div class="row" data-value="row3">Row 3</div></div>)";
    case GuiKind::VirtualList:
        return R"(<div class="virtuallist" data-model="list" data-row-height="24">)"
               R"(<div class="row" data-for="item : items">{{item.name}}</div></div>)";
    default: return "";
    }
}

// Where a widget property lives: the element and its attribute ("" = the element's text); a null element if node has
// no such property.
std::pair<pugi::xml_node, const char*> propertyPlace(pugi::xml_node node, std::string_view name)
{
    const auto among = [&](std::initializer_list<const char*> names) -> const char* { // the literal name matches
        const auto it = std::ranges::find_if(names, [&](const char* n) { return name == n; });
        return it == names.end() ? nullptr : *it;
    };
    switch (guiKind(node)) {
    case GuiKind::Slider:
        if (const char* attribute = among({"min", "max", "step", "value"}))
            return {node, attribute};
        break;
    case GuiKind::Spinbox:
        if (name == "value")
            return {GuiDoc::elements(node)[1], "value"};
        if (name == "data-value")
            return {GuiDoc::elements(node)[1], "data-value"};
        if (name == "min")
            return {node, "data-min"};
        if (name == "max")
            return {node, "data-max"};
        if (name == "step")
            return {node, "data-step"};
        break;
    case GuiKind::Progress:
        if (const char* attribute = among({"value", "max"}))
            return {node, attribute};
        break;
    case GuiKind::Toggle:
        if (name == "checked")
            return {GuiDoc::widgetElement(node), "checked"};
        if (name == "label" && labelledToggle(node))
            return {node, ""};
        break;
    case GuiKind::ListBox:
        if (name == "follow")
            return {node, "data-follow"};
        break;
    case GuiKind::VirtualList:
        if (name == "model")
            return {node, "data-model"};
        if (name == "key")
            return {node, "data-key"};
        if (name == "row-height")
            return {node, "data-row-height"};
        if (name == "follow")
            return {node, "data-follow"};
        if (name == "source")
            return {rowTemplate(node), "data-for"};
        if (name == "template")
            return {rowTemplate(node), ""};
        break;
    default:
        break;
    }
    return {};
}

// root and every element under it, in preorder.
std::vector<pugi::xml_node> subtree(pugi::xml_node root)
{
    std::vector<pugi::xml_node> out{root};
    preorder(root, out);
    return out;
}

bool transient(std::string_view attribute) { return attribute.starts_with("data-kast-"); }

// node's element child indices from root, "i/j/..." ("" for root itself): an override target without an id.
std::string indexPath(pugi::xml_node node, pugi::xml_node root)
{
    std::string path;
    for (pugi::xml_node at = node; at && at != root; at = at.parent()) {
        const std::vector<pugi::xml_node> siblings = GuiDoc::elements(at.parent());
        const size_t index = static_cast<size_t>(std::ranges::find(siblings, at) - siblings.begin());
        path = path.empty() ? std::to_string(index) : std::format("{}/{}", index, path);
    }
    return path;
}

// Replaces node's own text (its text children), keeping its elements: the engine's text override.
void replaceText(pugi::xml_node node, std::string_view text)
{
    for (pugi::xml_node child = node.first_child(); child;) {
        const pugi::xml_node next = child.next_sibling();
        if (child.type() == pugi::node_pcdata)
            node.remove_child(child);
        child = next;
    }
    if (!text.empty())
        node.append_child(pugi::node_pcdata).set_value(std::string(text).c_str());
}

// One field that differs between an instance and its fresh expansion: the element's preorder index, the attribute ("" =
// its text).
struct Change {
    size_t index = 0;
    std::string attribute;
};

// What the instance current differs in from fresh (both subtree()s of the same prefab expansion): attributes (never the
// root's id, data-prefab or the builder's own data-kast-*) and text, element by element.
std::vector<Change> changes(const std::vector<pugi::xml_node>& current, const std::vector<pugi::xml_node>& fresh)
{
    std::vector<Change> out;
    const auto skipped = [](std::string_view name, size_t index) {
        return transient(name) || name == kast::kGuiPrefabAttribute || (index == 0 && name == "id");
    };
    for (size_t i = 0; i < current.size() && i < fresh.size(); ++i) {
        for (pugi::xml_attribute attribute : current[i].attributes()) {
            const pugi::xml_attribute was = fresh[i].attribute(attribute.name());
            if (!skipped(attribute.name(), i) && (!was || std::string_view(was.value()) != attribute.value()))
                out.push_back({i, attribute.name()});
        }
        for (pugi::xml_attribute attribute : fresh[i].attributes())
            if (!skipped(attribute.name(), i) && !current[i].attribute(attribute.name()))
                out.push_back({i, attribute.name()});
        if (GuiDoc::text(current[i]) != GuiDoc::text(fresh[i]))
            out.push_back({i, ""});
    }
    return out;
}

// A placeholder under node (node included) that didn't expand because it would close a cycle; its reason, else empty.
std::string cycleUnder(pugi::xml_node node)
{
    for (pugi::xml_node element : subtree(node))
        if (const std::string_view reason = element.attribute(kast::kGuiMissingPrefabAttribute).value(); reason.starts_with("a cycle"))
            return std::string(reason);
    return {};
}

void addUnique(std::vector<std::string>& to, const std::vector<std::string>& from)
{
    for (const std::string& item : from)
        if (std::ranges::find(to, item) == to.end())
            to.push_back(item);
}

} // namespace

const char* guiKindLabel(GuiKind kind)
{
    switch (kind) {
    case GuiKind::Panel: return "Panel";
    case GuiKind::Text: return "Text";
    case GuiKind::Button: return "Button";
    case GuiKind::Toggle: return "Toggle";
    case GuiKind::Slider: return "Slider";
    case GuiKind::Dropdown: return "Dropdown";
    case GuiKind::Spinbox: return "Spinbox";
    case GuiKind::Progress: return "Progress";
    case GuiKind::ListBox: return "List Box";
    case GuiKind::VirtualList: return "Virtual List";
    case GuiKind::RichText: return "Rich Text";
    default: return "Unknown";
    }
}

bool guiBindingAttribute(std::string_view name)
{
    static constexpr std::string_view kNames[] = {"data-model", "data-for", "data-if", "data-visible",
                                                  "data-value", "data-checked", "data-rml", "data-text"};
    static constexpr std::string_view kPrefixes[] = {"data-attr-", "data-attrif-", "data-class-",
                                                     "data-style-", "data-event-", "data-alias-"};
    return std::ranges::find(kNames, name) != std::end(kNames) ||
           std::ranges::any_of(kPrefixes, [&](std::string_view p) { return name.size() > p.size() && name.starts_with(p); });
}

GuiKind guiKind(pugi::xml_node node)
{
    const std::string_view tag = node.name();
    if (tag == "div")
        return isSpinbox(node)                                            ? GuiKind::Spinbox
               : isVirtualList(node)                                      ? GuiKind::VirtualList
               : hasClass(node, "listbox") && !hasClass(node, "virtuallist") ? GuiKind::ListBox
                                                                          : GuiKind::Panel;
    if (tag == "p")
        return hasClass(node, "rich") ? GuiKind::RichText : GuiKind::Text;
    if (tag == "button")
        return GuiKind::Button;
    if (tag == "select")
        return GuiKind::Dropdown;
    if (tag == "progress")
        return GuiKind::Progress;
    if (tag == "input" && std::string_view(node.attribute("type").value()) == "range")
        return GuiKind::Slider;
    if (isToggle(node) || labelledToggle(node))
        return GuiKind::Toggle;
    return GuiKind::Unknown;
}

bool GuiDoc::parse(std::string_view rml, std::string& error)
{
    pugi::xml_document parsed;
    const pugi::xml_parse_result result = parsed.load_buffer(rml.data(), rml.size(), kParseOptions);
    if (!result) {
        error = std::format("{} at offset {}", result.description(), result.offset);
        return false;
    }
    if (!parsed.child("rml").child("body")) {
        error = "no <rml><body> element";
        return false;
    }
    xml_.reset(parsed);
    prefab_ = false;
    expandInstances();
    return true;
}

bool GuiDoc::parsePrefab(std::string_view text, std::string& error)
{
    pugi::xml_document parsed;
    const pugi::xml_parse_result result = parsed.load_buffer(text.data(), text.size(), kParseOptions);
    if (!result) {
        error = std::format("{} at offset {}", result.description(), result.offset);
        return false;
    }
    if (const size_t count = elements(parsed).size(); count != 1) {
        error = std::format("a GUI prefab must hold exactly one element, not {}", count);
        return false;
    }
    xml_.reset();
    pugi::xml_node rml = xml_.append_child("rml");
    rml.append_child("head");
    pugi::xml_node body = rml.append_child("body");
    for (pugi::xml_node node : parsed.children()) // the element and the comments around it
        if (node.type() != pugi::node_declaration)
            body.append_copy(node);
    prefab_ = true;
    expandInstances();
    return true;
}

std::string GuiDoc::serialize() const
{
    pugi::xml_document copy;
    copy.reset(xml_);
    collapseUnder(copy.child("rml").child("body"));
    std::ostringstream out;
    if (!prefab_)
        copy.save(out, kIndent, pugi::format_default | pugi::format_no_declaration);
    else
        for (pugi::xml_node node : copy.child("rml").child("body").children())
            node.print(out, kIndent, pugi::format_default | pugi::format_no_declaration);
    return out.str();
}

void GuiDoc::setPrefabContext(const std::filesystem::path& root, std::string documentPath)
{
    prefabRoot_ = root;
    documentPath_ = std::move(documentPath);
    prefabCache_.clear();
}

kast::GuiPrefabExpansion GuiDoc::expansion(bool quiet) const
{
    kast::GuiPrefabExpansion x;
    x.document = documentPath_;
    x.read = [this](const std::string& path) {
        auto it = prefabCache_.find(path);
        if (it == prefabCache_.end()) {
            std::string ignored;
            const std::optional<std::filesystem::path> file = kast::resolveProjectPath(prefabRoot_, path, ignored);
            std::error_code ec;
            std::optional<std::string> text;
            if (file && std::filesystem::is_regular_file(*file, ec))
                text = readText(*file);
            it = prefabCache_.emplace(path, std::move(text)).first;
        }
        return it->second;
    };
    x.parseOptions = kParseOptions;
    x.quiet = quiet;
    x.keepPlaceholders = true;
    return x;
}

void GuiDoc::expandInstances()
{
    prefabFiles_.clear();
    prefabSheets_.clear();
    if (prefabRoot_.empty())
        return;
    kast::GuiPrefabExpansion x = expansion(false);
    kast::guiExpandPrefabs(body(), x);
    prefabFiles_ = std::move(x.files);
    prefabSheets_ = std::move(x.sheets);
}

void GuiDoc::refreshPrefabs()
{
    const std::string text = serialize();
    prefabCache_.clear();
    std::string error;
    if (prefab_)
        parsePrefab(text, error);
    else
        parse(text, error);
}

void GuiDoc::collapseUnder(pugi::xml_node node) const
{
    for (pugi::xml_node child : elements(node)) {
        if (missingPrefab(child))
            child.remove_attribute(kast::kGuiMissingPrefabAttribute); // saved as it was read
        else if (child.attribute(kast::kGuiPrefabAttribute))
            collapse(child);
        else
            collapseUnder(child);
    }
}

void GuiDoc::collapse(pugi::xml_node root) const
{
    const std::string path = root.attribute(kast::kGuiPrefabAttribute).value();
    const std::string id = root.attribute("id").value();
    pugi::xml_node reference = root.parent().insert_child_before("div", root);
    reference.append_attribute("id").set_value(id.c_str());
    reference.append_attribute(kast::kGuiPrefabAttribute).set_value(path.c_str());
    pugi::xml_document holder;
    const pugi::xml_node fresh = pristine(root, holder);
    const std::vector<pugi::xml_node> current = subtree(root);
    const std::vector<pugi::xml_node> expected = fresh ? subtree(fresh) : std::vector<pugi::xml_node>();
    if (current.size() != expected.size())
        kast::log::warn(std::format("GUI '{}': instance #{} no longer matches prefab '{}': saved without its overrides",
                                    documentPath_, id, path));
    else
        for (const Change& change : changes(current, expected)) {
            const pugi::xml_node was = expected[change.index];
            const pugi::xml_node now = current[change.index];
            // An id inside the prefab names the element (if it is the only one with it), else its child indices.
            std::string target;
            const std::string inner = attribute(was, "id");
            if (change.index > 0 && inner.starts_with(id + "_") &&
                std::ranges::count_if(expected, [&](pugi::xml_node e) { return attribute(e, "id") == inner; }) == 1)
                target = inner.substr(id.size() + 1);
            else
                target = indexPath(was, fresh);
            pugi::xml_node o = reference.append_child(kast::kGuiOverrideTag);
            o.append_attribute("target").set_value(target.c_str());
            if (change.attribute.empty()) {
                o.append_attribute("text").set_value(text(now).c_str());
            } else {
                o.append_attribute("attribute").set_value(change.attribute.c_str());
                if (const pugi::xml_attribute value = now.attribute(change.attribute.c_str()))
                    o.append_attribute("value").set_value(value.value());
            }
        }
    root.parent().remove_child(root);
}

pugi::xml_node GuiDoc::instanceRoot(pugi::xml_node node)
{
    pugi::xml_node root;
    for (pugi::xml_node at = node; at && at.type() == pugi::node_element; at = at.parent())
        if (at.attribute(kast::kGuiPrefabAttribute) && !missingPrefab(at))
            root = at;
    return root;
}

bool GuiDoc::prefabLocked(pugi::xml_node node)
{
    const pugi::xml_node root = instanceRoot(node);
    return root && root != node;
}

bool GuiDoc::missingPrefab(pugi::xml_node node) { return node.attribute(kast::kGuiMissingPrefabAttribute); }

bool GuiDoc::canAddInto(pugi::xml_node node) const
{
    return isInside(node, body()) && !instanceRoot(node) && !missingPrefab(node) &&
           !(prefab_ && node == body() && !elements(body()).empty());
}

pugi::xml_node GuiDoc::pristine(pugi::xml_node node, pugi::xml_document& holder) const
{
    const pugi::xml_node root = instanceRoot(node);
    if (!root)
        return {};
    holder.reset();
    pugi::xml_node reference = holder.append_child("div");
    reference.append_attribute("id").set_value(root.attribute("id").value());
    reference.append_attribute(kast::kGuiPrefabAttribute).set_value(root.attribute(kast::kGuiPrefabAttribute).value());
    kast::GuiPrefabExpansion x = expansion(true);
    const pugi::xml_node fresh = kast::guiExpandPrefab(reference, x);
    if (fresh == reference)
        return {};
    const std::vector<pugi::xml_node> current = subtree(root);
    const std::vector<pugi::xml_node> expected = subtree(fresh);
    const size_t index = static_cast<size_t>(std::ranges::find(current, node) - current.begin());
    return current.size() == expected.size() ? expected[index] : pugi::xml_node();
}

std::optional<std::string> GuiDoc::field(pugi::xml_node node, std::string_view name)
{
    if (name == "text")
        return text(node);
    if (const std::vector<std::string> properties = widgetProperties(node); std::ranges::find(properties, name) != properties.end())
        return widgetProperty(node, name);
    const pugi::xml_attribute value = widgetElement(node).attribute(std::string(name).c_str());
    return value ? std::optional<std::string>(value.value()) : std::nullopt;
}

void GuiDoc::setField(pugi::xml_node node, std::string_view name, const std::optional<std::string>& value)
{
    if (name == "text") {
        replaceText(node, value.value_or(""));
    } else if (const std::vector<std::string> properties = widgetProperties(node);
               std::ranges::find(properties, name) != properties.end()) {
        setWidgetProperty(node, name, value.value_or(""));
    } else {
        pugi::xml_node element = widgetElement(node);
        const std::string attributeName(name);
        if (!value) {
            element.remove_attribute(attributeName.c_str());
            return;
        }
        pugi::xml_attribute attribute = element.attribute(attributeName.c_str());
        if (!attribute)
            attribute = element.append_attribute(attributeName.c_str());
        attribute.set_value(value->c_str());
    }
}

bool GuiDoc::overridden(pugi::xml_node node, std::string_view name) const
{
    if (name == "id" && instanceRoot(node) == node)
        return false;
    pugi::xml_document holder;
    const pugi::xml_node was = pristine(node, holder);
    return was && field(node, name) != field(was, name);
}

bool GuiDoc::setInstanceId(pugi::xml_node root, std::string_view id)
{
    if (id.empty())
        return false;
    const std::string prefix = attribute(root, "id") + "_";
    std::vector<pugi::xml_node> inside;
    preorder(root, inside);
    for (pugi::xml_node node : inside)
        if (const std::string inner = attribute(node, "id"); inner.starts_with(prefix))
            node.attribute("id").set_value(std::format("{}_{}", id, inner.substr(prefix.size())).c_str());
    setAttribute(root, "id", id);
    return true;
}

pugi::xml_node GuiDoc::placePrefab(const std::string& path, pugi::xml_node parent, std::string& error)
{
    if (prefabRoot_.empty() || !canAddInto(parent)) {
        error = prefabRoot_.empty() ? "GUI prefabs need a project"
                : prefab_ && parent == body() ? "a GUI prefab holds exactly one element: place it inside that"
                                              : "nothing can be added into a prefab instance";
        return {};
    }
    // The prefab's file stem, made unique: no element has it, nor an id it would prefix.
    std::vector<pugi::xml_node> all;
    preorder(body(), all);
    const auto taken = [&](const std::string& id) {
        return std::ranges::any_of(all, [&](pugi::xml_node node) {
            const std::string other = attribute(node, "id");
            return other == id || other.starts_with(id + "_");
        });
    };
    const std::string stem = utf8(fromUtf8(path).stem());
    std::string id = stem;
    for (int n = 2; taken(id); ++n)
        id = std::format("{}_{}", stem, n);
    pugi::xml_node reference = parent.append_child("div");
    reference.append_attribute("id").set_value(id.c_str());
    reference.append_attribute(kast::kGuiPrefabAttribute).set_value(path.c_str());
    kast::GuiPrefabExpansion x = expansion(true);
    const pugi::xml_node instance = kast::guiExpandPrefab(reference, x);
    const std::string cycle = instance == reference ? std::string() : cycleUnder(instance);
    if (instance == reference || !cycle.empty()) {
        parent.remove_child(instance);
        error = std::format("prefab '{}' can't be placed here: {}", path,
                            cycle.empty() ? x.error : std::format("it would hold itself, {}", cycle));
        return {};
    }
    addUnique(prefabFiles_, x.files);
    addUnique(prefabSheets_, x.sheets);
    return instance;
}

pugi::xml_node GuiDoc::createPrefab(pugi::xml_node node, const std::filesystem::path& dir, std::filesystem::path& file,
                                    std::string& error)
{
    const std::string ownId = attribute(node, "id");
    const std::optional<std::string> folder = prefabRoot_.empty() ? std::nullopt : kast::toProjectPath(prefabRoot_, dir);
    if (!folder || node == body() || !isInside(node, body()) || instanceRoot(node) || missingPrefab(node)) {
        error = !folder ? "GUI prefabs are made inside the project"
                : instanceRoot(node) || missingPrefab(node) ? "it is (part of) a prefab instance: unpack it first"
                                                            : "select an element under Body";
        return {};
    }
    // <id, else widget id, else kind>.guiprefab: characters a file name can't hold become '_'.
    std::string stem = ownId.empty() ? attribute(widgetElement(node), "id") : ownId;
    if (stem.empty()) {
        stem = guiKindLabel(guiKind(node));
        std::ranges::transform(stem, stem.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    }
    for (char& c : stem)
        if (c == ' ' || static_cast<unsigned char>(c) < 0x20 || std::string_view(R"(<>:"/\|?*)").find(c) != std::string_view::npos)
            c = '_';
    if (!assetNameError(stem + kast::kGuiPrefabExtension).empty())
        stem = "prefab";
    std::string name = stem;
    for (int n = 2; assetNameTaken(dir, fromUtf8(name + kast::kGuiPrefabExtension)) || assetNameTaken(dir, fromUtf8(name + ".rcss")); ++n)
        name = std::format("{}_{}", stem, n);
    file = dir / fromUtf8(name + kast::kGuiPrefabExtension); // ponytail: free when checked, not locked until written
    const std::string path = folder->empty() ? utf8(file.filename()) : std::format("{}/{}", *folder, utf8(file.filename()));

    // The prefab: node as saved (instances inside it as references), without its id; ids that start with "<its id>_"
    // without that, so expanding it as an instance named after node gives them back unchanged.
    pugi::xml_document content;
    pugi::xml_node root = content.append_copy(node);
    collapseUnder(root);
    root.remove_attribute("id");
    for (pugi::xml_node element : subtree(root)) {
        for (pugi::xml_attribute a = element.first_attribute(); a;) {
            const pugi::xml_attribute next = a.next_attribute();
            if (transient(a.name()))
                element.remove_attribute(a);
            a = next;
        }
        if (const std::string inner = attribute(element, "id"); !ownId.empty() && inner.starts_with(ownId + "_"))
            element.attribute("id").set_value(inner.substr(ownId.size() + 1).c_str());
    }
    pugi::xml_document check;
    check.append_copy(root);
    kast::GuiPrefabExpansion x = expansion(true);
    x.document = path; // a .guiprefab: holding itself is a cycle
    kast::guiExpandPrefabs(check, x);
    if (const std::string cycle = cycleUnder(check.first_child()); !cycle.empty()) {
        error = std::format("GUI prefab '{}' would hold itself: {}", path, cycle);
        return {};
    }
    std::ostringstream text;
    content.save(text, kIndent, pugi::format_default | pugi::format_no_declaration);
    if (!writeTextFile(guiPrefabSheet(file), std::format("/* Styles of GUI prefab {}: linked into every GUI holding an instance of it. */\n", path),
                       error) ||
        !writeTextFile(file, text.str(), error))
        return {};

    std::string instanceId = ownId;
    if (instanceId.empty()) { // the file stem, unique in the document
        std::vector<pugi::xml_node> all;
        preorder(body(), all);
        instanceId = name;
        for (int n = 2; std::ranges::any_of(all, [&](pugi::xml_node e) {
                 const std::string other = attribute(e, "id");
                 return other == instanceId || other.starts_with(instanceId + "_");
             });
             ++n)
            instanceId = std::format("{}_{}", name, n);
    }
    pugi::xml_node reference = node.parent().insert_child_before("div", node);
    node.parent().remove_child(node);
    reference.append_attribute("id").set_value(instanceId.c_str());
    reference.append_attribute(kast::kGuiPrefabAttribute).set_value(path.c_str());
    prefabCache_.erase(path); // a placeholder may have found it missing
    prefabCache_.erase(kast::guiPrefabStylesheet(path));
    kast::GuiPrefabExpansion place = expansion(false);
    const pugi::xml_node instance = kast::guiExpandPrefab(reference, place);
    addUnique(prefabFiles_, place.files);
    addUnique(prefabSheets_, place.sheets);
    return instance;
}

bool GuiDoc::revert(pugi::xml_node node, std::string_view name)
{
    if (!overridden(node, name))
        return false;
    pugi::xml_document holder;
    setField(node, name, field(pristine(node, holder), name));
    return true;
}

pugi::xml_node GuiDoc::revertAll(pugi::xml_node root)
{
    pugi::xml_document holder;
    const pugi::xml_node fresh = instanceRoot(root) == root ? pristine(root, holder) : pugi::xml_node();
    if (!fresh)
        return {};
    const pugi::xml_node reverted = root.parent().insert_copy_before(fresh, root);
    root.parent().remove_child(root);
    return reverted;
}

bool GuiDoc::unpack(pugi::xml_node root)
{
    return instanceRoot(root) == root && root.remove_attribute(kast::kGuiPrefabAttribute);
}

bool GuiDoc::apply(pugi::xml_node node, std::string_view name, std::string& error)
{
    const pugi::xml_node root = instanceRoot(node);
    if (!root || (name == "id" && node == root)) {
        error = !root ? "not part of a prefab instance" : "the instance root's id is the instance's own";
        return false;
    }
    const std::string path = attribute(root, kast::kGuiPrefabAttribute);
    const std::string id = attribute(root, "id");
    // The prefab as its own document, expanded the same way (from the same reads): element i of it is element i of the
    // instance.
    GuiDoc prefab;
    prefab.setPrefabContext(prefabRoot_, path);
    prefab.prefabCache_ = prefabCache_;
    const std::optional<std::string> source = expansion(true).read(path);
    if (!source || !prefab.parsePrefab(*source, error)) {
        error = std::format("cannot read prefab '{}'{}", path, source ? ": " + error : "");
        return false;
    }
    const std::vector<pugi::xml_node> current = subtree(root);
    const std::vector<pugi::xml_node> target = subtree(elements(prefab.body())[0]);
    pugi::xml_document holder;
    const pugi::xml_node fresh = pristine(root, holder);
    const std::vector<pugi::xml_node> expected = fresh ? subtree(fresh) : std::vector<pugi::xml_node>();
    if (current.size() != target.size() || current.size() != expected.size()) {
        error = std::format("prefab '{}' no longer matches this instance: reload the GUI first", path);
        return false;
    }
    const auto unprefixed = [&](std::optional<std::string> value) { // ids are stored as they are inside the prefab
        if (value && value->starts_with(id + "_"))
            value = value->substr(id.size() + 1);
        return value;
    };
    if (name.empty()) {
        for (const Change& change : changes(current, expected)) {
            pugi::xml_node to = target[change.index];
            const pugi::xml_node from = current[change.index];
            if (change.attribute.empty()) {
                replaceText(to, text(from));
                continue;
            }
            std::optional<std::string> value;
            if (const pugi::xml_attribute a = from.attribute(change.attribute.c_str()))
                value = a.value();
            if (change.attribute == "id")
                value = unprefixed(value);
            if (!value)
                to.remove_attribute(change.attribute.c_str());
            else if (pugi::xml_attribute a = to.attribute(change.attribute.c_str()))
                a.set_value(value->c_str());
            else
                to.append_attribute(change.attribute.c_str()).set_value(value->c_str());
        }
    } else {
        const size_t index = static_cast<size_t>(std::ranges::find(current, node) - current.begin());
        setField(target[index], name, name == "id" ? unprefixed(field(node, name)) : field(node, name));
    }
    if (!writeTextFile(prefabRoot_ / fromUtf8(path), prefab.serialize(), error))
        return false;
    kast::log::info(std::format("Applied {} to GUI prefab '{}'", name.empty() ? "all overrides" : std::format("'{}'", name), path));
    refreshPrefabs();
    return true;
}

bool GuiDoc::remapPrefabPaths(const std::string& from, const std::string& to)
{
    bool any = false;
    for (pugi::xml_node node : subtree(body()))
        if (pugi::xml_attribute reference = node.attribute(kast::kGuiPrefabAttribute)) {
            std::vector<std::string> path{reference.value()};
            if (remapProjectPaths(path, from, to)) {
                reference.set_value(path[0].c_str());
                any = true;
            }
        }
    if (any)
        prefabCache_.clear();
    return any;
}

std::vector<pugi::xml_node> GuiDoc::elements(pugi::xml_node parent)
{
    std::vector<pugi::xml_node> out;
    for (pugi::xml_node child : parent.children())
        if (child.type() == pugi::node_element)
            out.push_back(child);
    return out;
}

pugi::xml_node GuiDoc::add(GuiKind kind, pugi::xml_node parent)
{
    if (kind == GuiKind::Unknown || !canAddInto(parent))
        return {};
    const std::string_view markup = kindMarkup(kind);
    if (!parent.append_buffer(markup.data(), markup.size(), kParseOptions))
        return {};
    return parent.last_child();
}

bool GuiDoc::remove(pugi::xml_node node)
{
    return node != body() && isInside(node, body()) && !prefabLocked(node) && !(prefab_ && node.parent() == body()) &&
           node.parent().remove_child(node);
}

bool GuiDoc::move(pugi::xml_node node, int delta)
{
    if (node == body() || !isInside(node, body()) || prefabLocked(node))
        return false;
    pugi::xml_node sibling = node;
    do
        sibling = delta < 0 ? sibling.previous_sibling() : sibling.next_sibling();
    while (sibling && sibling.type() != pugi::node_element);
    if (!sibling)
        return false;
    pugi::xml_node parent = node.parent();
    return delta < 0 ? static_cast<bool>(parent.insert_move_before(node, sibling))
                     : static_cast<bool>(parent.insert_move_after(node, sibling));
}

bool GuiDoc::reparent(pugi::xml_node node, pugi::xml_node parent)
{
    if (node == body() || !isInside(node, body()) || !canAddInto(parent) || isInside(parent, node) || prefabLocked(node))
        return false;
    return static_cast<bool>(parent.append_move(node));
}

std::string GuiDoc::attribute(pugi::xml_node node, const char* name) { return node.attribute(name).value(); }

void GuiDoc::setAttribute(pugi::xml_node node, const char* name, std::string_view value)
{
    if (value.empty()) {
        node.remove_attribute(name);
        return;
    }
    pugi::xml_attribute attribute = node.attribute(name);
    if (!attribute)
        attribute = node.append_attribute(name);
    attribute.set_value(std::string(value).c_str());
}

bool GuiDoc::hasEditableText(pugi::xml_node node)
{
    const GuiKind kind = guiKind(node);
    if (kind != GuiKind::Text && kind != GuiKind::RichText && kind != GuiKind::Button)
        return false;
    for (pugi::xml_node child : node.children())
        if (child.type() != pugi::node_pcdata)
            return false;
    return true;
}

std::string GuiDoc::text(pugi::xml_node node)
{
    std::string out;
    for (pugi::xml_node child : node.children())
        if (child.type() == pugi::node_pcdata)
            out += child.value();
    return out;
}

void GuiDoc::setText(pugi::xml_node node, std::string_view text)
{
    while (node.first_child())
        node.remove_child(node.first_child());
    if (!text.empty())
        node.append_child(pugi::node_pcdata).set_value(std::string(text).c_str());
}

bool GuiDoc::composite(pugi::xml_node node)
{
    const GuiKind kind = guiKind(node);
    return kind == GuiKind::Spinbox || kind == GuiKind::Dropdown || kind == GuiKind::ListBox || kind == GuiKind::VirtualList ||
           labelledToggle(node);
}

pugi::xml_node GuiDoc::widgetRoot(pugi::xml_node node)
{
    pugi::xml_node root = node;
    for (pugi::xml_node at = node.parent(); at; at = at.parent())
        if (composite(at))
            root = at;
    return root;
}

pugi::xml_node GuiDoc::widgetElement(pugi::xml_node node) { return labelledToggle(node) ? elements(node)[0] : node; }

std::vector<std::string> GuiDoc::widgetProperties(pugi::xml_node node)
{
    switch (guiKind(node)) {
    case GuiKind::Slider: return {"min", "max", "step", "value"};
    case GuiKind::Spinbox: return {"min", "max", "step", "value", "data-value"};
    case GuiKind::Progress: return {"value", "max"};
    case GuiKind::Toggle: return labelledToggle(node) ? std::vector<std::string>{"checked", "label"} : std::vector<std::string>{"checked"};
    case GuiKind::ListBox: return {"follow"};
    case GuiKind::VirtualList: { // a template holding other elements is edited in the .rml
        std::vector<std::string> names{"model", "source", "key", "row-height", "follow"};
        if (std::ranges::all_of(rowTemplate(node).children(), [](pugi::xml_node c) { return c.type() == pugi::node_pcdata; }))
            names.push_back("template");
        return names;
    }
    default: return {};
    }
}

std::string GuiDoc::widgetProperty(pugi::xml_node node, std::string_view name)
{
    const auto [element, attribute] = propertyPlace(node, name);
    if (!element)
        return {};
    if (isFlag(name))
        return element.attribute(attribute) ? "true" : "false";
    if (name == "source")
        return dataFor(element.attribute("data-for").value()).second;
    return *attribute ? GuiDoc::attribute(element, attribute) : text(element);
}

bool GuiDoc::widgetAttribute(pugi::xml_node node, std::string_view attribute)
{
    const GuiKind kind = guiKind(node);
    if (attribute == "type" && (kind == GuiKind::Slider || kind == GuiKind::Toggle))
        return true;
    return std::ranges::any_of(widgetProperties(node), [&](const std::string& name) {
        const auto [element, stored] = propertyPlace(node, name);
        return element == widgetElement(node) && attribute == stored;
    });
}

bool GuiDoc::setWidgetProperty(pugi::xml_node node, std::string_view name, std::string_view value)
{
    auto [element, attribute] = propertyPlace(node, name);
    if (!element)
        return false;
    if (isFlag(name)) {
        element.remove_attribute(attribute);
        if (value == "true")
            element.append_attribute(attribute); // checked="", data-follow=""
    } else if (name == "source") { // the alias stays: "item : <value>"
        const std::string alias = dataFor(element.attribute("data-for").value()).first;
        setAttribute(element, "data-for", alias.empty() || value.empty() ? std::string(value) : std::format("{} : {}", alias, value));
    } else if (name == "template") {
        setText(element, value);
    } else if (*attribute) {
        setAttribute(element, attribute, value);
    } else { // a Toggle's label: the <label>'s text, after its <input>
        for (pugi::xml_node child = element.first_child(); child;) {
            const pugi::xml_node next = child.next_sibling();
            if (child.type() == pugi::node_pcdata)
                element.remove_child(child);
            child = next;
        }
        if (!value.empty())
            element.append_child(pugi::node_pcdata).set_value(std::string(value).c_str());
    }
    return true;
}

std::vector<pugi::xml_node> GuiDoc::options(pugi::xml_node select)
{
    std::vector<pugi::xml_node> out;
    for (pugi::xml_node child : elements(select))
        if (std::string_view(child.name()) == "option")
            out.push_back(child);
    return out;
}

pugi::xml_node GuiDoc::addOption(pugi::xml_node select)
{
    const size_t n = options(select).size() + 1;
    pugi::xml_node option = select.append_child("option");
    option.append_attribute("value").set_value(std::format("option{}", n).c_str());
    setText(option, std::format("Option {}", n));
    return option;
}

void GuiDoc::selectOption(pugi::xml_node select, pugi::xml_node option)
{
    for (pugi::xml_node other : options(select))
        other.remove_attribute("selected");
    option.append_attribute("selected"); // selected=""
}

std::vector<pugi::xml_node> GuiDoc::rows(pugi::xml_node listbox)
{
    std::vector<pugi::xml_node> out;
    for (pugi::xml_node child : elements(listbox))
        if (hasClass(child, "row"))
            out.push_back(child);
    return out;
}

pugi::xml_node GuiDoc::addRow(pugi::xml_node listbox)
{
    const size_t n = rows(listbox).size() + 1;
    pugi::xml_node row = listbox.append_child("div");
    row.append_attribute("class").set_value("row");
    row.append_attribute("data-value").set_value(std::format("row{}", n).c_str());
    setText(row, std::format("Row {}", n));
    return row;
}

void GuiDoc::selectRow(pugi::xml_node listbox, pugi::xml_node row)
{
    for (pugi::xml_node other : rows(listbox))
        if (other != row && rowSelected(other))
            setClassToken(other, "selected", false);
    if (row && !rowSelected(row))
        setClassToken(row, "selected", true);
}

bool GuiDoc::rowSelected(pugi::xml_node row) { return hasClass(row, "selected"); }

std::string GuiDoc::stylesheetHref() const
{
    for (pugi::xml_node node : xml_.child("rml").child("head").children())
        if (isStylesheetLink(node))
            return node.attribute("href").value();
    return {};
}

void GuiDoc::setStylesheetHref(std::string_view href)
{
    for (pugi::xml_node node : xml_.child("rml").child("head").children())
        if (isStylesheetLink(node)) {
            setAttribute(node, "href", href);
            return;
        }
}

std::string GuiDoc::previewRml(std::string_view rcss, std::vector<pugi::xml_node>& nodes) const
{
    nodes.clear();
    preorder(body(), nodes);
    pugi::xml_document copy;
    copy.reset(xml_);
    std::vector<pugi::xml_node> copied;
    preorder(copy.child("rml").child("body"), copied); // same shape, same order
    for (size_t i = 0; i < copied.size(); ++i)
        copied[i].append_attribute(kNodeAttribute).set_value(static_cast<unsigned long long>(i));
    // A Virtual List's template has no data here: placeholder rows instead, at its row height, the first one the
    // template itself (keeping its node index, so a click on it selects the list).
    for (pugi::xml_node list : copied) {
        if (guiKind(list) != GuiKind::VirtualList)
            continue;
        pugi::xml_node row = rowTemplate(list);
        forEachElement(row, [](pugi::xml_node element) {
            for (pugi::xml_attribute attribute = element.first_attribute(); attribute;) {
                const pugi::xml_attribute next = attribute.next_attribute();
                if (guiBindingAttribute(attribute.name()))
                    element.remove_attribute(attribute);
                attribute = next;
            }
        });
        const std::string text = GuiDoc::attribute(list, "data-row-height");
        float height = 0.0f;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), height);
        if (error != std::errc() || end != text.data() + text.size() || !(height > 0.0f && height < 1e6f))
            height = 24.0f; // as the engine reads it (gui_lists.h)
        const std::string style = GuiDoc::attribute(row, "style");
        GuiDoc::setAttribute(row, "style", std::format("{}{}height: {}px;", style, style.empty() || style.ends_with(';') ? "" : ";", height));
        for (int n = kGuiPreviewRows; n >= 2; --n) {
            pugi::xml_node placeholder = list.insert_copy_after(row, row);
            forEachElement(placeholder, [](pugi::xml_node element) { element.remove_attribute(kNodeAttribute); });
            fillPlaceholders(placeholder, n);
        }
        fillPlaceholders(row, 1);
    }
    // A missing prefab's overrides are kept for saving, not shown (last: copied still points at them until here).
    std::erase_if(copied, [](pugi::xml_node node) { return !missingPrefab(node) || missingPrefab(node.parent()); });
    for (pugi::xml_node placeholder : copied)
        while (placeholder.first_child())
            placeholder.remove_child(placeholder.first_child());
    pugi::xml_node head = copy.child("rml").child("head");
    if (!head) // the sheet goes in a <head>, even in a document written without one
        head = copy.child("rml").prepend_child("head");
    for (pugi::xml_node node = head.first_child(); node;) {
        const pugi::xml_node next = node.next_sibling();
        if (isStylesheetLink(node))
            head.remove_child(node);
        node = next;
    }
    // RmlUi reads <style> raw, but pugixml would escape the sheet's '>' selectors: splice it in after saving.
    constexpr std::string_view kSheetMarker = "KAST_PREVIEW_RCSS";
    head.append_child("style").append_child(pugi::node_pcdata).set_value(kSheetMarker.data());
    std::ostringstream out;
    copy.save(out, "", pugi::format_raw | pugi::format_no_declaration);
    std::string rml = out.str();
    rml.replace(rml.find(kSheetMarker), kSheetMarker.size(), rcss);
    return rml;
}

std::string guiWrapRichText(std::string_view markup, size_t start, size_t end, std::string_view tag, std::string_view value)
{
    if (start > end)
        std::swap(start, end);
    end = std::min(end, markup.size());
    start = std::min(start, end);
    const std::string open = value.empty() ? std::format("[{}]", tag) : std::format("[{}={}]", tag, value);
    if (tag == "img")
        return std::format("{}{}{}", markup.substr(0, start), open, markup.substr(start));
    return std::format("{}{}{}[/{}]{}", markup.substr(0, start), open, markup.substr(start, end - start), tag, markup.substr(end));
}

bool writeNewGui(const std::filesystem::path& dir, std::filesystem::path& rml, std::string& error)
{
    std::string stem = "new_gui";
    for (int n = 2; assetNameTaken(dir, fromUtf8(stem + ".rml")) || assetNameTaken(dir, fromUtf8(stem + ".rcss")); ++n)
        stem = std::format("new_gui_{}", n);
    rml = dir / fromUtf8(stem + ".rml"); // ponytail: free when checked, not locked until written
    if (!writeTextFile(dir / fromUtf8(stem + ".rcss"), kGuiRcssTemplate, error))
        return false;
    return writeTextFile(rml, withName(kGuiRmlTemplate, stem), error);
}

std::filesystem::path linkedStylesheet(const std::filesystem::path& rml)
{
    GuiDoc doc;
    std::string error;
    if (!doc.parse(readText(rml), error) || doc.stylesheetHref().empty())
        return {};
    const std::filesystem::path rcss = (rml.parent_path() / fromUtf8(doc.stylesheetHref())).lexically_normal();
    // Save writes the sheet: never outside the project (or, for a GUI in no project, outside the .rml's folder).
    const std::filesystem::path root = kast::findProjectDir(rml.parent_path()).value_or(rml.parent_path());
    return kast::toProjectPath(root, rcss) ? rcss : std::filesystem::path();
}

bool renameGui(const std::filesystem::path& rml, const std::string& newName, std::string& error)
{
    const std::filesystem::path target = fromUtf8(newName);
    const std::filesystem::path oldRcss = rml.parent_path() / fromUtf8(utf8(rml.stem()) + ".rcss");
    GuiDoc doc;
    std::string ignored;
    const bool linked = target.extension() == ".rml" && target.stem() != rml.stem() &&
                        doc.parse(readText(rml), ignored) && doc.stylesheetHref() == utf8(oldRcss.filename()) &&
                        std::filesystem::is_regular_file(oldRcss);
    if (!linked)
        return renameAsset(rml, newName, error);
    const std::string newRcss = utf8(target.stem()) + ".rcss";
    if (!renameAsset(oldRcss, newRcss, error))
        return false;
    const std::filesystem::path renamed = rml.parent_path() / target;
    doc.setStylesheetHref(newRcss);
    const bool moved = renameAsset(rml, newName, error);
    if (moved && writeTextFile(renamed, doc.serialize(), error))
        return true;
    std::string undo; // put both names back; the .rml text is only written last
    if (moved)
        renameAsset(renamed, utf8(rml.filename()), undo);
    renameAsset(rml.parent_path() / fromUtf8(newRcss), utf8(oldRcss.filename()), undo);
    return false;
}

std::filesystem::path guiPrefabSheet(const std::filesystem::path& prefab)
{
    return prefab.parent_path() / fromUtf8(utf8(prefab.stem()) + ".rcss");
}

bool renameGuiPrefab(const std::filesystem::path& prefab, const std::string& newName, std::string& error)
{
    const std::filesystem::path target = fromUtf8(newName);
    const std::filesystem::path oldSheet = guiPrefabSheet(prefab);
    std::error_code ec;
    if (target.extension() != kast::kGuiPrefabExtension || target.stem() == prefab.stem() ||
        !std::filesystem::is_regular_file(oldSheet, ec))
        return renameAsset(prefab, newName, error);
    const std::string newSheet = utf8(target.stem()) + ".rcss";
    if (!renameAsset(oldSheet, newSheet, error))
        return false;
    if (renameAsset(prefab, newName, error))
        return true;
    std::string undo; // put the sheet's name back
    renameAsset(prefab.parent_path() / fromUtf8(newSheet), utf8(oldSheet.filename()), undo);
    return false;
}

bool moveGuiPrefab(const std::filesystem::path& prefab, const std::filesystem::path& destDir, std::string& error)
{
    const std::filesystem::path sheet = guiPrefabSheet(prefab);
    std::error_code ec;
    const bool withSheet = std::filesystem::is_regular_file(sheet, ec);
    if (withSheet && !moveAsset(sheet, destDir, error))
        return false;
    if (moveAsset(prefab, destDir, error))
        return true;
    std::string undo; // put the sheet back
    if (withSheet)
        moveAsset(destDir / sheet.filename(), sheet.parent_path(), undo);
    return false;
}

std::vector<std::string> remapGuiPrefabReferences(const std::filesystem::path& root, const std::string& from,
                                                  const std::string& to)
{
    static const std::regex reference(R"re((data-prefab\s*=\s*")([^"]*)("))re");
    std::vector<std::string> changed;
    for (const std::filesystem::path& file : kast::listFiles(root)) {
        if (file.extension() != ".rml" && file.extension() != kast::kGuiPrefabExtension)
            continue;
        const std::string text = readText(file);
        std::string out;
        bool any = false;
        auto last = text.cbegin();
        for (auto it = std::sregex_iterator(text.begin(), text.end(), reference); it != std::sregex_iterator(); ++it) {
            std::vector<std::string> path{(*it)[2].str()};
            any = remapProjectPaths(path, from, to) || any;
            out.append(last, (*it)[0].first).append((*it)[1].str() + path[0] + (*it)[3].str());
            last = (*it)[0].second;
        }
        out.append(last, text.cend());
        std::string error;
        if (!any)
            continue;
        if (writeTextFile(file, out, error))
            changed.push_back(kast::toProjectPath(root, file).value_or(utf8(file)));
        else
            kast::log::error(std::format("Cannot update the GUI prefab references in '{}': {}", utf8(file), error));
    }
    return changed;
}
