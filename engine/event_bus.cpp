#include "event_bus.h"

#include "log.h"

#include <algorithm>
#include <format>
#include <utility>

namespace kast {

EventBus::Subscription::Subscription(Subscription&& other) noexcept
    : bus_(std::exchange(other.bus_, nullptr)), id_(std::exchange(other.id_, 0))
{
}

EventBus::Subscription& EventBus::Subscription::operator=(Subscription&& other) noexcept
{
    if (this != &other) {
        unsubscribe();
        bus_ = std::exchange(other.bus_, nullptr);
        id_ = std::exchange(other.id_, 0);
    }
    return *this;
}

void EventBus::Subscription::unsubscribe()
{
    if (bus_)
        bus_->unsubscribe(id_);
    bus_ = nullptr;
}

EventBus::EventBus() { dispatcher_.sink<Event>().connect<&EventBus::deliver>(*this); }

void EventBus::publish(std::string name, nlohmann::json payload, std::string publisher)
{
    dispatcher_.enqueue(Event{std::move(name), std::move(payload), std::move(publisher)});
}

EventBus::Subscription EventBus::subscribe(std::string name, Callback callback)
{
    subscribers_.push_back({nextId_, std::move(name), std::move(callback), true});
    return Subscription(this, nextId_++);
}

void EventBus::unsubscribe(uint64_t id)
{
    const auto it = std::ranges::find(subscribers_, id, &Subscriber::id);
    if (it == subscribers_.end())
        return;
    if (draining_)
        it->active = false; // erased after the drain
    else
        subscribers_.erase(it);
}

void EventBus::drain()
{
    ++frame_;
    draining_ = true;
    dispatcher_.update<Event>(); // only the events queued before this call
    draining_ = false;
    std::erase_if(subscribers_, [](const Subscriber& s) { return !s.active; });
}

void EventBus::deliver(Event& event)
{
    history_.push_back({frame_, event.name, event.payload.dump(), event.publisher});
    if (history_.size() > kHistorySize)
        history_.pop_front();
    // Index loop over the count at the start: subscriptions added by a callback wait for the next event.
    const size_t count = subscribers_.size();
    for (size_t i = 0; i < count; ++i) {
        const Subscriber& subscriber = subscribers_[i];
        if (!subscriber.active || subscriber.name != event.name)
            continue;
        try {
            subscriber.callback(event.payload);
        } catch (const std::exception& e) { // nothing may escape to the main loop
            log::error(std::format("Event '{}': C++ subscriber failed: {}", event.name, e.what()));
        } catch (...) {
            log::error(std::format("Event '{}': C++ subscriber failed", event.name));
        }
    }
}

void EventBus::dropQueued() { dispatcher_.clear<Event>(); }

void EventBus::reset()
{
    dropQueued();
    subscribers_.clear();
    history_.clear();
    frame_ = 0;
}

} // namespace kast
