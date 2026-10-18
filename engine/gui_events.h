#pragma once

#include "event_bus.h"

#include <RmlUi/Core/EventListener.h>

#include <functional>
#include <string>
#include <string_view>

// The links from game GUIs to game logic, all published on the Play session's event bus from publisher "UI":
// - clicking a <button data-event="name"> publishes "name" with payload {"id": <button id or "">, "document":
//   <project-relative .rml path>}. A missing or empty data-event publishes nothing.
// - dragging an element whose RCSS makes it draggable (drag: clone, drag-drop, drag) and that has data-drag="name"
//   (on it or an ancestor) publishes "name.start" when the drag begins and "name.drop" when the button is released,
//   both with {"id", "document", "target", "targetDocument", "shift", "ctrl", "alt"}. target is the id of the element
//   under the cursor (or its nearest ancestor with one), "" over no element or a document's own <body>; targetDocument
//   is that element's .rml ("" with no target). The modifier flags are the keys held at that moment.
namespace kast {

// Publishes the click event, or nothing if event is empty. True if it published.
bool publishButtonClick(EventBus& bus, std::string_view event, std::string_view id, std::string_view document);

// Listens for clicks on one document; the bus must outlive it.
class ButtonClickListener final : public Rml::EventListener {
public:
    ButtonClickListener(EventBus& bus, std::string document) : bus_(bus), document_(std::move(document)) {}
    void ProcessEvent(Rml::Event& event) override;

private:
    EventBus& bus_;
    std::string document_;
};

// Listens for drags (dragstart, dragend) on one document; the bus must outlive it. documentPath names the .rml of any
// loaded document (target documents may be other ones), "" if unknown.
class DragListener final : public Rml::EventListener {
public:
    using DocumentPath = std::function<std::string(const Rml::Element& document)>;
    DragListener(EventBus& bus, std::string document, DocumentPath documentPath)
        : bus_(bus), document_(std::move(document)), documentPath_(std::move(documentPath)) {}
    void ProcessEvent(Rml::Event& event) override;

private:
    EventBus& bus_;
    std::string document_;
    DocumentPath documentPath_;
};

// The id of element or its nearest ancestor that has one, stopping at its document (RmlUi's document is the <body>): ""
// if none. What kast.gui.hovered() and a drop's target report.
std::string guiElementId(const Rml::Element* element);

} // namespace kast
