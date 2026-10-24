#pragma once

#include "event_bus.h"
#include "gui_access.h"

#include <RmlUi/Core/EventListener.h>

#include <limits>
#include <optional>
#include <string>
#include <string_view>

// Game GUI form widgets (docs/engine/game-gui.html, "Form widgets"), RmlUi's own controls plus kast's spinbox:
// - Toggle: <input type="checkbox" class="toggle"/>, optionally inside a <label> holding its text. Value: a bool.
// - Slider: <input type="range" min max step value/>. Value: a number.
// - Dropdown: <select> of <option value="...">label</option>. Value: the selected option's value (a string).
// - Spinbox: <div class="spinbox" data-min data-max data-step> holding a <button> (-), a text <input> (its value) and a
//   <button> (+). Value: a number. Missing data-min / data-max: unbounded; missing or bad data-step: 1.
// - Progress: <progress value max/>. Value: a number in 0..max.
// Every one, the lists (gui_lists.h) and rich text links (gui_richtext.h) are styled by kGuiWidgetStyle unless the
// document's own sheets say otherwise.
namespace kast {

// The engine's default widget style (Kast palette), put under every game GUI document's own sheets (gui::init,
// gui::addWidgetStyle). Selectors are as plain as they can be, so a project rule of the same specificity overrides it.
extern const char* const kGuiWidgetStyle;

struct GuiSpinbox {
    double min = -std::numeric_limits<double>::infinity();
    double max = std::numeric_limits<double>::infinity();
    double step = 1.0;
};
// text read as a spinbox value: a number (spaces and a leading + allowed) snapped to min + k * step (0 + k * step when
// min is unbounded) inside [min, max]; nullopt if text isn't a finite number.
std::optional<double> guiSpinboxValue(const GuiSpinbox& spinbox, std::string_view text);
// value moved by steps steps, snapped and clamped the same way.
double guiSpinboxStep(const GuiSpinbox& spinbox, double value, int steps);
// value as the spinbox shows it: with as many decimals as step has (step 0.25: "1.50"; step 1: "3"; at most 6).
std::string guiSpinboxText(const GuiSpinbox& spinbox, double value);

} // namespace kast

namespace Rml {
class Element;
}

namespace kast {

// The widget's value (see above) or a text field's text; nullopt for any other element.
std::optional<GuiValue> guiWidgetValue(Rml::Element& element);
// Sets the widget's value as the player could: a slider's and spinbox's number snapped and clamped, a progress clamped
// to 0..max; a toggle takes a bool, a dropdown the value of one of its options, a text field a string. False (and why
// in error, never naming the value) if element is no widget or value doesn't fit it.
bool setGuiWidgetValue(Rml::Element& element, const GuiValue& value, std::string& error);

// Listens to one document's form widgets; the bus must outlive it. Drives the spinboxes (-/+ clicks, Up / Down while its
// input has focus, typed text checked on blur or Enter: a number is snapped and clamped, anything else reverts to the
// last valid value). A value the player changes on a toggle, slider, dropdown or spinbox with data-event="name"
// publishes "name" with {"id": <the widget's id or "">, "document": <the .rml path>, "value": <its value>} from
// publisher "UI"; a slider publishes at every step of a drag. Changes made while quiet is true (kast.gui.setValue) or
// while the documents update (gui::updating(): data binding) publish nothing.
class WidgetListener final : public Rml::EventListener {
public:
    WidgetListener(EventBus& bus, std::string document, const bool& quiet)
        : bus_(bus), document_(std::move(document)), quiet_(quiet) {}
    void ProcessEvent(Rml::Event& event) override;

private:
    void publish(Rml::Element& widget, GuiValue value);
    void commit(Rml::Element& spinbox, double value);

    EventBus& bus_;
    std::string document_;
    const bool& quiet_;
};

} // namespace kast
