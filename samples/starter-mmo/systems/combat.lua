-- Server system: tab-target PvE melee. The client asks with kast.world.send("combat.attack", {target = <npc entity>}),
-- which arrives here as "client.combat.attack" {entity = attacker, data}. data is untrusted: the attack is refused
-- (the attacker alone gets "combat.rejected" {reason}) unless the target is an NPC that is alive, on the attacker's map
-- and within ATTACK_RANGE, the attacker is alive and its ATTACK_COOLDOWN has passed. A hit takes ATTACK_DAMAGE Health.
--
-- A hit NPC enters "combat" (fsms/patrol.lua for the Guard, fsms/creature.lua for the Bat), walks to its attacker
-- (kast.map.path, or straight on a map without a navmesh) and hits it for its NPC_ATTACKS damage every interval while
-- within ATTACK_RANGE. It gives up ("returning") once it is LEASH_RANGE from its spawn point or its target is dead,
-- gone or on another map; back at its spawn point it heals to full and goes back to patrolling (Guard) or standing
-- (Bat). The first player to hit an NPC tags it (WoW classic): tagged[npc], forgotten when it gets home healed. At 0
-- Health an NPC dies: its machine goes "dead" (a corpse: no walking, no fighting, refused as a target) and "npc.died"
-- {npc, tagger} goes on the server bus; systems/inventory.lua rolls its loot and removes the corpse later, and its spawn
-- file brings it back respawnSec after that.
--
-- A character at 0 Health is dead for REVIVE_SECONDS: it cannot move (kast.setMovable) or attack, and NPCs drop it.
-- Then it revives at its map's spawn point (kast.map.spawn) with full Health. One that logged out dead dies again on
-- login and revives the same way.
--
-- Each hit dealt or taken publishes "combat.engaged" {entity} on the server bus for both sides (systems/regen.lua
-- holds off regeneration after it).
--
-- Bandwidth: events only when something happens, sent with kast.sendNearby through the player involved (an NPC that
-- dies is still there as a corpse): "combat.attack" {attacker, target, attackerName, targetName} and
-- "combat.hit" (the same plus damage) per hit, "combat.death" {entity, name}, "combat.revive" {entity, name}. Health
-- is replicated on its own, once per hit. A chasing or returning NPC costs one EntityMove a tick to each viewer, like a
-- patrolling one.
local ATTACK_DAMAGE = 10     -- a player's melee hit
local ATTACK_RANGE = 3       -- metres, on x/z, for players and NPCs
local ATTACK_COOLDOWN = 1.5  -- seconds between a player's hits
local NPC_ATTACKS = {        -- by NPC definition name (npcs/)
    Guard = {damage = 5, interval = 2},
    Bat = {damage = 2, interval = 1.5},
}
local CHASE_SPEED = 5        -- m/s, chasing and returning
local REPATH_SECONDS = 0.5   -- how often a chasing NPC plans its route to the target again
local LEASH_RANGE = 30       -- metres from its spawn point, on x/z
local REVIVE_SECONDS = 5
local RESUME_STATE = {patrol = "walking", creature = "idle"} -- by machine: the state after returning

local now = 0           -- seconds since start, summed from dt
local readyAt = {}      -- player -> time its next attack is allowed
local deadUntil = {}    -- player -> time it revives
local fights = {}       -- npc -> {target = player, swingAt = time, home = {x, y, z}, route = corners, leg, repathAt}
local tagged = {}       -- npc -> the player who hit it first

local function exists(e, component)
    local ok, has = pcall(kast.has, e, component) -- raises for ids that are not in the world
    return ok and has
end

local function name(e)
    if exists(e, "Character") then return kast.get(e, "Character").name end
    return kast.get(e, "Npc").name
end

local function flatDistance(a, b)
    return math.sqrt((a.x - b.x) ^ 2 + (a.z - b.z) ^ 2)
end

local function alive(e)
    return deadUntil[e] == nil and kast.get(e, "Health").current > 0
end

local function engaged(a, b)
    kast.publish("combat.engaged", {entity = a})
    kast.publish("combat.engaged", {entity = b})
end

local function killPlayer(e)
    deadUntil[e] = now + REVIVE_SECONDS
    kast.setMovable(e, false)
    kast.sendNearby(e, "combat.death", {entity = e, name = name(e)})
end

local function revive(e)
    deadUntil[e] = nil
    local health = kast.get(e, "Health")
    kast.set(e, "Health", {current = health.max})
    local map = kast.get(e, "Transform").map
    local spawn = kast.map.spawn(map)
    kast.transfer(e, map, spawn.position, spawn.heading)
    kast.setMovable(e, true)
    kast.sendNearby(e, "combat.revive", {entity = e, name = name(e)})
end

-- Takes damage off target's Health; player is the one who sends the events (the attacker or the victim).
local function hit(attacker, target, damage, player)
    local health = kast.get(target, "Health")
    local current = math.max(0, health.current - damage)
    kast.set(target, "Health", {current = current})
    local names = {attacker = attacker, target = target, attackerName = name(attacker), targetName = name(target)}
    kast.sendNearby(player, "combat.attack", names)
    names.damage = damage
    kast.sendNearby(player, "combat.hit", names)
    engaged(attacker, target)
    if current > 0 then return end
    if exists(target, "Character") then
        killPlayer(target)
    else
        fights[target] = nil
        if exists(target, "StateMachine") then kast.fsm.change(target, "dead") end
        kast.sendNearby(player, "combat.death", {entity = target, name = name(target)})
        kast.publish("npc.died", {npc = target, tagger = tagged[target]})
        tagged[target] = nil
    end
