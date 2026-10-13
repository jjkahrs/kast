#include "gui_events.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>

namespace kast {

bool publishButtonClick(EventBus& bus, std::string_view event, std::string_view id, std::string_view document)
{
    if (event.empty())
        return false;
    bus.publish(std::string(event), {{"id", std::string(id)}, {"document", std::string(document)}}, "UI");
    return true;
}

void ButtonClickListener::ProcessEvent(Rml::Event& event)
{
    Rml::Element* target = event.GetTargetElement();
    Rml::Element* button = target && target->GetTagName() == "button" ? target : target ? target->Closest("button") : nullptr;
    if (button)
        publishButtonClick(bus_, button->GetAttribute<Rml::String>("data-event", ""), button->GetId(), document_);
}

} // namespace kast
