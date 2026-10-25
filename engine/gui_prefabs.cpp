#include "gui_prefabs.h"

#include "log.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <format>
#include <sstream>

namespace kast {
namespace {

constexpr unsigned kRawSaveFormat = pugi::format_raw | pugi::format_no_escapes | pugi::format_no_declaration;

std::vector<pugi::xml_node> elements(pugi::xml_node parent)
{
    std::vector<pugi::xml_node> out;
    for (pugi::xml_node child : parent.children())
        if (child.type() == pugi::node_element)
            out.push_back(child);
    return out;
}

void preorder(pugi::xml_node node, std::vector<pugi::xml_node>& out)
{
    for (pugi::xml_node child : elements(node)) {
        out.push_back(child);
        preorder(child, out);
    }
}

void setAttribute(pugi::xml_node node, const char* name, const std::string& value)
{
    pugi::xml_attribute attribute = node.attribute(name);
    if (!attribute)
        attribute = node.append_attribute(name);
    attribute.set_value(value.c_str());
}

// The element of the expanded instance (root, id instanceId) an override's target names: "" the root, an id inside the
// prefab (now "<instanceId>_<id>"), else child indices "i/j/..." from the root. Null if none.
pugi::xml_node findTarget(pugi::xml_node root, const std::string& instanceId, std::string_view target)
{
    if (target.empty())
        return root;
    std::vector<pugi::xml_node> inside;
    preorder(root, inside);
    const std::string id = std::format("{}_{}", instanceId, target);
    for (pugi::xml_node node : inside)
        if (node.attribute("id").value() == id)
            return node;
    pugi::xml_node at = root;
    for (size_t start = 0; at && start <= target.size();) {
        const size_t slash = std::min(target.find('/', start), target.size());
        size_t index = 0;
        const char* first = target.data() + start;
        const char* last = target.data() + slash;
        const auto [end, error] = std::from_chars(first, last, index);
        if (error != std::errc() || end != last || first == last)
            return {};
        const std::vector<pugi::xml_node> children = elements(at);
        at = index < children.size() ? children[index] : pugi::xml_node();
        start = slash + 1;
    }
    return at;
}

void setText(pugi::xml_node node, const std::string& text)
{
    for (pugi::xml_node child = node.first_child(); child;) {
        const pugi::xml_node next = child.next_sibling();
        if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata)
            node.remove_child(child);
        child = next;
    }
    if (!text.empty())
        node.append_child(pugi::node_pcdata).set_value(text.c_str());
}

struct Override {
    std::string target;
    std::optional<std::string> text;
    std::string attribute;
    std::optional<std::string> value; // nullopt: the attribute is removed
};

std::filesystem::path pathOf(std::string_view text)
{
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

std::string textOf(const std::filesystem::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

pugi::xml_node expand(pugi::xml_node reference, GuiPrefabExpansion& x, std::vector<std::string>& stack);

void expandUnder(pugi::xml_node node, GuiPrefabExpansion& x, std::vector<std::string>& stack)
{
    for (pugi::xml_node child : elements(node)) {
        if (child.attribute(kGuiPrefabAttribute))
            expand(child, x, stack);
        else
            expandUnder(child, x, stack);
    }
}

pugi::xml_node expand(pugi::xml_node reference, GuiPrefabExpansion& x, std::vector<std::string>& stack)
{
    const std::string path = reference.attribute(kGuiPrefabAttribute).value();
    const std::string id = reference.attribute("id").value();
    const auto fail = [&](std::string reason) {
        x.error = reason;
        if (!x.quiet)
            log::warn(std::format("GUI '{}': prefab '{}'{}: {}; left as an empty placeholder", x.document, path,
                                  id.empty() ? "" : std::format(" (#{})", id), reason));
        if (x.keepPlaceholders) {
            setAttribute(reference, kGuiMissingPrefabAttribute, reason);
        } else {
            while (reference.first_child())
                reference.remove_child(reference.first_child());
        }
        return reference;
    };
    if (id.empty())
        return fail("the reference has no id");
    if (std::ranges::find(stack, path) != stack.end()) {
        std::string chain;
        for (auto it = std::ranges::find(stack, path); it != stack.end(); ++it)
            chain += *it + " -> ";
        return fail(std::format("a cycle ({}{})", chain, path));
    }
    const std::optional<std::string> text = x.read ? x.read(path) : std::nullopt;
    if (!text)
        return fail("not found");
    pugi::xml_document prefab;
    const pugi::xml_parse_result parsed = prefab.load_buffer(text->data(), text->size(), x.parseOptions);
    if (!parsed)
        return fail(std::format("not well-formed: {} at offset {}", parsed.description(), parsed.offset));
    const std::vector<pugi::xml_node> roots = elements(prefab);
    if (roots.size() != 1)
        return fail(std::format("it must hold exactly one element, not {}", roots.size()));
    if (std::ranges::find(x.files, path) == x.files.end())
        x.files.push_back(path);

    std::vector<Override> overrides;
    for (pugi::xml_node child : elements(reference)) {
        if (std::string_view(child.name()) != kGuiOverrideTag)
            continue;
        Override o{child.attribute("target").value()};
        if (pugi::xml_attribute replaced = child.attribute("text"))
            o.text = replaced.value();
        o.attribute = child.attribute("attribute").value();
        if (pugi::xml_attribute value = child.attribute("value"))
            o.value = value.value();
        if (o.text || !o.attribute.empty())
            overrides.push_back(std::move(o));
    }

    pugi::xml_node node = reference.parent().insert_copy_before(roots[0], reference);
    stack.push_back(path);
    expandUnder(node, x, stack);
    stack.pop_back();
    std::vector<pugi::xml_node> inside;
    preorder(node, inside);
    for (pugi::xml_node element : inside)
        if (pugi::xml_attribute inner = element.attribute("id"); inner && *inner.value())
            inner.set_value(std::format("{}_{}", id, inner.value()).c_str());
    if (pugi::xml_attribute own = node.attribute("id"))
        own.set_value(id.c_str());
    else
        node.prepend_attribute("id").set_value(id.c_str());
    setAttribute(node, kGuiPrefabAttribute, path);

    // Targets are found before any override applies (an id override would hide the element from the next one).
    std::vector<std::pair<pugi::xml_node, const Override*>> targets;
    for (const Override& o : overrides) {
        if (const pugi::xml_node target = findTarget(node, id, o.target))
            targets.emplace_back(target, &o);
        else if (!x.quiet)
            log::warn(std::format("GUI '{}': prefab '{}' (#{}): override target '{}' is not in the prefab: ignored",
                                  x.document, path, id, o.target));
    }
    for (auto [target, o] : targets) {
        if (o->text)
            setText(target, *o->text);
        else if (o->attribute == "id" && target == node)
            continue; // the instance's id is the reference's
        else if (o->value)
            setAttribute(target, o->attribute.c_str(), *o->value);
        else
            target.remove_attribute(o->attribute.c_str());
    }

    const std::string sheet = guiPrefabStylesheet(path);
    if (std::ranges::find(x.sheets, sheet) == x.sheets.end() && x.read(sheet)) {
        x.sheets.push_back(sheet);
        x.files.push_back(sheet);
    }
    reference.parent().remove_child(reference);
    return node;
}

// A prefab being expanded as a document (the GUI Builder editing it) is on the stack from the start: an instance of
// itself inside it is a cycle.
std::vector<std::string> startStack(const GuiPrefabExpansion& x)
{
    return x.document.ends_with(kGuiPrefabExtension) ? std::vector<std::string>{x.document} : std::vector<std::string>{};
}

} // namespace

std::string guiPrefabStylesheet(std::string_view prefab)
{
    const size_t slash = prefab.rfind('/');
    const size_t dot = prefab.rfind('.');
    const size_t stem = dot != std::string_view::npos && (slash == std::string_view::npos || dot > slash) ? dot : prefab.size();
    return std::format("{}.rcss", prefab.substr(0, stem));
}

pugi::xml_node guiExpandPrefab(pugi::xml_node reference, GuiPrefabExpansion& expansion)
{
    std::vector<std::string> stack = startStack(expansion);
    return expand(reference, expansion, stack);
}

void guiExpandPrefabs(pugi::xml_node node, GuiPrefabExpansion& expansion)
{
    std::vector<std::string> stack = startStack(expansion);
    expandUnder(node, expansion, stack);
}

std::string guiExpandPrefabsRml(std::string_view rml, GuiPrefabExpansion& expansion)
{
    pugi::xml_document doc;
    if (rml.find(kGuiPrefabAttribute) == std::string_view::npos || !doc.load_buffer(rml.data(), rml.size(), kGuiRawParseOptions) ||
        !doc.child("rml"))
        return std::string(rml);
    pugi::xml_node root = doc.child("rml");
    guiExpandPrefabs(root, expansion);
    if (!expansion.sheets.empty()) {
        pugi::xml_node head = root.child("head");
        if (!head)
            head = root.prepend_child("head");
        const pugi::xml_node first = head.child("link");
        const std::filesystem::path dir = pathOf(expansion.document).parent_path();
        for (const std::string& sheet : expansion.sheets) {
            pugi::xml_node link = first ? head.insert_child_before("link", first) : head.append_child("link");
            link.append_attribute("type").set_value("text/rcss");
            link.append_attribute("href").set_value(textOf(pathOf(sheet).lexically_relative(dir)).c_str());
        }
    }
    std::ostringstream out;
    doc.save(out, "", kRawSaveFormat);
    return out.str();
}

std::optional<std::string> guiPrefabInstanceRml(const std::string& path, const std::string& id, GuiPrefabExpansion& expansion)
{
    pugi::xml_document doc;
    pugi::xml_node reference = doc.append_child("div");
    reference.append_attribute("id").set_value(id.c_str());
    reference.append_attribute(kGuiPrefabAttribute).set_value(path.c_str());
    expansion.quiet = true;
    const pugi::xml_node node = guiExpandPrefab(reference, expansion);
    if (node == reference)
        return std::nullopt;
    std::ostringstream out;
    node.print(out, "", kRawSaveFormat);
    return out.str();
}

std::vector<std::string> guiMarkupIds(std::string_view rml)
{
    pugi::xml_document doc;
    doc.load_buffer(rml.data(), rml.size(), kGuiRawParseOptions);
    std::vector<pugi::xml_node> all;
    preorder(doc, all);
    std::vector<std::string> ids;
    for (pugi::xml_node node : all)
        if (*node.attribute("id").value())
            ids.emplace_back(node.attribute("id").value());
    return ids;
}

} // namespace kast
