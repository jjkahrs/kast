// The zone: owns the ECS registry and AOI grid, and runs the systems in a fixed order each tick.
// Talks to the outside only through Send (to a session) and Save (to the DB worker).
#pragma once
#include "systems.h"

#include <db/char_repo.h>

#include <entt/entity/registry.hpp>

namespace kast::world {

struct WorldRules {
    MoveRules move;
    ChatRules chat;
    float cellSize = 64, viewRadius = 128;
    proto::Vec3 spawn{1024, 0, 1024};
    size_t chatRateCount = 5;
    Millis chatRateWindow{10000};
    Millis saveInterval{60000};
};

class World {
public:
    World(WorldRules rules, Send send, Save save);

    // Places the character (at its saved position, or the spawn point if that is outside the zone) and sends
    // EnterWorldOk. Other players get Spawn on the next tick.
    entt::entity spawn(uint32_t sessionId, const db::CharRow& row, TimePoint now);
    // Saves the character, sends Despawn to everyone who could see it, removes it.
    void despawn(entt::entity e);
    void push_move(entt::entity e, const proto::Move& m);
    void push_say(entt::entity e, std::string text);

    void tick(TimePoint now);
    size_t population() const { return population_; }

private:
    WorldRules rules_;
    Send send_;
    Save save_;
    entt::registry reg_;
    AoiGrid grid_;
    size_t population_ = 0;
};

} // namespace kast::world
