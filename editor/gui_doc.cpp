#include "gui_doc.h"

#include "assets.h"

#include <project.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
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
        return GuiKind::Text;
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
    return true;
}

std::string GuiDoc::serialize() const
{
    std::ostringstream out;
    xml_.save(out, kIndent, pugi::format_default | pugi::format_no_declaration);
    return out.str();
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
    if (kind == GuiKind::Unknown || !isInside(parent, body()))
        return {};
    const std::string_view markup = kindMarkup(kind);
    if (!parent.append_buffer(markup.data(), markup.size(), kParseOptions))
        return {};
    return parent.last_child();
}

bool GuiDoc::remove(pugi::xml_node node)
{
    return node != body() && isInside(node, body()) && node.parent().remove_child(node);
}

bool GuiDoc::move(pugi::xml_node node, int delta)
{
    if (node == body() || !isInside(node, body()))
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
    if (node == body() || !isInside(node, body()) || !isInside(parent, body()) || isInside(parent, node))
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
    if (kind != GuiKind::Text && kind != GuiKind::Button)
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
    pugi::xml_node head = copy.child("rml").child("head");
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
