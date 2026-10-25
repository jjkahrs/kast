#include "gui_models.h"

#include "gui.h"
#include "gui_richtext.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/DataTypeRegister.h>
#include <RmlUi/Core/DataVariable.h>
#include <RmlUi/Core/Variant.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <regex>

namespace kast {
namespace {

bool isLetter(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; }
bool isWordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }
bool isSpace(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

std::string_view trim(std::string_view text)
{
    while (!text.empty() && isSpace(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && isSpace(text.back()))
        text.remove_suffix(1);
    return text;
}

// An XML attribute value as RmlUi reads it.
std::string unescape(std::string_view text)
{
    static constexpr std::pair<std::string_view, char> kEntities[] = {
        {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&amp;", '&'}};
    std::string out;
    for (size_t i = 0; i < text.size();) {
        const auto entity = std::ranges::find_if(kEntities, [&](const auto& e) { return text.substr(i).starts_with(e.first); });
        if (entity != std::end(kEntities)) {
            out += entity->second;
            i += entity->first.size();
        } else {
            out += text[i++];
        }
    }
    return out;
}

// Adds the names expression reads at the start of an address (rows in rows[2].name) to out.
void expressionNames(std::string_view expression, std::set<std::string>& out)
{
    for (size_t i = 0; i < expression.size();) {
        const char c = expression[i];
        if (c == '\'' || c == '"') { // a string literal
            const size_t end = expression.find(c, i + 1);
            i = end == std::string_view::npos ? expression.size() : end + 1;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) { // a number literal or index
            while (i < expression.size() && (isWordChar(expression[i]) || expression[i] == '.'))
                ++i;
            continue;
        }
        if (!isLetter(c) && c != '_') {
            ++i;
            continue;
        }
        const size_t start = i;
        while (i < expression.size() && isWordChar(expression[i]))
            ++i;
        size_t before = start, after = i;
        while (before > 0 && isSpace(expression[before - 1]))
            --before;
        while (after < expression.size() && isSpace(expression[after]))
            ++after;
        const char previous = before > 0 ? expression[before - 1] : '\0';
        const char next = after < expression.size() ? expression[after] : '\0';
        if (previous != '.' && previous != '|' && next != '(') // not a member, transform or event callback
            out.emplace(expression.substr(start, i - start));
    }
}

// Reads and writes one GuiValue node for RmlUi: scalars as Variants, arrays by index (and .size), tables by key. One
// definition for every node, whatever it holds now: RmlUi asks it, never its DataVariableType, how to read a node.
class ValueDefinition final : public Rml::VariableDefinition {
public:
    ValueDefinition() : Rml::VariableDefinition(Rml::DataVariableType::Scalar) {}

    bool Get(void* ptr, Rml::Variant& variant) override
    {
        const GuiValue& value = *static_cast<const GuiValue*>(ptr);
        if (value.is_string())
            variant = value.get_ref<const std::string&>();
        else if (value.is_boolean())
            variant = value.get<bool>();
        else if (value.is_number())
            variant = value.get<double>();
        else if (value.is_null())
            variant = Rml::Variant(); // unset: reads as "" and false
        else
            return false; // an array or table has no single value
        return true;
    }

    // Two-way bound fields (data-value, data-checked): a checkbox gives a bool; a text field, range or select a string,
    // kept a number if the node held one and the text is one.
    bool Set(void* ptr, const Rml::Variant& variant) override
    {
        GuiValue& value = *static_cast<GuiValue*>(ptr);
        switch (variant.GetType()) {
        case Rml::Variant::BOOL: value = variant.Get<bool>(); return true;
        case Rml::Variant::STRING: {
            const std::string text = variant.Get<Rml::String>();
            double number = 0.0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
            if (value.is_number() && error == std::errc() && end == text.data() + text.size())
                value = number;
            else
                value = text;
            return true;
        }
        default: {
            double number = 0.0;
            if (!variant.GetInto(number))
                return false;
            value = number;
            return true;
        }
        }
    }

    int Size(void* ptr) override
    {
        const GuiValue& value = *static_cast<const GuiValue*>(ptr);
        return value.is_array() ? static_cast<int>(value.size()) : 0;
    }

    Rml::DataVariable Child(void* ptr, const Rml::DataAddressEntry& address) override
    {
        GuiValue& value = *static_cast<GuiValue*>(ptr);
        if (value.is_array()) {
            if (address.index >= 0 && address.index < static_cast<int>(value.size()))
                return Rml::DataVariable(this, &value[static_cast<size_t>(address.index)]);
            if (address.name == "size")
                return Rml::MakeLiteralIntVariable(static_cast<int>(value.size()));
        } else if (value.is_object()) {
            if (const auto it = value.find(address.name); it != value.end())
                return Rml::DataVariable(this, &*it);
        }
        return {};
    }
};

ValueDefinition& valueDefinition()
{
    static ValueDefinition definition;
    return definition;
}

// What a row past a window's end reads as, members and all: empty. A data-for's rows outlive a shrink until it removes
// them, and their views (at the same depth) may update first.
class GoneRowDefinition final : public Rml::VariableDefinition {
public:
    GoneRowDefinition() : Rml::VariableDefinition(Rml::DataVariableType::Scalar) {}
    bool Get(void*, Rml::Variant& variant) override
    {
        variant = Rml::Variant();
        return true;
    }
    Rml::DataVariable Child(void*, const Rml::DataAddressEntry&) override { return Rml::DataVariable(this, nullptr); }
};

// A GuiWindow as an array: its rows are the source's own nodes, read through ValueDefinition (nothing is copied).
class WindowDefinition final : public Rml::VariableDefinition {
public:
    WindowDefinition() : Rml::VariableDefinition(Rml::DataVariableType::Array) {}

    int Size(void* ptr) override
    {
        const GuiWindow& window = *static_cast<const GuiWindow*>(ptr);
        const int rows = window.source && window.source->is_array() ? static_cast<int>(window.source->size()) : 0;
        return std::clamp(rows - window.first, 0, window.count);
    }

    Rml::DataVariable Child(void* ptr, const Rml::DataAddressEntry& address) override
    {
        const GuiWindow& window = *static_cast<const GuiWindow*>(ptr);
        if (address.index >= 0 && address.index < Size(ptr))
            return Rml::DataVariable(&valueDefinition(), &(*window.source)[static_cast<size_t>(window.first + address.index)]);
        static GoneRowDefinition gone;
        if (address.index >= 0)
            return Rml::DataVariable(&gone, nullptr);
        if (address.name == "size")
            return Rml::MakeLiteralIntVariable(Size(ptr));
        return {};
    }
};

WindowDefinition& windowDefinition()
{
    static WindowDefinition definition;
    return definition;
}

bool hasModel(Rml::Context& context, const std::string& model) { return context.GetDataModels().count(model) > 0; }

} // namespace

GuiBindingNames guiBindingNames(std::string_view rml)
{
    static const std::regex comment(R"(<!--[\s\S]*?-->)");
    static const std::regex attribute(R"re(\sdata-([A-Za-z]+)(?:-([^\s=/>]*))?\s*=\s*(?:"([^"]*)"|'([^']*)'))re");
    static const std::set<std::string, std::less<>> kViews = {"attr", "attrif", "class", "if", "visible", "rml",
                                                              "style", "text", "value", "checked", "rich"};
    const std::string text = std::regex_replace(std::string(rml), comment, "");
    GuiBindingNames names;
    std::set<std::string> aliases, read;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), attribute); it != std::sregex_iterator(); ++it) {
        std::string type = (*it)[1].str();
        std::ranges::transform(type, type.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
        const std::string modifier = (*it)[2].str();
        const std::string value = unescape((*it)[3].matched ? (*it)[3].str() : (*it)[4].str());
        std::string_view expression = value;
        if (type == "model") {
            names.models.emplace(trim(value));
            continue;
        } else if (type == "for") { // "[item[, index] :] array"
            if (const size_t colon = value.find(':'); colon != std::string::npos) {
                std::string_view pair = std::string_view(value).substr(0, colon);
                for (size_t comma; (comma = pair.find(',')) != std::string_view::npos; pair.remove_prefix(comma + 1))
                    aliases.emplace(trim(pair.substr(0, comma)));
                aliases.emplace(trim(pair));
                expression = std::string_view(value).substr(colon + 1);
            }
        } else if (type == "alias") {
            aliases.insert(modifier);
        } else if (type == "event") {
            if (modifier.empty()) // kast's own data-event="name" (gui_events.h): an event name, not an expression
                continue;
        } else if (!kViews.contains(type)) {
            continue; // not RmlUi's (data-drag, data-kast-node, ...)
        }
        expressionNames(expression, read);
    }
    for (size_t at = text.find("{{"); at != std::string::npos; at = text.find("{{", at)) {
        const size_t end = text.find("}}", at + 2);
        if (end == std::string::npos)
            break;
        expressionNames(std::string_view(text).substr(at + 2, end - at - 2), read);
        at = end + 2;
    }
    for (const std::string& name : read)
        if (!aliases.contains(name) && guiVariableName(name))
            names.variables.insert(name);
    return names;
}

std::string guiRichBindings(std::string_view rml)
{
    static const std::regex rich(R"re(\sdata-rich\s*=\s*(?:"([^"]*)"|'([^']*)'))re");
    std::string out;
    const std::string text(rml);
    auto last = text.cbegin();
    for (auto it = std::sregex_iterator(text.begin(), text.end(), rich); it != std::sregex_iterator(); ++it) {
        const std::smatch& match = *it;
        const bool doubled = match[1].matched;
        out.append(last, match[0].second);
        out += doubled ? std::format(R"( data-rml="({}) | rich")", match[1].str())
                       : std::format(" data-rml='({}) | rich'", match[2].str());
        last = match[0].second;
    }
    out.append(last, text.cend());
    return out;
}

// ponytail: a name stays bound for the context's life (RmlUi can't unbind one); a hot reload that turns a bound name
// into a data-for / data-alias name gets RmlUi's "shadowed" warning (document rejected) until Play restarts.
void GuiModels::prepare(Rml::Context& context, std::string_view rml)
{
    const GuiBindingNames names = guiBindingNames(rml);
    for (const std::string& model : names.models) {
        if (!hasModel(context, model))
            if (Rml::DataModelConstructor constructor = context.CreateDataModel(model)) {
                // Transforms belong to the context, shared by its models: registered once (a second time is an error).
                Rml::TransformFuncRegister& transforms = *constructor.GetDataTypeRegister()->GetTransformFuncRegister();
                Rml::Variant unused;
                if (!transforms.Call("rich", {Rml::Variant(Rml::String())}, unused))
                    transforms.Register("rich", [](const Rml::VariantList& arguments) {
                        const Rml::String bbcode = arguments.empty() ? Rml::String() : arguments[0].Get<Rml::String>();
                        return Rml::Variant(guiRichTextRml(bbcode, nullptr, gui::fontFamilyLoaded));
                    });
            }
        for (const std::string& key : names.variables)
            bind(context, model, key);
        for (const auto& [key, unused] : values_[model])
            bind(context, model, key);
    }
}

void GuiModels::set(std::span<Rml::Context* const> contexts, const std::string& model, const std::string& key, GuiValue value)
{
    values_[model][key] = std::move(value);
    const auto windows = windows_.find(model);
    if (windows != windows_.end())
        for (auto& [name, window] : windows->second)
            if (window.source == &values_[model][key])
                ++window.version;
    for (Rml::Context* context : contexts) {
        if (!context || !hasModel(*context, model))
            continue;
        bind(*context, model, key);
        Rml::DataModelHandle handle = context->GetDataModel(model).GetModelHandle();
        handle.DirtyVariable(key);
        if (windows != windows_.end())
            for (const auto& [name, window] : windows->second)
                if (window.source == &values_[model][key])
                    handle.DirtyVariable(window.name); // a window of another context: no such variable, ignored
    }
}

std::optional<GuiValue> GuiModels::get(const std::string& model, const std::string& key) const
{
    const auto found = values_.find(model);
    if (found == values_.end())
        return std::nullopt;
    const auto value = found->second.find(key);
    return value == found->second.end() ? std::nullopt : std::optional(value->second);
}

void GuiModels::clear(std::span<Rml::Context* const> contexts)
{
    for (Rml::Context* context : contexts)
        if (context)
            for (const auto& [model, unused] : values_)
                context->RemoveDataModel(model);
    values_.clear();
    windows_.clear();
    held_.clear();
}

GuiWindow* GuiModels::window(Rml::Context& context, const std::string& model, const std::string& source)
{
    if (!hasModel(context, model))
        return nullptr;
    size_t count = 0;
    for (const auto& [name, windows] : windows_)
        count += windows.size();
    const std::string name = std::format("kastlist{}", count + 1); // ponytail: a name a document's own variable could take too
    GuiWindow& window = windows_[model][name];
    window.name = name;
    window.source = &values_[model][source];
    context.GetDataModel(model).BindCustomDataVariable(name, Rml::DataVariable(&windowDefinition(), &window));
    return &window;
}

void GuiModels::hold(const std::string& model, const std::string& key)
{
    if (!held_.contains({model, key}))
        held_[{model, key}] = std::exchange(values_[model][key], GuiValue::array());
}

void GuiModels::restore(std::span<Rml::Context* const> contexts)
{
    for (auto& [name, value] : held_) {
        values_[name.first][name.second] = std::move(value);
        for (Rml::Context* context : contexts)
            if (context && hasModel(*context, name.first))
                context->GetDataModel(name.first).GetModelHandle().DirtyVariable(name.second);
    }
    held_.clear();
}

void GuiModels::dirty(Rml::Context& context, const std::string& model, const GuiWindow& window)
{
    if (hasModel(context, model))
        context.GetDataModel(model).GetModelHandle().DirtyVariable(window.name);
}

void GuiModels::bind(Rml::Context& context, const std::string& model, const std::string& key)
{
    Rml::DataModelConstructor constructor = context.GetDataModel(model);
    if (Rml::Detail::DataModelConstructorAccessor::GetAllVariables(constructor).count(key) > 0)
        return;
    constructor.BindCustomDataVariable(key, Rml::DataVariable(&valueDefinition(), &values_[model][key]));
}

} // namespace kast
