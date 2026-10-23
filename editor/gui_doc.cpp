#include "gui_doc.h"

#include "assets.h"

#include <project.h>

#include <array>
#include <format>
#include <fstream>
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

} // namespace

const char* guiKindTag(GuiKind kind)
{
    switch (kind) {
    case GuiKind::Panel: return "div";
    case GuiKind::Text: return "p";
    case GuiKind::Button: return "button";
    default: return "";
    }
}

const char* guiKindLabel(GuiKind kind)
{
    switch (kind) {
    case GuiKind::Panel: return "Panel";
    case GuiKind::Text: return "Text";
    case GuiKind::Button: return "Button";
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
    for (GuiKind kind : kGuiCreatableKinds)
        if (std::string_view(node.name()) == guiKindTag(kind))
            return kind;
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
    pugi::xml_node node = parent.append_child(guiKindTag(kind));
    if (kind != GuiKind::Panel)
        setText(node, guiKindLabel(kind));
    return node;
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
