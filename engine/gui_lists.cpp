#include "gui_lists.h"

#include "gui_models.h"
#include "log.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>

namespace kast {
namespace {

std::string trim(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return std::string(text);
}

float rowHeightOf(const Rml::Element& element)
{
    const Rml::String text = element.GetAttribute<Rml::String>("data-row-height", "");
    float height = 0.0f;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), height);
    return error == std::errc() && end == text.data() + text.size() && std::isfinite(height) && height > 0.0f ? height
                                                                                                             : kGuiListRowHeight;
}

std::optional<GuiListKey> listKey(int identifier)
{
    switch (static_cast<Rml::Input::KeyIdentifier>(identifier)) {
    case Rml::Input::KI_UP: return GuiListKey::Up;
    case Rml::Input::KI_DOWN: return GuiListKey::Down;
    case Rml::Input::KI_PRIOR: return GuiListKey::PageUp;
    case Rml::Input::KI_NEXT: return GuiListKey::PageDown;
    case Rml::Input::KI_HOME: return GuiListKey::Home;
    case Rml::Input::KI_END: return GuiListKey::End;
    default: return std::nullopt;
    }
}

Rml::Element* spacer(Rml::ElementDocument& document, Rml::Element& list, Rml::Element* before)
{
    Rml::ElementPtr made = document.CreateElement("div");
    made->SetProperty(Rml::PropertyId::Display, Rml::Property(Rml::Style::Display::Block));
    made->SetProperty(Rml::PropertyId::Height, Rml::Property(0.0f, Rml::Unit::PX));
    return before ? list.InsertBefore(std::move(made), before) : list.AppendChild(std::move(made));
}

} // namespace

GuiRowRange guiVisibleRows(float scrollTop, float viewHeight, float rowHeight, int rows, int buffer)
{
    if (rows <= 0 || !(rowHeight > 0.0f))
        return {};
    const double top = std::max(0.0, static_cast<double>(scrollTop)) / rowHeight;
    const double bottom = top + std::max(0.0, static_cast<double>(viewHeight)) / rowHeight;
    const auto row = [&](double at) { return static_cast<int>(std::min(at, static_cast<double>(rows))); };
    const int first = std::clamp(row(std::floor(top)) - buffer, 0, rows);
    const int last = std::clamp(row(std::ceil(bottom)) + buffer, first, rows);
    return {first, last - first};
}

float guiScrollIntoView(float scrollTop, float viewHeight, float rowTop, float rowHeight)
{
    if (rowTop < scrollTop)
        return rowTop;
    if (rowTop + rowHeight > scrollTop + viewHeight)
        return std::min(rowTop, rowTop + rowHeight - viewHeight); // a row taller than the view shows its top
    return scrollTop;
}

int guiListMove(GuiListKey key, int selected, int rows, int page)
{
    if (rows <= 0)
        return -1;
    const int last = rows - 1;
    page = std::max(page, 1);
    if (selected < 0 || selected > last)
        return key == GuiListKey::Up || key == GuiListKey::End ? last : 0;
    switch (key) {
    case GuiListKey::Up: return std::max(selected - 1, 0);
    case GuiListKey::Down: return std::min(selected + 1, last);
    case GuiListKey::PageUp: return std::max(selected - page, 0);
    case GuiListKey::PageDown: return std::min(selected + page, last);
    case GuiListKey::Home: return 0;
    case GuiListKey::End: return last;
    }
    return selected;
}

std::optional<std::string> guiRowKey(const GuiValue& item, std::string_view field)
{
    if (!item.is_object())
        return std::nullopt;
    const auto it = item.find(field);
    if (it == item.end())
        return std::nullopt;
    if (it->is_string())
        return it->get<std::string>();
    if (!it->is_number())
        return std::nullopt;
    const double number = it->get<double>();
    if (number == std::floor(number) && std::abs(number) < 9007199254740992.0) // 2^53: whole, as Lua prints it
        return std::format("{}", static_cast<long long>(number));
    return std::format("{}", number);
}

