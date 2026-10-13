#include "scene_gui.h"

#include "files.h"
#include "log.h"
#include "project.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Factory.h>

#include <algorithm>
#include <format>
#include <regex>

namespace kast {
namespace {

std::string utf8Path(const std::filesystem::path& path)
{
    const std::u8string text = path.generic_u8string();
    return {text.begin(), text.end()};
}

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
    const uint64_t problems = gui::problemCount();
    Rml::ElementDocument* document = context->LoadDocument(utf8Path(*file));
    if (!document || gui::problemCount() != problems) {
        log::error(std::format("UI document '{}' (or its style sheet) has errors, see above: not shown", entry.path));
        if (document)
            document->Close();
        return;
    }
    entry.listener = std::make_unique<ButtonClickListener>(bus, entry.path);
    document->AddEventListener(Rml::EventId::Click, entry.listener.get());
    if (entry.visible)
        document->Show();
    entry.document = document;
}

void SceneGui::unload(Entry& entry)
{
    if (entry.document && gui::ready()) {
        entry.document->RemoveEventListener(Rml::EventId::Click, entry.listener.get());
        entry.document->Close();
    }
    entry.document = nullptr;
    entry.listener.reset();
}

void SceneGui::restack()
{
    for (Entry& entry : entries_) // later ids on top
        if (entry.document)
            entry.document->PullToFront();
}

} // namespace kast
