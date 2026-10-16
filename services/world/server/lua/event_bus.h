// Event bus: named events with plain-data JSON payloads, queued on publish and delivered by drain(). Copied from
// engine/event_bus.h (same behaviour) without the editor's history. One per lua::Runtime; Lua systems use it through
// kast.publish/subscribe/unsubscribe (README "Lua components and systems").
#pragma once
#include <entt/signal/dispatcher.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <string>

namespace kast {

class EventBus {
public:
    using Callback = std::function<void(const nlohmann::json& payload)>;

    // Unsubscribes when destroyed. Must not outlive its bus. Unsubscribing twice does nothing.
    class Subscription {
    public:
        Subscription() = default;
        Subscription(Subscription&& other) noexcept;
        Subscription& operator=(Subscription&& other) noexcept;
        ~Subscription() { unsubscribe(); }
        void unsubscribe();
        uint64_t id() const { return id_; }

    private:
        friend class EventBus;
        Subscription(EventBus* bus, uint64_t id) : bus_(bus), id_(id) {}
        EventBus* bus_ = nullptr;
        uint64_t id_ = 0;
    };

    EventBus();
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    // Queues an event for the next drain().
    void publish(std::string name, nlohmann::json payload, std::string publisher = "C++");
    // Subscribers of an event are called in subscription order. One added during a drain gets events from the next
    // one on; one removed during a drain gets nothing more, even from the current event.
    [[nodiscard]] Subscription subscribe(std::string name, Callback callback);
    // Delivers the events queued before this call, in publish order; events published meanwhile wait for the next
    // drain. A subscriber that throws is logged and the rest still run.
    void drain();
    void dropQueued();

private:
    struct Event {
        std::string name;
        nlohmann::json payload;
        std::string publisher;
    };
    struct Subscriber {
        uint64_t id;
        std::string name;
        Callback callback;
        bool active;
    };

    void deliver(Event& event);
    void unsubscribe(uint64_t id);

    entt::dispatcher dispatcher_;
    // A deque: a callback may subscribe while it runs, and push_back must not move the running callback. Removed
    // entries are erased outside drains only.
    std::deque<Subscriber> subscribers_;
    uint64_t nextId_ = 1; // never reused, so a stale handle can't remove a newer subscription
    bool draining_ = false;
};

} // namespace kast