int guiTrackRow(const GuiValue& rows, std::string_view field, int index, const std::optional<std::string>& key)
{
    const int count = rows.is_array() ? static_cast<int>(rows.size()) : 0;
    if (!key)
        return index >= 0 && index < count ? index : -1;
    if (index >= 0 && index < count && guiRowKey(rows[static_cast<size_t>(index)], field) == key)
        return index; // the usual case: rows appended or changed elsewhere
    for (int i = 0; i < count; ++i) // ponytail: a scan per replace; a key index if replacing 100k rows often shows up
        if (guiRowKey(rows[static_cast<size_t>(i)], field) == key)
            return i;
    return -1;
}

void ListListener::attach(Rml::ElementDocument& document)
{
    Rml::ElementList found;
    document.GetElementsByClassName(found, "virtuallist");
    for (Rml::Element* element : found)
        makeVirtual(*element);
    found.clear();
    document.GetElementsByClassName(found, "listbox");
    for (Rml::Element* element : found)
        find(*element); // taken now, so data-follow sees its first rows added
}

std::optional<ListListener::Template> ListListener::rowTemplate(Rml::Element& list)
{
    Template found;
    for (int i = 0; i < list.GetNumChildren() && !found.element; ++i)
        if (list.GetChild(i)->HasAttribute("data-for"))
            found.element = list.GetChild(i);
    for (Rml::Element* at = &list; at && found.model.empty(); at = at->GetParentNode())
        found.model = at->GetAttribute<Rml::String>("data-model", "");
    const std::string expression = found.element ? found.element->GetAttribute<Rml::String>("data-for", "") : "";
    const size_t colon = expression.find(':');
    found.aliases = colon == std::string::npos ? "" : trim(std::string_view(expression).substr(0, colon));
    found.source = trim(colon == std::string::npos ? expression : std::string_view(expression).substr(colon + 1));
    if (!found.element || found.model.empty() || !guiVariableName(found.source))
        return std::nullopt;
    return found;
}

void ListListener::hold(Rml::ElementDocument& document)
{
    Rml::ElementList found;
    document.GetElementsByClassName(found, "virtuallist");
    for (Rml::Element* element : found)
        if (const std::optional<Template> row = rowTemplate(*element))
            models_.hold(row->model, row->source);
}

void ListListener::makeVirtual(Rml::Element& element)
{
    const std::optional<Template> row = rowTemplate(element);
    Rml::Context* context = element.GetContext();
    Rml::ElementDocument* document = element.GetOwnerDocument();
    GuiWindow* window = row && context && document
                            ? models_.window(*context, row->model, row->source)
                            : nullptr;
    if (!window) {
        log::warn(std::format("UI document '{}': virtual list '{}' needs a data model and one row template whose "
                              "data-for names a top-level array of it: shown as written", document_, element.GetId()));
        return;
    }
    Rml::Element& old = *row->element;
    const std::string& aliases = row->aliases;
    List& list = lists_.emplace_back();
    list.element = element.GetObserverPtr();
    list.window = window;
    list.model = row->model;
    list.keyField = element.GetAttribute<Rml::String>("data-key", "");
    list.rowHeight = rowHeightOf(element);
    list.version = window->version;
    list.rows = -1; // its source is held empty now: its rows are first counted at its first update

    // The template again, over the window, each row its fixed height: a data-for view is made with its element, so the
    // new one comes with a new element. Rows go before the template, between the spacers.
    Rml::ElementAttributes attributes = old.GetAttributes();
    attributes["data-for"] = Rml::Variant(aliases.empty() ? window->name : std::format("{} : {}", aliases, window->name));
    const std::string style = trim(old.GetAttribute<Rml::String>("style", ""));
    attributes["style"] = Rml::Variant(std::format("{}{}height: {}px; box-sizing: border-box;", style,
                                                   style.empty() || style.ends_with(';') ? "" : ";", list.rowHeight));
    Rml::ElementPtr replacement = document->CreateElement(old.GetTagName());
    replacement->SetAttributes(attributes);
    element.RemoveChild(&old); // and its data-for view
    list.top = spacer(*document, element, element.GetNumChildren() > 0 ? element.GetChild(0) : nullptr)->GetObserverPtr();
    element.AppendChild(std::move(replacement));
    list.bottom = spacer(*document, element, nullptr)->GetObserverPtr();
}

