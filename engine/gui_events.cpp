#include "gui_events.h"

#include "gui.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
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

namespace kast {
namespace {

// The element carrying data-drag: the dragged one or an ancestor.
Rml::Element* dragSource(Rml::Element* element)
{
    for (; element; element = element->GetParentNode())
        if (element->HasAttribute("data-drag"))
            return element;
    return nullptr;
}

} // namespace

std::string guiElementId(const Rml::Element* element)
{
    for (; element; element = element->GetParentNode()) {
        if (element == element->GetOwnerDocument()) // RmlUi's document element is the <body>
            return "";
        if (!element->GetId().empty())
            return element->GetId();
    }
    return "";
}

void DragListener::ProcessEvent(Rml::Event& event)
{
    const bool start = event.GetId() == Rml::EventId::Dragstart;
    if (!start && event.GetId() != Rml::EventId::Dragend)
        return;
    Rml::Element* dragged = event.GetTargetElement();
    Rml::Element* source = dragSource(dragged);
    if (!source)
        return;
    const std::string name = source->GetAttribute<Rml::String>("data-drag", "");
    if (name.empty())
        return;
    const Rml::ElementDocument* owner = dragged->GetOwnerDocument();
    gui::setDragDocument(start && owner ? owner->GetSourceURL() : "");
    std::string target, targetDocument;
    if (!start) {
        Rml::Context* context = dragged->GetContext();
        const Rml::Element* hover = context ? context->GetHoverElement() : nullptr;
        target = guiElementId(hover);
        if (!target.empty())
            targetDocument = documentPath_(*hover->GetOwnerDocument());
    }
    bus_.publish(name + (start ? ".start" : ".drop"),
                 {{"id", guiElementId(dragged)},
                  {"document", document_},
                  {"target", target},
                  {"targetDocument", targetDocument},
                  {"shift", event.GetParameter<bool>("shift_key", false)},
                  {"ctrl", event.GetParameter<bool>("ctrl_key", false)},
                  {"alt", event.GetParameter<bool>("alt_key", false)}},
                 "UI");
}

} // namespace kast
