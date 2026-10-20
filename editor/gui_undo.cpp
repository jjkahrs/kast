#include "gui_undo.h"

#include <sstream>

namespace {

std::string print(const GuiElementCopy& element)
{
    std::ostringstream out;
    element.copy->save(out, "", pugi::format_raw | pugi::format_no_declaration);
    return out.str();
}

void remove(pugi::xml_node root, const GuiElementCopy& element)
{
    const pugi::xml_node node = guiNodeAt(root, element.path);
    node.parent().remove_child(node);
}

pugi::xml_node insert(pugi::xml_node root, const GuiElementCopy& element)
{
    const std::vector<size_t> parentPath(element.path.begin(), element.path.end() - 1);
    pugi::xml_node parent = guiNodeAt(root, parentPath);
    const pugi::xml_node next = guiNodeAt(parent, {element.path.back()});
    return next ? parent.insert_copy_before(element.copy->first_child(), next)
                : parent.append_copy(element.copy->first_child());
}

} // namespace

GuiElementCopy copyGuiElement(pugi::xml_node node)
{
    GuiElementCopy element;
    for (pugi::xml_node at = node; at.parent(); at = at.parent()) {
        size_t index = 0;
        for (pugi::xml_node sibling = at.previous_sibling(); sibling; sibling = sibling.previous_sibling())
            ++index;
        element.path.insert(element.path.begin(), index);
    }
    auto copy = std::make_shared<pugi::xml_document>();
    copy->append_copy(node);
    element.copy = std::move(copy);
    return element;
}

pugi::xml_node guiNodeAt(pugi::xml_node root, const std::vector<size_t>& path)
{
    pugi::xml_node node = root;
    for (size_t index : path) {
        node = node.first_child();
        for (size_t i = 0; i < index && node; ++i)
            node = node.next_sibling();
    }
    return node;
}

bool pushGuiStep(UndoStack& history, pugi::xml_node root, std::string name, std::optional<GuiElementCopy> before,
                 std::optional<GuiElementCopy> after, std::function<void(pugi::xml_node)> select)
{
    if (before && after && before->path == after->path && print(*before) == print(*after))
        return false;
    // Each way: the element as it is now goes, the other copy comes back.
    const auto apply = [root, select](const std::optional<GuiElementCopy>& from, const std::optional<GuiElementCopy>& to) {
        if (from)
            remove(root, *from);
        select(to ? insert(root, *to) : pugi::xml_node());
    };
    history.push({std::move(name), [apply, before, after] { apply(after, before); },
                  [apply, before, after] { apply(before, after); }});
    return true;
}
