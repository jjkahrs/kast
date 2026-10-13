#pragma once

#include "event_bus.h"

#include <RmlUi/Core/EventListener.h>

#include <string>
#include <string_view>

// The one link from game GUIs to game logic: clicking a <button data-event="name"> publishes "name" on the Play
// session's event bus with payload {"id": <button id or "">, "document": <project-relative .rml path>} from publisher
// "UI". A missing or empty data-event publishes nothing.
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

} // namespace kast
