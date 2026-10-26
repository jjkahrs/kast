#include "gui_widgets.h"

#include "gui.h"
#include "gui_richtext.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <RmlUi/Core/Elements/ElementProgress.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>

namespace kast {

const char* const kGuiWidgetStyle = R"(
/* Toggle: a switch, its knob a radial gradient; on, it turns #ffaa00 and the knob moves right. */
input.toggle {
    display: inline-block;
    width: 36px;
    height: 18px;
    margin-right: 6px;
    vertical-align: middle;
    border-radius: 9px;
    background-color: #454545;
    decorator: radial-gradient(circle 7px at 9px 50%, #ffffff 6px, #ffffff00 7px);
}
input.toggle:hover {
    background-color: #818181;
}
input.toggle:checked {
    background-color: #ffaa00;
    decorator: radial-gradient(circle 7px at 27px 50%, #ffffff 6px, #ffffff00 7px);
}
input.toggle:checked:hover {
    background-color: #ffcc00;
}

/* Slider: a thin track filled up to the bar. */
input[type="range"] {
    display: inline-block;
    width: 160px;
    height: 18px;
    vertical-align: middle;
}
input[type="range"] slidertrack {
    margin-top: 7px;
    height: 4px;
    border-radius: 2px;
    background-color: #454545;
}
input[type="range"] sliderprogress {
    height: 4px;
    border-radius: 2px;
    background-color: #ff9900;
}
input[type="range"] sliderbar {
    width: 12px;
    height: 18px;
    border-radius: 6px;
    background-color: #ffaa00;
}
input[type="range"] sliderbar:hover {
    background-color: #ffcc00;
}
input[type="range"] sliderbar:active {
    background-color: #ff9900;
}

/* Dropdown: the value on a text field, an arrow button, the open list on a card. */
select {
    display: inline-block;
    width: 160px;
    height: 24px;
    vertical-align: middle;
    border-radius: 3px;
    background-color: #454545;
    color: #ffffff;
}
select selectvalue {
    height: 24px;
    margin-right: 20px;
    padding: 0 6px;
    line-height: 24px;
}
select selectarrow {
    width: 20px;
    height: 24px;
    border-radius: 0 3px 3px 0;
    background-color: #ffaa00;
}
select:hover selectarrow {
    background-color: #ffcc00;
}
select selectbox {
    margin-top: 2px;
    border: 1px #818181;
    background-color: #313131;
}
select selectbox option {
    padding: 4px 6px;
}
select selectbox option:hover {
    background-color: #818181;
}
select selectbox option:checked {
    color: #ffcc00;
}

/* Spinbox: - and + buttons around a centred number field. */
.spinbox {
    display: inline-block;
    height: 24px;
    vertical-align: middle;
    white-space: nowrap;
}
.spinbox button {
    display: inline-block;
    vertical-align: top;
    width: 24px;
    height: 24px;
    padding: 0;
    line-height: 24px;
    text-align: center;
    border-radius: 3px;
    background-color: #ffaa00;
    color: #000000;
}
.spinbox button:hover {
    background-color: #ffcc00;
}
.spinbox button:active {
    background-color: #ff9900;
}
.spinbox input {
    display: inline-block;
    vertical-align: top;
    width: 56px;
    height: 24px;
    margin: 0 2px;
    padding: 0 4px;
    line-height: 24px;
    text-align: center;
    border-radius: 3px;
    background-color: #454545;
    color: #ffffff;
}
.spinbox input:focus {
    background-color: #818181;
}

/* Progress: filled left to right. */
progress {
    display: inline-block;
    width: 160px;
    height: 12px;
    vertical-align: middle;
    border-radius: 6px;
    background-color: #454545;
}
progress fill {
    border-radius: 6px;
    background-color: #ffaa00;
}

/* List Box and Virtual List (gui_lists.h): rows on a card, the selected one #ffaa00, a slim scrollbar; the list takes the
   focus (its rows don't), outlined while it has it. Scoped to the lists: other scroll containers keep their look. */
.listbox, .virtuallist {
    display: block;
    width: 240px;
    height: 168px;
    overflow-y: auto;
    border: 1px #454545;
    border-radius: 3px;
    background-color: #313131;
    color: #ffffff;
    text-align: left;
}
.listbox:focus, .virtuallist:focus {
    border-color: #ffcc00;
}
.listbox .row, .virtuallist .row {
    display: block;
    height: 24px;
    padding: 0 8px;
    line-height: 24px;
    white-space: nowrap;
    focus: none;
}
.listbox .row:hover, .virtuallist .row:hover {
    background-color: #454545;
}
.listbox .row.selected, .virtuallist .row.selected {
    background-color: #ffaa00;
    color: #000000;
}
.listbox scrollbarvertical, .virtuallist scrollbarvertical {
    width: 10px;
}
.listbox scrollbarvertical slidertrack, .virtuallist scrollbarvertical slidertrack {
    background-color: #000000;
}
.listbox scrollbarvertical sliderbar, .virtuallist scrollbarvertical sliderbar {
    min-height: 20px;
    border-radius: 5px;
    background-color: #818181;
}
.listbox scrollbarvertical sliderbar:hover, .virtuallist scrollbarvertical sliderbar:hover {
    background-color: #ffcc00;
}
.listbox scrollbarvertical sliderbar:active, .virtuallist scrollbarvertical sliderbar:active {
    background-color: #ff9900;
}
/* Rich text links ([link=...], gui_richtext.h). */
.link {
    color: #ffaa00;
}
.link:hover {
    color: #ffcc00;
}
.link:active {
    color: #ff9900;
}
)";

namespace {

constexpr int kMaxDecimals = 6;
constexpr const char* kValidAttribute = "data-kast-valid"; // a spinbox's last valid text (runtime only, never saved)

std::optional<double> parseNumber(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    if (text.starts_with('+'))
        text.remove_prefix(1);
    double value = 0.0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc() || end != text.data() + text.size() || !std::isfinite(value))
        return std::nullopt;
    return value;
}

int decimals(double step)
{
    for (int d = 0; d < kMaxDecimals; ++d) {
        const double scaled = step * std::pow(10.0, d);
        if (std::abs(scaled - std::round(scaled)) < 1e-9 * std::max(1.0, scaled))
            return d;
    }
    return kMaxDecimals;
}

double snap(const GuiSpinbox& spinbox, double value)
{
    const double step = spinbox.step > 0.0 && std::isfinite(spinbox.step) ? spinbox.step : 1.0;
    const double base = std::isfinite(spinbox.min) ? spinbox.min : 0.0;
    const double low = std::isfinite(spinbox.min) ? 0.0 : -std::numeric_limits<double>::infinity();
    double high = std::isfinite(spinbox.max) ? std::floor((spinbox.max - base) / step + 1e-9)
                                             : std::numeric_limits<double>::infinity();
    high = std::max(high, low); // max below min: min wins
    const double k = std::clamp(std::round((value - base) / step), low, high);
    const double scale = std::pow(10.0, decimals(step));
    return std::round((base + k * step) * scale) / scale; // no float noise past step's decimals
}

// A float RmlUi holds, to 6 decimals (0.6f reads 0.6, not 0.60000002).
double tidy(double value) { return std::round(value * 1e6) / 1e6; }

bool isSpinbox(const Rml::Element* element) { return element && element->GetTagName() == "div" && element->IsClassSet("spinbox"); }

Rml::ElementFormControl* spinboxInput(Rml::Element& spinbox)
{
    for (int i = 0; i < spinbox.GetNumChildren(); ++i)
        if (spinbox.GetChild(i)->GetTagName() == "input")
            return dynamic_cast<Rml::ElementFormControl*>(spinbox.GetChild(i));
    return nullptr;
}

GuiSpinbox spinboxLimits(const Rml::Element& spinbox)
{
    const auto number = [&](const char* name) { return parseNumber(spinbox.GetAttribute<Rml::String>(name, "")); };
    GuiSpinbox limits;
    limits.min = number("data-min").value_or(limits.min);
    limits.max = number("data-max").value_or(limits.max);
    limits.step = number("data-step").value_or(limits.step);
    return limits;
}

// The value a spinbox shows (its text, snapped), else its last valid one; nullopt if it has neither.
std::optional<double> spinboxShown(Rml::Element& spinbox)
{
    Rml::ElementFormControl* input = spinboxInput(spinbox);
    const GuiSpinbox limits = spinboxLimits(spinbox);
    if (const std::optional<double> value = input ? guiSpinboxValue(limits, input->GetValue()) : std::nullopt)
        return value;
    return guiSpinboxValue(limits, spinbox.GetAttribute<Rml::String>(kValidAttribute, ""));
}

// Its last valid value (as of its input's focus or its last commit), else what it shows.
std::optional<double> spinboxValid(Rml::Element& spinbox)
{
    if (const auto valid = guiSpinboxValue(spinboxLimits(spinbox), spinbox.GetAttribute<Rml::String>(kValidAttribute, "")))
        return valid;
    return spinboxShown(spinbox);
}

std::string inputType(const Rml::Element& element) { return element.GetAttribute<Rml::String>("type", "text"); }

// Shows text in the spinbox and remembers it as its last valid value. RmlUi's SetValue on a text field sends no change
// event, so this sends one, as typing does: a data-value binding on the input writes its model.
void setSpinboxText(Rml::Element& spinbox, Rml::ElementFormControl& input, const std::string& text)
{
    if (input.GetValue() != text) {
        input.SetValue(text);
        Rml::Dictionary parameters;
        parameters["value"] = Rml::Variant(text);
        input.DispatchEvent(Rml::EventId::Change, parameters);
    }
    spinbox.SetAttribute(kValidAttribute, text);
}

} // namespace

std::optional<double> guiSpinboxValue(const GuiSpinbox& spinbox, std::string_view text)
{
    const std::optional<double> value = parseNumber(text);
    return value ? std::optional(snap(spinbox, *value)) : std::nullopt;
}

double guiSpinboxStep(const GuiSpinbox& spinbox, double value, int steps)
{
    const double step = spinbox.step > 0.0 && std::isfinite(spinbox.step) ? spinbox.step : 1.0;
    return snap(spinbox, value + steps * step);
}

std::string guiSpinboxText(const GuiSpinbox& spinbox, double value)
{
    const int places = decimals(spinbox.step > 0.0 && std::isfinite(spinbox.step) ? spinbox.step : 1.0);
    std::string text = std::format("{:.{}f}", value, places);
    return text == std::format("-{:.{}f}", 0.0, places) ? text.substr(1) : text; // never "-0"
}

std::optional<GuiValue> guiWidgetValue(Rml::Element& element)
{
    if (isSpinbox(&element)) {
        const std::optional<double> value = spinboxShown(element);
        return value ? std::optional<GuiValue>(*value) : std::nullopt;
    }
    if (auto* progress = dynamic_cast<Rml::ElementProgress*>(&element))
        return tidy(progress->GetValue());
    auto* control = dynamic_cast<Rml::ElementFormControl*>(&element);
    if (!control)
        return std::nullopt;
    if (element.GetTagName() == "input" && inputType(element) == "checkbox")
        return element.HasAttribute("checked");
    if (element.GetTagName() == "input" && inputType(element) == "range")
        return tidy(parseNumber(control->GetValue()).value_or(0.0));
    return control->GetValue(); // a text field's text, a dropdown's value
}

bool setGuiWidgetValue(Rml::Element& element, const GuiValue& value, std::string& error)
{
    const auto fail = [&](std::string why) {
        error = std::move(why);
        return false;
    };
    if (isSpinbox(&element)) {
        Rml::ElementFormControl* input = spinboxInput(element);
        if (!input)
            return fail("the spinbox has no <input>");
        if (!value.is_number())
            return fail("a spinbox takes a number");
        const GuiSpinbox limits = spinboxLimits(element);
        setSpinboxText(element, *input, guiSpinboxText(limits, guiSpinboxStep(limits, value.get<double>(), 0)));
        return true;
    }
    if (auto* progress = dynamic_cast<Rml::ElementProgress*>(&element)) {
        if (!value.is_number())
            return fail("a progress bar takes a number");
        progress->SetAttribute("value", std::format("{}", std::clamp(value.get<double>(), 0.0, double(progress->GetMax()))));
        return true;
    }
    auto* control = dynamic_cast<Rml::ElementFormControl*>(&element);
    if (!control)
        return fail("it is not a form widget or text field");
    const std::string tag = element.GetTagName();
    const std::string type = tag == "input" ? inputType(element) : "";
    if (type == "checkbox") {
        if (!value.is_boolean())
            return fail("a toggle takes a boolean");
        if (value.get<bool>())
            element.SetAttribute("checked", "");
        else
            element.RemoveAttribute("checked");
        return true;
    }
    if (type == "range") {
        if (!value.is_number())
            return fail("a slider takes a number");
        control->SetValue(std::format("{}", value.get<double>())); // RmlUi snaps it to step and clamps it
        return true;
    }
    if (tag == "select") {
        auto& select = static_cast<Rml::ElementFormControlSelect&>(*control);
        if (!value.is_string())
            return fail("a dropdown takes a string");
        for (int i = 0; i < select.GetNumOptions(); ++i)
            if (select.GetOption(i)->GetAttribute<Rml::String>("value", "") == value.get_ref<const std::string&>()) {
                select.SetSelection(i);
                return true;
            }
        return fail("the dropdown has no option with that value");
    }
    if (tag == "textarea" || type == "text" || type == "password") {
        if (!value.is_string())
            return fail("a text field takes a string");
        control->SetValue(value.get<std::string>());
        return true;
    }
    return fail("it is not a form widget or text field");
}

void applyGuiWrite(Rml::Element& element, const GuiWrite& write)
{
    if (write.text || write.rich) {
        if (auto* control = dynamic_cast<Rml::ElementFormControl*>(&element))
            control->SetValue(write.text ? *write.text : *write.rich);
        else
            element.SetInnerRML(write.text ? guiTextRml(*write.text)
                                           : guiRichTextRml(*write.rich, nullptr, gui::fontFamilyLoaded));
    }
    for (const auto& [name, value] : write.attributes)
        element.SetAttribute(name, value);
    if (write.visible) {
        if (*write.visible)
            element.RemoveProperty("display"); // back to the style sheet's
        else
            element.SetProperty("display", "none");
    }
}

void WidgetListener::ProcessEvent(Rml::Event& event)
{
    Rml::Element* target = event.GetTargetElement();
    if (!target)
        return;
    Rml::Element* parent = target->GetParentNode();
    const bool spinboxInputTarget = isSpinbox(parent) && target->GetTagName() == "input";
    switch (event.GetId()) {
    case Rml::EventId::Change: {
        const std::string type = target->GetTagName() == "input" ? inputType(*target) : "";
        if (type == "checkbox" || type == "range" || target->GetTagName() == "select")
            if (const std::optional<GuiValue> value = guiWidgetValue(*target))
                publish(*target, *value);
        break;
    }
    case Rml::EventId::Click: {
        Rml::Element* button = target->GetTagName() == "button" ? target : target->Closest("button");
        Rml::Element* spinbox = button ? button->GetParentNode() : nullptr;
        Rml::ElementFormControl* input = isSpinbox(spinbox) ? spinboxInput(*spinbox) : nullptr;
        if (!input)
            break;
        bool beforeInput = false; // the - button comes before the input, the + button after it
        for (Rml::Element* next = button->GetNextSibling(); next && !beforeInput; next = next->GetNextSibling())
            beforeInput = next == input;
        const GuiSpinbox limits = spinboxLimits(*spinbox);
        const double shown = spinboxShown(*spinbox).value_or(std::isfinite(limits.min) ? limits.min : 0.0);
        commit(*spinbox, guiSpinboxStep(limits, shown, beforeInput ? -1 : 1));
        break;
    }
    case Rml::EventId::Keydown: {
        if (!spinboxInputTarget)
            break;
        const auto key = static_cast<Rml::Input::KeyIdentifier>(event.GetParameter<int>("key_identifier", 0));
        const GuiSpinbox limits = spinboxLimits(*parent);
        if (key == Rml::Input::KI_UP || key == Rml::Input::KI_DOWN) {
            const double shown = spinboxShown(*parent).value_or(std::isfinite(limits.min) ? limits.min : 0.0);
            commit(*parent, guiSpinboxStep(limits, shown, key == Rml::Input::KI_UP ? 1 : -1));
            event.StopPropagation();
        } else if (key == Rml::Input::KI_RETURN || key == Rml::Input::KI_NUMPADENTER) {
            if (const std::optional<double> typed = guiSpinboxValue(limits, static_cast<Rml::ElementFormControl*>(target)->GetValue()))
                commit(*parent, *typed);
            else if (const std::optional<double> valid = spinboxValid(*parent))
                commit(*parent, *valid);
        }
        break;
    }
    case Rml::EventId::Focus:
        if (spinboxInputTarget)
            if (const std::optional<double> shown = spinboxShown(*parent))
                parent->SetAttribute(kValidAttribute, guiSpinboxText(spinboxLimits(*parent), *shown));
        break;
    case Rml::EventId::Blur:
        if (spinboxInputTarget) {
            const GuiSpinbox limits = spinboxLimits(*parent);
            if (const std::optional<double> typed = guiSpinboxValue(limits, static_cast<Rml::ElementFormControl*>(target)->GetValue()))
                commit(*parent, *typed);
            else if (const std::optional<double> valid = spinboxValid(*parent))
                commit(*parent, *valid); // not a number: back to the last valid value
        }
        break;
    default:
        break;
    }
}

void WidgetListener::commit(Rml::Element& spinbox, double value)
{
    const std::optional<double> before = spinboxValid(spinbox);
    if (Rml::ElementFormControl* input = spinboxInput(spinbox))
        setSpinboxText(spinbox, *input, guiSpinboxText(spinboxLimits(spinbox), value));
    if (before != value)
        publish(spinbox, value);
}

void WidgetListener::publish(Rml::Element& widget, GuiValue value)
{
    const std::string name = widget.GetAttribute<Rml::String>("data-event", "");
    if (name.empty() || quiet_ || gui::updating())
        return;
    bus_.publish(name, {{"id", widget.GetId()}, {"document", document_}, {"value", std::move(value)}}, "UI");
}

} // namespace kast