ListListener::List* ListListener::find(Rml::Element& element)
{
    const auto it = std::ranges::find_if(lists_, [&](const List& list) { return list.element.get() == &element; });
    if (it != lists_.end())
        return &*it;
    if (!element.IsClassSet("listbox") || element.IsClassSet("virtuallist"))
        return nullptr;
    List& list = lists_.emplace_back(); // made later (clone, create): taken on first use
    list.element = element.GetObserverPtr();
    list.rows = rowCount(list);
    return &list;
}

ListListener::List* ListListener::listOf(Rml::Element* element)
{
    for (; element && element != element->GetOwnerDocument(); element = element->GetParentNode())
        if (List* list = find(*element))
            return list;
    return nullptr;
}

std::vector<Rml::Element*> ListListener::rowElements(Rml::Element& element) const
{
    std::vector<Rml::Element*> rows;
    for (int i = 0; i < element.GetNumChildren(); ++i) {
        Rml::Element* child = element.GetChild(i);
        if (child->IsClassSet("row") && !child->HasAttribute("data-for")) // a data-for template is no row
            rows.push_back(child);
    }
    return rows;
}

int ListListener::rowCount(List& list)
{
    if (list.window)
        return list.window->source && list.window->source->is_array() ? static_cast<int>(list.window->source->size()) : 0;
    return static_cast<int>(rowElements(*list.element.get()).size());
}

int ListListener::selectedRow(List& list)
{
    if (list.window)
        return list.selected;
    const std::vector<Rml::Element*> rows = rowElements(*list.element.get());
    const auto it = std::ranges::find_if(rows, [](Rml::Element* row) { return row->IsClassSet("selected"); });
    return it == rows.end() ? -1 : static_cast<int>(it - rows.begin());
}

void ListListener::setSelected(List& list, int row, bool publish)
{
    const int before = selectedRow(list);
    if (list.window) {
        list.selected = row;
        list.key = row >= 0 && !list.keyField.empty() ? guiRowKey((*list.window->source)[static_cast<size_t>(row)], list.keyField)
                                                      : std::nullopt;
        markRows(list);
    } else {
        const std::vector<Rml::Element*> rows = rowElements(*list.element.get());
        for (size_t i = 0; i < rows.size(); ++i)
            rows[i]->SetClass("selected", static_cast<int>(i) == row);
    }
    Rml::Element& element = *list.element.get();
    const std::string name = element.GetAttribute<Rml::String>("data-event", "");
    if (publish && before != row && !name.empty())
        bus_.publish(name, {{"id", element.GetId()}, {"document", document_}, {"value", *value(element)}}, "UI");
}

void ListListener::markRows(List& list)
{
    const std::vector<Rml::Element*> rows = rowElements(*list.element.get());
    for (size_t i = 0; i < rows.size(); ++i)
        rows[i]->SetClass("selected", list.window->first + static_cast<int>(i) == list.selected);
}

bool ListListener::laidOut(List& list)
{
    if (list.element.get()->GetClientHeight() <= 0.0f)
        return false; // not laid out yet, or hidden
    if (!list.window)
        return list.pendingAge >= 1; // ponytail: a list box's rows are taken as laid out one update after asking
    Rml::Element* top = list.top.get();
    Rml::Element* bottom = list.bottom.get();
    if (!top || !bottom)
        return false;
    const float laid = bottom->GetOffsetTop() + bottom->GetOffsetHeight() - top->GetOffsetTop();
    return std::abs(laid - static_cast<float>(list.rows) * list.rowHeight) < 0.5f;
}