end

local function home(npc)
    local tf = kast.get(npc, "Transform")
    if not exists(npc, "Spawn") then return tf.position end -- kast.create NPCs: where the fight started
    local pos = kast.get(npc, "Spawn").pos -- spawn files may leave y 0: kast.map.path wants it on the ground
    return {x = pos.x, y = kast.map.ground(tf.map, pos.x, pos.z) or pos.y, z = pos.z}
end

local function startFight(npc, player)
    if fights[npc] and kast.fsm.state(npc) == "combat" then return end -- keeps its first target
    local fight = fights[npc] or {home = home(npc)}
    fight.target, fight.swingAt, fight.route, fight.repathAt = player, now, nil, now
    fights[npc] = fight
    kast.fsm.change(npc, "combat")
end

local function reject(e, reason)
    kast.sendTo(e, "combat.rejected", {reason = reason})
end

local function onAttack(event)
    local attacker = event.entity
    local target = type(event.data) == "table" and event.data.target or nil
    if not alive(attacker) then return reject(attacker, "dead") end
    if type(target) ~= "number" or not exists(target, "Npc") or not exists(target, "Health") then
        return reject(attacker, "target")
    end
    if not alive(target) then return reject(attacker, "target_dead") end
    if (readyAt[attacker] or 0) > now then return reject(attacker, "cooldown") end
    local from, to = kast.get(attacker, "Transform"), kast.get(target, "Transform")
    if from.map ~= to.map or flatDistance(from.position, to.position) > ATTACK_RANGE then
        return reject(attacker, "range")
    end
    readyAt[attacker] = now + ATTACK_COOLDOWN
    tagged[target] = tagged[target] or attacker
    hit(attacker, target, ATTACK_DAMAGE, attacker)
    if alive(target) and exists(target, "StateMachine") then startFight(target, attacker) end
end

-- Moves npc toward goal at CHASE_SPEED along fight.route (planned again when stale); true once within reach.
local function moveToward(npc, fight, goal, reach, dt)
    local tf = kast.get(npc, "Transform")
    local pos = tf.position
    if flatDistance(pos, goal) <= reach then return true end
    if not fight.route or now >= fight.repathAt then
        fight.route = kast.map.path(tf.map, pos, goal) or {goal}
        fight.leg, fight.repathAt = 1, now + REPATH_SECONDS
    end
    local corner = fight.route[fight.leg] or goal
    local dx, dz = corner.x - pos.x, corner.z - pos.z
    local dist = math.sqrt(dx * dx + dz * dz)
    local step = CHASE_SPEED * dt
    if dist <= step then
        kast.set(npc, "Transform", {position = {x = corner.x, y = pos.y, z = corner.z}})
        fight.leg = fight.leg + 1
        if fight.leg > #fight.route then fight.route = nil end
    else
        kast.set(npc, "Transform", {
            position = {x = pos.x + dx / dist * step, y = pos.y, z = pos.z + dz / dist * step},
            heading = math.atan2(dx, dz),
        })
    end
    return false
end

local function targetLost(npc, target)
    if not exists(target, "Character") or not alive(target) then return true end
    return kast.get(target, "Transform").map ~= kast.get(npc, "Transform").map
end

local function fight(npc, f, dt)
    local pos = kast.get(npc, "Transform").position
    if targetLost(npc, f.target) or flatDistance(pos, f.home) > LEASH_RANGE then
        f.route = nil
        kast.fsm.change(npc, "returning")
        return
    end
    local goal = kast.get(f.target, "Transform").position
    if moveToward(npc, f, goal, ATTACK_RANGE * 0.8, dt) or flatDistance(pos, goal) <= ATTACK_RANGE then
        local dx, dz = goal.x - pos.x, goal.z - pos.z
        if dx ~= 0 or dz ~= 0 then kast.set(npc, "Transform", {heading = math.atan2(dx, dz)}) end
        if now >= f.swingAt then
            local attack = NPC_ATTACKS[kast.get(npc, "Npc").npc]
            if attack then
                f.swingAt = now + attack.interval
                hit(npc, f.target, attack.damage, f.target)
            end
        end
    end
end

local function goHome(npc, f, dt)
    if not moveToward(npc, f, f.home, 0.1, dt) then return end
    local health = kast.get(npc, "Health")
    kast.set(npc, "Health", {current = health.max})
    if exists(npc, "Spawn") then kast.set(npc, "Transform", {heading = kast.get(npc, "Spawn").heading}) end
    fights[npc], tagged[npc] = nil, nil
    kast.fsm.change(npc, RESUME_STATE[kast.get(npc, "StateMachine").machine] or "idle")
end

return {
    init = function()
        kast.subscribe("client.combat.attack", onAttack)
    end,

    onEnter = function(e)
        if kast.has(e, "Health") and kast.get(e, "Health").current <= 0 then killPlayer(e) end -- logged out dead
    end,

    onLeave = function(e)
        readyAt[e], deadUntil[e] = nil, nil
    end,

    update = function(dt)
        now = now + dt
        for npc in pairs(tagged) do
            if not exists(npc, "Npc") then tagged[npc] = nil end
        end
        for player, at in pairs(deadUntil) do
            if now >= at and exists(player, "Character") then revive(player) end
        end
        for npc, f in pairs(fights) do
            if not exists(npc, "Npc") then
                fights[npc] = nil
            elseif kast.fsm.state(npc) == "combat" then
                fight(npc, f, dt)
            elseif kast.fsm.state(npc) == "returning" then
                goHome(npc, f, dt)
            end
        end
    end,
}
