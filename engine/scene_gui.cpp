#include "scene_gui.h"

#include "files.h"
#include "log.h"
#include "project.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/StyleSheetContainer.h>

#include <algorithm>
#include <format>
#include <regex>

namespace kast {
namespace {

// What WidgetListener hears. Focus and blur don't bubble, and a text field stops the keys it uses: those are heard on
// the way down (capture).
constexpr Rml::EventId kWidgetEvents[] = {Rml::EventId::Change, Rml::EventId::Click, Rml::EventId::Keydown,
                                          Rml::EventId::Focus, Rml::EventId::Blur};
bool capture(Rml::EventId id) { return id == Rml::EventId::Keydown || id == Rml::EventId::Focus || id == Rml::EventId::Blur; }
constexpr Rml::EventId kListEvents[] = {Rml::EventId::Click, Rml::EventId::Keydown}; // what ListListener hears

std::filesystem::file_time_type modified(const std::filesystem::path& file)
{
    std::error_code ec;
    return std::filesystem::last_write_time(file, ec); // disk only: under a pak, never changes
}

// The .rml file plus every file its <link href="..."> elements name, resolved next to it.
std::vector<std::filesystem::path> documentFiles(const std::filesystem::path& rml, const std::string& text)
{
    std::vector<std::filesystem::path> files{rml};
    static const std::regex link(R"re(<link\b[^>]*\bhref\s*=\s*"([^"]+)")re", std::regex::icase);
    for (auto it = std::sregex_iterator(text.begin(), text.end(), link); it != std::sregex_iterator(); ++it)
        files.push_back((rml.parent_path() / std::filesystem::path(reinterpret_cast<const char8_t*>((*it)[1].str().c_str())))
                            .lexically_normal());
    return files;
}

// Typed into: a <textarea>, or an <input> of a text type (text is the default).
bool isTextField(const Rml::Element& element)
{
    if (element.GetTagName() == "textarea")
        return true;
    if (element.GetTagName() != "input")
        return false;
    const Rml::String type = element.GetAttribute<Rml::String>("type", "text");
    return type == "text" || type == "password";
}

} // namespace

void SceneGui::sync(const Scene& scene, const std::filesystem::path& projectRoot, EventBus& bus)
{
    root_ = projectRoot;
    bus_ = &bus;
    std::vector<Entry> wanted;
    const entt::registry& registry = scene.registry();
    for (auto [entity, component, id] : registry.view<const UIDocument, const PersistentId>().each())
        if (!component.path.empty())
            wanted.push_back({.entity = entity, .id = id.value, .path = component.path, .visible = component.visible});
    std::ranges::sort(wanted, {}, &Entry::id);

    bool changed = false;
    std::vector<Entry> next;
    next.reserve(wanted.size());
    for (Entry& want : wanted) {
        auto old = std::ranges::find_if(entries_, [&](const Entry& entry) {
            return entry.entity == want.entity && entry.path == want.path;
        });
        if (old != entries_.end()) {
            next.push_back(std::move(*old));
            entries_.erase(old);
        } else {
            load(want, bus);
            next.push_back(std::move(want));
            changed = true;
        }
        Entry& entry = next.back();
        if (entry.document && entry.document->IsVisible() != want.visible) {
            want.visible ? entry.document->Show() : entry.document->Hide();
        }
        entry.visible = want.visible;
    }
    changed = changed || !entries_.empty();
    for (Entry& gone : entries_)
        unload(gone);
    entries_ = std::move(next);
    if (changed)
        restack();
    for (Entry& entry : entries_)
        if (entry.document)
            entry.listListener->update();
}

void SceneGui::clear()
{
    for (Entry& entry : entries_)
        unload(entry);
    entries_.clear();
}

void SceneGui::reloadChanged()
{
    bool changed = false;
    for (Entry& entry : entries_) {
        const bool stale = std::ranges::any_of(entry.files, [](const auto& file) { return modified(file.first) != file.second; });
        if (!stale || !bus_)
            continue;
        log::info(std::format("UI document '{}' changed: reloading", entry.path));
        Rml::Factory::ClearStyleSheetCache(); // else the old .rcss is reused
        Rml::Factory::ClearTemplateCache();
        unload(entry);
        load(entry, *bus_);
        changed = true;
    }
    if (changed)
        restack();
}

bool SceneGui::click(const std::string& id)
{
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
        if (it->document && it->visible)
            if (Rml::Element* element = it->document->GetElementById(id)) {
                element->Click();
                return true;
            }
    return false;
}

std::optional<GuiValue> SceneGui::value(const std::string& id)
{
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
        if (Rml::Element* element = it->document ? it->document->GetElementById(id) : nullptr) {
            if (std::optional<GuiValue> value = it->listListener->value(*element))
                return value;
            if (std::optional<GuiValue> value = guiWidgetValue(*element))
                return value;
        }
    return std::nullopt;
}

void SceneGui::setValue(const std::string& id, GuiValue value)
{
    bool found = false, applied = false;
    for (Entry& entry : entries_)
        if (Rml::Element* element = entry.document ? entry.document->GetElementById(id) : nullptr) {
            found = true;
            applied = applyValue(entry, *element, id, value) || applied;
        }
    if (applied || !found) // a value no loaded widget took isn't kept (it would only warn again)
        written_[id].value = std::move(value);
}

void SceneGui::scrollTo(const std::string& id, int index) { scroll(id, index, "scrollTo"); }

void SceneGui::scrollToEnd(const std::string& id) { scroll(id, ListListener::kEnd, "scrollToEnd"); }

void SceneGui::scroll(const std::string& id, int index, const char* function)
{
    const std::string call = index == ListListener::kEnd ? std::format("kast.gui.{}('{}')", function, id)
                                                         : std::format("kast.gui.{}('{}', {})", function, id, index);
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
        if (Rml::Element* element = it->document ? it->document->GetElementById(id) : nullptr) {
            std::string error;
            if (!it->listListener->scrollTo(*element, index, error))
                log::warn(std::format("{}: {}: unchanged", call, error));
            return;
        }
    log::warn(std::format("{}: no loaded GUI has it", call));
}

void SceneGui::setText(const std::string& id, std::string_view text)
{
    Written& written = written_[id];
    written.text = std::string(text);
    written.rich.reset();
    for (Entry& entry : entries_)
        if (entry.document)
            apply(entry, *entry.document, id, {.text = written.text});
}

void SceneGui::setRichText(const std::string& id, std::string_view text)
{
    Written& written = written_[id];
    written.rich = std::string(text);
    written.text.reset();
    for (Entry& entry : entries_)
        if (entry.document)
            apply(entry, *entry.document, id, {.rich = written.rich});
}

void SceneGui::setVisible(const std::string& id, bool visible)
{
    written_[id].visible = visible;
    for (Entry& entry : entries_)
        if (entry.document)
            apply(entry, *entry.document, id, {.visible = visible});
}

void SceneGui::setAttribute(const std::string& id, const std::string& name, std::string_view value)
{
    written_[id].attributes[name] = std::string(value);
    for (Entry& entry : entries_)
        if (entry.document)
            apply(entry, *entry.document, id, {.attributes = {{name, std::string(value)}}});
}

bool SceneGui::focus(const std::string& id)
{
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
        if (it->document && it->visible)
            if (Rml::Element* element = it->document->GetElementById(id))
                return element->Focus();
    return false;
}

std::optional<std::string> SceneGui::focusedField()
{
    Rml::Context* context = gui::ready() ? context_.context() : nullptr;
    Rml::Element* focus = context ? context->GetFocusElement() : nullptr;
    if (!focus || !isTextField(*focus))
        return std::nullopt;
    return focus->GetId();
}

bool SceneGui::mouseOver()
{
    Rml::Context* context = gui::ready() ? context_.context() : nullptr;
    Rml::Element* hover = context ? context->GetHoverElement() : nullptr;
    return hover && hover != context->GetRootElement() && hover != hover->GetOwnerDocument();
}

std::optional<std::string> SceneGui::hovered()
{
    Rml::Context* context = gui::ready() ? context_.context() : nullptr;
    const std::string id = guiElementId(context ? context->GetHoverElement() : nullptr);
    return id.empty() ? std::nullopt : std::optional(id);
}

void SceneGui::setModelValue(const std::string& model, const std::string& key, GuiValue value)
{
    models_.set(gui::ready() ? context_.context() : nullptr, model, key, std::move(value));
}

std::optional<GuiValue> SceneGui::modelValue(const std::string& model, const std::string& key)
{
    return models_.get(model, key);
}

bool SceneGui::cloneElement(const std::string& templateId, const std::string& parentId, const std::string& newId)
{
    Rml::ElementDocument* document = documentWith(parentId);
    Rml::Element* source = document ? document->GetElementById(templateId) : nullptr;
    if (!document || !source || newId.empty() || document->GetElementById(newId)) {
        log::warn(std::format("kast.gui.clone('{}', '{}', '{}'): {}", templateId, parentId, newId,
                              !document  ? "no loaded GUI has the parent"
                              : !source  ? "the parent's GUI has no such template"
                              : newId.empty() ? "the new id is empty"
                                         : "the new id is taken in that GUI"));
        return false;
    }
    Rml::ElementPtr copy = source->Clone();
    copy->SetId(newId);
    copy->RemoveProperty("display");
    const std::string prefix = templateId + "_";
    const auto rename = [&](auto&& self, Rml::Element& element) -> void {
        for (int i = 0; i < element.GetNumChildren(); ++i) {
            Rml::Element& child = *element.GetChild(i);
            if (child.GetId().starts_with(prefix))
                child.SetId(newId + "_" + child.GetId().substr(prefix.size()));
            self(self, child);
        }
    };
    rename(rename, *copy);
    document->GetElementById(parentId)->AppendChild(std::move(copy));
    return true;
}

bool SceneGui::createElement(const std::string& parentId, const std::string& tag, const std::string& newId,
                             const std::map<std::string, std::string>& attributes)
{
    Rml::ElementDocument* document = documentWith(parentId);
    if (!document || tag.empty() || newId.empty() || document->GetElementById(newId)) {
        log::warn(std::format("kast.gui.create('{}', '{}', '{}'): {}", parentId, tag, newId,
                              !document      ? "no loaded GUI has the parent"
                              : tag.empty()  ? "the tag is empty"
                              : newId.empty() ? "the new id is empty"
                                             : "the new id is taken in that GUI"));
        return false;
    }
    Rml::ElementPtr element = document->CreateElement(tag);
    for (const auto& [name, value] : attributes)
        element->SetAttribute(name, value);
    element->SetId(newId);
    document->GetElementById(parentId)->AppendChild(std::move(element));
    return true;
}

bool SceneGui::instantiatePrefab(const std::string& prefab, const std::string& parentId, const std::string& newId)
{
    const auto fail = [&](std::string_view reason) {
        log::warn(std::format("kast.gui.instantiate('{}', '{}', '{}'): {}", prefab, parentId, newId, reason));
        return false;
    };
    Rml::ElementDocument* document = documentWith(parentId);
    if (!document)
        return fail("no loaded GUI has the parent");
    if (newId.empty())
        return fail("the new id is empty");
    const auto entry = std::ranges::find(entries_, document, &Entry::document);
    GuiPrefabExpansion expansion{.document = entry->path, .read = prefabReader()};
    const std::optional<std::string> rml = guiPrefabInstanceRml(prefab, newId, expansion);
    if (!rml)
        return fail(std::format("the prefab can't be expanded: {}", expansion.error));
    for (const std::string& id : guiMarkupIds(*rml))
        if (document->GetElementById(id))
            return fail(std::format("the id '{}' is taken in that GUI", id));
    // The prefab's sheet joins the document's own, after them (a sheet the document had from loading stays as it was).
    for (const std::string& sheet : expansion.sheets) {
        std::string ignored;
        const auto file = resolveProjectPath(root_, sheet, ignored);
        if (std::ranges::find(entry->prefabSheets, sheet) != entry->prefabSheets.end() || !file)
            continue;
        if (const Rml::SharedPtr<Rml::StyleSheetContainer> added = Rml::Factory::InstanceStyleSheetFile(gui::toRmlPath(*file))) {
            const Rml::StyleSheetContainer* own = document->GetStyleSheetContainer();
            document->SetStyleSheetContainer(own ? own->CombineStyleSheetContainer(*added) : added);
        }
        entry->prefabSheets.push_back(sheet);
    }
    Rml::Element* parent = document->GetElementById(parentId);
    Rml::Element* holder = parent->AppendChild(document->CreateElement("div"));
    holder->SetInnerRML(*rml);
    Rml::ElementPtr instance = holder->GetFirstChild() ? holder->RemoveChild(holder->GetFirstChild()) : nullptr;
    if (!instance) {
        parent->RemoveChild(holder);
        return fail("the prefab's markup didn't load (see above)");
    }
    Rml::Element* added = instance.get();
    parent->ReplaceChild(std::move(instance), holder);
    gui::convertRichText(*added);
    return true;
}

GuiPrefabReader SceneGui::prefabReader() const
{
    return [root = root_](const std::string& path) -> std::optional<std::string> {
        std::string ignored, text;
        const std::optional<std::filesystem::path> file = root.empty() ? std::nullopt : resolveProjectPath(root, path, ignored);
        return file && readFile(*file, text) ? std::optional(std::move(text)) : std::nullopt;
    };
}

bool SceneGui::removeElement(const std::string& id)
{
    Rml::ElementDocument* document = documentWith(id);
    Rml::Element* element = document ? document->GetElementById(id) : nullptr;
    // ponytail: "made by data-for" is read off the DOM (RmlUi inserts the rows before the hidden data-for template,
    // same tag); an authored element of that tag just before a data-for one is refused too.
    const auto madeByFor = [](Rml::Element* e) {
        for (; e && e != e->GetOwnerDocument(); e = e->GetParentNode())
            for (Rml::Element* next = e->GetNextSibling(); next; next = next->GetNextSibling())
                if (next->HasAttribute("data-for") && next->GetTagName() == e->GetTagName())
                    return true;
        return false;
    };
    if (!element || element == document || madeByFor(element)) {
        log::warn(std::format("kast.gui.remove('{}'): {}", id,
                              !element              ? "no loaded GUI has it"
                              : element == document ? "a document's <body> can't be removed"
                                                    : "it is (inside) a data-for row: change the model instead"));
        return false;
    }
    element->GetParentNode()->RemoveChild(element);
    return true;
}

void SceneGui::reset()
{
    clear();
    written_.clear();
    models_.clear(gui::ready() ? context_.context() : nullptr);
}

Rml::ElementDocument* SceneGui::documentWith(const std::string& id) const
{
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
        if (it->document && it->document->GetElementById(id))
            return it->document;
    return nullptr;
}

void SceneGui::apply(Entry& entry, Rml::ElementDocument& document, const std::string& id, const Written& written)
{
    Rml::Element* element = document.GetElementById(id);
    if (!element)
        return;
    if (written.text || written.rich) {
        if (auto* control = dynamic_cast<Rml::ElementFormControl*>(element))
            control->SetValue(written.text ? *written.text : *written.rich);
        else
            element->SetInnerRML(written.text ? guiTextRml(*written.text)
                                              : guiRichTextRml(*written.rich, nullptr, gui::fontFamilyLoaded));
    }
    for (const auto& [name, value] : written.attributes)
        element->SetAttribute(name, value);
    if (written.visible) {
        if (*written.visible)
            element->RemoveProperty("display"); // back to the style sheet's
        else
            element->SetProperty("display", "none");
    }
    if (written.value)
        applyValue(entry, *element, id, *written.value);
}

bool SceneGui::applyValue(Entry& entry, Rml::Element& element, const std::string& id, const GuiValue& value)
{
    std::string error;
    quiet_ = true;
    const bool list = entry.listListener->value(element).has_value();
    const bool applied = list ? entry.listListener->select(element, value, error) : setGuiWidgetValue(element, value, error);
    quiet_ = false;
    if (!applied)
        log::warn(std::format("kast.gui.setValue('{}'): {}: unchanged", id, error));
    return applied;
}

void SceneGui::load(Entry& entry, EventBus& bus)
{
    entry.document = nullptr;
    entry.files.clear();
    Rml::Context* context = context_.context();
    if (!context)
        return;
    std::string reason = "no project is open";
    const std::optional<std::filesystem::path> file =
        root_.empty() ? std::nullopt : resolveProjectPath(root_, entry.path, reason);
    if (!file) {
        log::error(std::format("UI document '{}': {}", entry.path, reason));
        return;
    }
    std::string text;
    const bool found = readFile(*file, text);
    for (const std::filesystem::path& part : documentFiles(*file, text))
        entry.files.emplace_back(part, modified(part));
    if (!found) {
        log::error(std::format("UI document '{}' not found", entry.path));
        return;
    }
    // GUI prefab instances expanded first (their warnings logged), their files watched for hot reload too.
    GuiPrefabExpansion prefabs{.document = entry.path, .read = prefabReader()};
    const std::string expanded = guiExpandPrefabsRml(text, prefabs);
    for (const std::string& path : prefabs.files)
        if (std::string ignored; const auto part = resolveProjectPath(root_, path, ignored))
            entry.files.emplace_back(*part, modified(*part));
    entry.prefabSheets = prefabs.sheets;
    gui::loadProjectFonts(root_); // first: a bad font file's RmlUi error isn't this document's
    const uint64_t problems = gui::problemCount();
    const std::string rml = guiRichBindings(expanded);
    models_.prepare(*context, rml);
    // Its virtual lists' sources read as empty while it loads, until their row templates are over windows
    // (ListListener::hold, attach).
    entry.listListener = std::make_unique<ListListener>(bus, entry.path, models_);
    context->AddEventListener("load", entry.listListener.get(), true);
    Rml::ElementDocument* document = context->LoadDocumentFromMemory(rml, gui::toRmlPath(*file));
    context->RemoveEventListener("load", entry.listListener.get(), true);
    if (document) {
        entry.listListener->attach(*document);
        gui::convertRichText(*document);
    }
    models_.restore(context);
    if (!document || gui::problemCount() != problems) {
        log::error(std::format("UI document '{}' (or its style sheet) has errors, see above: not shown", entry.path));
        if (document)
            document->Close();
        return;
    }
    gui::addWidgetStyle(*document);
    entry.listener = std::make_unique<ButtonClickListener>(bus, entry.path);
    document->AddEventListener(Rml::EventId::Click, entry.listener.get());
    entry.dragListener = std::make_unique<DragListener>(bus, entry.path, [this](const Rml::Element& d) { return documentPath(d); });
    document->AddEventListener(Rml::EventId::Dragstart, entry.dragListener.get());
    document->AddEventListener(Rml::EventId::Dragend, entry.dragListener.get());
    entry.widgetListener = std::make_unique<WidgetListener>(bus, entry.path, quiet_);
    for (const Rml::EventId id : kWidgetEvents)
        document->AddEventListener(id, entry.widgetListener.get(), capture(id));
    for (const Rml::EventId id : kListEvents)
        document->AddEventListener(id, entry.listListener.get());
    for (const auto& [id, written] : written_) // what Lua wrote before this document loaded
        apply(entry, *document, id, written);
    if (entry.visible)
        document->Show();
    entry.document = document;
}

void SceneGui::unload(Entry& entry)
{
    if (entry.document && gui::ready()) {
        entry.document->RemoveEventListener(Rml::EventId::Click, entry.listener.get());
        entry.document->RemoveEventListener(Rml::EventId::Dragstart, entry.dragListener.get());
        entry.document->RemoveEventListener(Rml::EventId::Dragend, entry.dragListener.get());
        for (const Rml::EventId id : kWidgetEvents)
            entry.document->RemoveEventListener(id, entry.widgetListener.get(), capture(id));
        for (const Rml::EventId id : kListEvents)
            entry.document->RemoveEventListener(id, entry.listListener.get());
        entry.document->Close();
    }
    entry.document = nullptr;
    entry.listener.reset();
    entry.dragListener.reset();
    entry.widgetListener.reset();
    entry.listListener.reset();
}

void SceneGui::restack()
{
    for (Entry& entry : entries_) // later ids on top
        if (entry.document)
            entry.document->PullToFront();
}

std::string SceneGui::documentPath(const Rml::Element& document) const
{
    for (const Entry& entry : entries_)
        if (entry.document == &document)
            return entry.path;
    return "";
}

} // namespace kast