void ListListener::scrollIntoView(List& list, int row)
{
    Rml::Element& element = *list.element.get();
    if (row == kEnd) {
        element.SetScrollTop(element.GetScrollHeight());
        return;
    }
    // Offsets are from the list's border box; its view starts at its client (padding) top.
    float top = 0.0f;
    float height = list.rowHeight;
    if (list.window) {
        if (!list.top.get())
            return;
        top = list.top.get()->GetOffsetTop() - element.GetClientTop() + static_cast<float>(row) * list.rowHeight;
    } else {
        const std::vector<Rml::Element*> rows = rowElements(element);
        if (row < 0 || row >= static_cast<int>(rows.size()))
            return;
        top = rows[static_cast<size_t>(row)]->GetOffsetTop() - element.GetClientTop();
        height = rows[static_cast<size_t>(row)]->GetOffsetHeight();
    }
    element.SetScrollTop(guiScrollIntoView(element.GetScrollTop(), element.GetClientHeight(), top, height));
}

void ListListener::update()
{
    std::erase_if(lists_, [](const List& list) { return !list.element.get(); }); // removed (kast.gui.remove)
    for (List& list : lists_) {
        Rml::Element& element = *list.element.get();
        const int rows = rowCount(list);
        if (list.window && list.window->version != list.version) { // the source was replaced: follow the selection
            list.version = list.window->version;
            if (list.selected >= 0)
                list.selected = guiTrackRow(*list.window->source, list.keyField, list.selected, list.key);
            if (list.selected < 0)
                list.key.reset();
        }
        if (list.rows >= 0 && rows > list.rows && list.atEnd && !list.pending && element.HasAttribute("data-follow")) {
            list.pending = kEnd;
            list.pendingAge = 0;
        }
        list.rows = rows;
        if (list.pending && laidOut(list)) {
            scrollIntoView(list, *list.pending);
            list.pending.reset();
        } else if (list.pending) {
            ++list.pendingAge;
        } else if (!list.window || laidOut(list)) {
            list.atEnd = element.GetScrollTop() + element.GetClientHeight() >= element.GetScrollHeight() - 1.0f;
        }
        if (!list.window)
            continue;
        GuiWindow& window = *list.window;
        const GuiRowRange range = guiVisibleRows(element.GetScrollTop(), element.GetClientHeight(), list.rowHeight, rows);
        if (range.first != window.first || range.count != window.count) {
            window.first = range.first;
            window.count = range.count;
            if (Rml::Context* context = element.GetContext())
                models_.dirty(*context, list.model, window);
        }
        const auto setHeight = [](Rml::Element* spacer, float& last, float height) {
            if (spacer && height != last) // setting it at all relays the list out
                spacer->SetProperty(Rml::PropertyId::Height, Rml::Property(height, Rml::Unit::PX));
            last = height;
        };
        setHeight(list.top.get(), list.topHeight, static_cast<float>(range.first) * list.rowHeight);
        setHeight(list.bottom.get(), list.bottomHeight, static_cast<float>(rows - range.first - range.count) * list.rowHeight);
        markRows(list);
    }
}

void ListListener::ProcessEvent(Rml::Event& event)
{
    Rml::Element* target = event.GetTargetElement();
    if (event.GetId() == Rml::EventId::Load) {
        if (auto* document = dynamic_cast<Rml::ElementDocument*>(target))
            hold(*document);
        return;
    }
    List* list = listOf(target);
    if (!list)
        return;
    Rml::Element& element = *list->element.get();
    if (event.GetId() == Rml::EventId::Click) {
        element.Focus(); // its keys move the selection (rows take no focus: focus: none in the default style)
        Rml::Element* row = target;
        while (row && row->GetParentNode() != &element)
            row = row->GetParentNode();
        const std::vector<Rml::Element*> rows = rowElements(element);
        const auto it = std::ranges::find(rows, row);
        if (it != rows.end())
            setSelected(*list, (list->window ? list->window->first : 0) + static_cast<int>(it - rows.begin()), true);
    } else if (event.GetId() == Rml::EventId::Keydown) {
        const std::optional<GuiListKey> key = listKey(event.GetParameter<int>("key_identifier", 0));
        if (!key)
            return;
        float rowHeight = list->rowHeight;
        if (!list->window)
            if (const std::vector<Rml::Element*> rows = rowElements(element); !rows.empty() && rows[0]->GetOffsetHeight() > 0.0f)
                rowHeight = rows[0]->GetOffsetHeight();
        const int page = static_cast<int>(element.GetClientHeight() / rowHeight);
        const int row = guiListMove(*key, selectedRow(*list), rowCount(*list), page);
        if (row < 0)
            return;
        setSelected(*list, row, true);
        scrollIntoView(*list, row);
        event.StopPropagation();
    }
}

std::optional<GuiValue> ListListener::value(Rml::Element& element)
{
    List* list = find(element);
    if (!list)
        return std::nullopt;
    const int row = selectedRow(*list);
    if (row < 0)
        return GuiValue(nullptr);
    if (list->window) {
        if (!list->keyField.empty())
            if (std::optional<std::string> key = guiRowKey((*list->window->source)[static_cast<size_t>(row)], list->keyField))
                return GuiValue(*key);
        return GuiValue(static_cast<double>(row));
    }
    Rml::Element* selected = rowElements(element)[static_cast<size_t>(row)];
    if (selected->HasAttribute("data-value"))
        return GuiValue(selected->GetAttribute<Rml::String>("data-value", ""));
    return GuiValue(static_cast<double>(row));
}

bool ListListener::select(Rml::Element& element, const GuiValue& value, std::string& error)
{
    const auto fail = [&](std::string why) {
        error = std::move(why);
        return false;
    };
    List* list = find(element);
    if (!list)
        return fail("it is not a list");
    const int rows = rowCount(*list);
    int row = -1;
    if (value.is_number()) {
        const double index = value.get<double>();
        if (index != std::floor(index) || index < 0.0 || index >= rows)
            return fail("the list has no row at that index");
        row = static_cast<int>(index);
    } else if (value.is_string()) {
        const std::string& wanted = value.get_ref<const std::string&>();
        if (list->window) {
            if (list->keyField.empty())
                return fail("the virtual list has no data-key: select by index");
            for (int i = 0; i < rows && row < 0; ++i) // ponytail: a scan per setValue
                if (guiRowKey((*list->window->source)[static_cast<size_t>(i)], list->keyField) == wanted)
                    row = i;
        } else {
            const std::vector<Rml::Element*> elements = rowElements(element);
            for (int i = 0; i < rows && row < 0; ++i)
                if (elements[static_cast<size_t>(i)]->HasAttribute("data-value") &&
                    elements[static_cast<size_t>(i)]->GetAttribute<Rml::String>("data-value", "") == wanted)
                    row = i;
        }
        if (row < 0)
            return fail("the list has no row with that value");
    } else if (!value.is_null()) {
        return fail("a list takes a string, a number or nil");
    }
    setSelected(*list, row, false);
    return true;
}

bool ListListener::scrollTo(Rml::Element& element, int index, std::string& error)
{
    List* list = find(element);
    if (!list) {
        error = "it is not a list";
        return false;
    }
    if (index != kEnd && (index < 0 || index >= rowCount(*list))) {
        error = "the list has no row at that index";
        return false;
    }
    list->pending = index;
    list->pendingAge = 0;
    return true;
}

} // namespace kast
