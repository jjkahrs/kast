-- Server system: tab-target PvE combat with class powers. Each class (components/class.lua, systems/classes.lua) has
-- the three POWERS below, on the client's keys 1-3 and action bar (scripts/powers.lua). On entering the world the
-- owner gets "class.powers" {class, gcd, powers = {{id, name, key, cooldown, effect}, ...}}, so its action bar shows
-- exactly what this file allows. All powers are instant, with cooldowns only.
--
-- The client asks with kast.world.send("combat.power", {power = <id>, target = <npc entity or nil>}), which arrives
-- here as "client.combat.power" {entity = caster, data}. data is untrusted: the power is refused (the caster alone gets
-- "combat.rejected" {reason}) and nothing changes unless, in this order: the caster is alive ("dead"); power is a power
-- id ("unknown_power") of the caster's class ("wrong_class"); for a damage power, target is an NPC ("target") that is
-- alive ("target_dead"); the power's own cooldown has passed ("cooldown") and so has the global cooldown, GCD seconds
-- that any power starts for all of the caster's powers ("gcd"); a damage power's target is on the caster's map within
-- its range on x/z ("range"), and for Backstab the caster stands in the 180 degree arc behind the target's heading
-- ("not_behind").
-- What a power does, by the fields of its definition:
--   damage   takes that much Health off the target, plus the caster's gear Damage; the first player to hit an NPC tags
--            it (WoW classic), and a hit NPC fights back (below).
--   root     every living NPC on the caster's map within radius m can't move for root seconds; it still hits a player
--            in reach. "npc.rooted" {npc, seconds} goes on the server bus for systems/npcs.lua's patrols.
--   heal     the caster gets that much Health, at most up to its max.
--   duration a buff on the caster for that long: taken (NPC damage times this, rounded down), absorb (soaks up that
--            much NPC damage, then breaks) or dodge (the chance an NPC hit misses; math.random, which a test seeds).
--            Buffs end early on death and are not saved.
-- Gear (systems/inventory.lua's "equipment.stats" {entity, armor, damage, stamina} on the server bus): Damage adds to
-- every damage power; each point of Armor takes 1 off every NPC hit (never below 1), before Shield Wall and Power
-- Shield. Stamina is systems/classes.lua's.
--
-- A hit NPC enters "combat" (fsms/patrol.lua for the Guard, fsms/creature.lua for the Bat), walks to its attacker
-- (kast.map.path, or straight on a map without a navmesh) and hits it for its NPC_ATTACKS damage every interval while
-- within ATTACK_RANGE. It gives up ("returning") once it is LEASH_RANGE from its spawn point or its target is dead,
-- gone or on another map; back at its spawn point it heals to full and goes back to patrolling (Guard) or standing
-- (Bat). tagged[npc] is forgotten when it gets home healed. At 0 Health an NPC dies: its machine goes "dead" (a corpse:
-- no walking, no fighting, refused as a target) and "npc.died" {npc, tagger} goes on the server bus;
-- systems/inventory.lua rolls its loot and removes the corpse later, and its spawn file brings it back respawnSec after.
--
-- A character at 0 Health is dead for REVIVE_SECONDS: it cannot move (kast.setMovable) or use powers, and NPCs drop it.
-- Then it revives at its map's spawn point (kast.map.spawn) with Health.max (its class's plus its gear's Stamina). One that logged out dead
-- dies again on login and revives the same way.
--
-- Each hit dealt or taken, and each dodge, publishes "combat.engaged" {entity} on the server bus for both sides
-- (systems/regen.lua holds off regeneration after it).
--
-- Bandwidth: events only when something happens, sent with kast.sendNearby through the player involved (an NPC that
-- dies is still there as a corpse): "combat.power" {caster, casterName, power, name, effect, target?, targetName?,
-- rooted?, amount?, duration?} per power used (clients draw its effect), "combat.hit" {attacker, target, attackerName,
-- targetName, damage, power? (its name), absorbed?, absorbedBy? (the shield's name)} per hit, "combat.dodge"
-- {attacker, target, attackerName, targetName}, "combat.fade" {entity, power, name} when a buff ends, "combat.death"
-- {entity, name}, "combat.revive" {entity, name}. Health is replicated on its own, once per hit or heal. A chasing or returning NPC costs one
-- EntityMove a tick to each viewer, like a patrolling one; a rooted one costs nothing.
local GCD = 1 -- seconds: the global cooldown every power starts
local POWERS = { -- by class; the order is the key (1-3). effect tells clients what to draw.
    warrior = {
        {id = "strike", name = "Strike", cooldown = 1.5, effect = "melee", range = 3, damage = 12},
        {id = "heroic_slam", name = "Heroic Slam", cooldown = 8, effect = "melee", range = 3, damage = 25},
        {id = "shield_wall", name = "Shield Wall", cooldown = 30, effect = "shield", duration = 6, taken = 0.5},
    },
    mage = {
        {id = "firebolt", name = "Firebolt", cooldown = 2, effect = "bolt", range = 25, damage = 14},
        {id = "frost_nova", name = "Frost Nova", cooldown = 20, effect = "nova", radius = 8, root = 4},
        {id = "arcane_blast", name = "Arcane Blast", cooldown = 10, effect = "bolt", range = 25, damage = 30},
    },
    priest = {
        {id = "smite", name = "Smite", cooldown = 1.5, effect = "bolt", range = 25, damage = 10},
        {id = "heal", name = "Heal", cooldown = 10, effect = "heal", heal = 40},
        {id = "power_shield", name = "Power Shield", cooldown = 25, effect = "shield", duration = 10, absorb = 30},
    },
    rogue = {
        {id = "sinister_strike", name = "Sinister Strike", cooldown = 1.2, effect = "melee", range = 3, damage = 11},
        {id = "backstab", name = "Backstab", cooldown = 8, effect = "melee", range = 3, damage = 30, behind = true},
        {id = "evasion", name = "Evasion", cooldown = 30, effect = "evasion", duration = 8, dodge = 0.5},
    },
}
local ATTACK_RANGE = 3       -- metres, on x/z: an NPC's reach
local NPC_ATTACKS = {        -- by NPC definition name (npcs/)
    Guard = {damage = 5, interval = 2},
    Bat = {damage = 2, interval = 1.5},
}
local CHASE_SPEED = 5        -- m/s, chasing and returning
local REPATH_SECONDS = 0.5   -- how often a chasing NPC plans its route to the target again
local LEASH_RANGE = 30       -- metres from its spawn point, on x/z
local REVIVE_SECONDS = 5
local RESUME_STATE = {patrol = "walking", creature = "idle"} -- by machine: the state after returning

local POWER, CLASS_OF = {}, {} -- power id -> its definition, its class
for class, powers in pairs(POWERS) do
    for _, power in ipairs(powers) do POWER[power.id], CLASS_OF[power.id] = power, class end
end

local now = 0           -- seconds since start, summed from dt
-- ponytail: cooldowns and buffs live in memory, so a relog resets them; save them with the character if that matters.
local readyAt = {}      -- player -> {[power id] = time it may be used again}
local gcdUntil = {}     -- player -> time the global cooldown ends
local buffs = {}        -- player -> {[power id] = {power = definition, endsAt = time, left = absorb left}}
local rootedUntil = {}  -- npc -> time it may move again
local deadUntil = {}    -- player -> time it revives
local fights = {}       -- npc -> {target = player, swingAt = time, home = {x, y, z}, route = corners, leg, repathAt}
local tagged = {}       -- npc -> the player who hit it first
local gear = {}         -- player -> {armor =, damage =}, its gear's totals ("equipment.stats")

local function exists(e, component)
    local ok, has = pcall(kast.has, e, component) -- raises for ids that are not in the world
    return ok and has
end

local function name(e)
    if exists(e, "Character") then return kast.get(e, "Character").name end
    return kast.get(e, "Npc").name
end

local function names(attacker, target)
    return {attacker = attacker, target = target, attackerName = name(attacker), targetName = name(target)}
end

local function flatDistance(a, b)
    return math.sqrt((a.x - b.x) ^ 2 + (a.z - b.z) ^ 2)
end

local function alive(e)
    return deadUntil[e] == nil and kast.get(e, "Health").current > 0
end

local function rooted(npc)
    return (rootedUntil[npc] or 0) > now
end

local function engaged(a, b)
    kast.publish("combat.engaged", {entity = a})
    kast.publish("combat.engaged", {entity = b})
end

local function fade(player, id)
    local buff = buffs[player][id]
    buffs[player][id] = nil
    kast.sendNearby(player, "combat.fade", {entity = player, power = id, name = buff.power.name})
end

-- The caster's buff whose definition has field (taken, absorb, dodge), if any.
local function buffWith(player, field)
    for _, buff in pairs(buffs[player] or {}) do
        if buff.power[field] then return buff end
    end
end

local function killPlayer(e)
    deadUntil[e] = now + REVIVE_SECONDS
    for id in pairs(buffs[e] or {}) do fade(e, id) end
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

-- Takes damage off target's Health; player is the one who sends the events (the attacker or the victim). extra goes
-- into the combat.hit event (power, absorbed, absorbedBy).
local function hit(attacker, target, damage, player, extra)
    local health = kast.get(target, "Health")
    local current = math.max(0, health.current - damage)
    kast.set(target, "Health", {current = current})
    local event = names(attacker, target)
    event.damage = damage
    for key, value in pairs(extra) do event[key] = value end
    kast.sendNearby(player, "combat.hit", event)
    engaged(attacker, target)
    if current > 0 then return end
    if exists(target, "Character") then
        killPlayer(target)
    else
        fights[target], rootedUntil[target] = nil, nil
        if exists(target, "StateMachine") then kast.fsm.change(target, "dead") end
        kast.sendNearby(player, "combat.death", {entity = target, name = name(target)})
        kast.publish("npc.died", {npc = target, tagger = tagged[target]})
        tagged[target] = nil
    end
end

-- An NPC's hit on a player, through the player's gear and buffs: dodged, then armor, then reduced, then absorbed.
local function npcHit(npc, player, damage)
    local evasion = buffWith(player, "dodge")
    if evasion and math.random() < evasion.power.dodge then
        kast.sendNearby(player, "combat.dodge", names(npc, player))
        engaged(npc, player)
        return
    end
    damage = math.max(1, damage - (gear[player] and gear[player].armor or 0))
    local wall = buffWith(player, "taken")
    if wall then damage = math.floor(damage * wall.power.taken) end
    local shield, absorbed = buffWith(player, "absorb"), nil
    if shield then
        absorbed = math.min(shield.left, damage)
        shield.left, damage = shield.left - absorbed, damage - absorbed
        if shield.left <= 0 then fade(player, shield.power.id) end
    end
    hit(npc, player, damage, player, {absorbed = absorbed, absorbedBy = shield and shield.power.name})
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

-- Caster within the 180 degree arc behind target (the half-plane behind its heading; 0 faces +z).
local function behind(caster, target)
    local from, to = kast.get(caster, "Transform").position, kast.get(target, "Transform")
    local dx, dz = from.x - to.position.x, from.z - to.position.z
    return dx * math.sin(to.heading) + dz * math.cos(to.heading) < 0
end

-- Why target can't be hit at all, or nil.
local function targetProblem(target)
    if type(target) ~= "number" or not exists(target, "Npc") or not exists(target, "Health") then return "target" end
    if not alive(target) then return "target_dead" end
end

-- Why power can't reach target from where the caster stands, or nil.
local function reachProblem(caster, target, power)
    local from, to = kast.get(caster, "Transform"), kast.get(target, "Transform")
    if from.map ~= to.map or flatDistance(from.position, to.position) > power.range then return "range" end
    if power.behind and not behind(caster, target) then return "not_behind" end
end

-- Roots every living NPC on the caster's map within power.radius; returns their ids.
local function rootAround(caster, power)
    local from = kast.get(caster, "Transform")
    local list = {}
    for npc in kast.query("Npc", "Health") do
        local tf = kast.get(npc, "Transform")
        if alive(npc) and tf.map == from.map and flatDistance(tf.position, from.position) <= power.radius then
            rootedUntil[npc] = now + power.root
            kast.publish("npc.rooted", {npc = npc, seconds = power.root})
            list[#list + 1] = npc
        end
    end
    return list
end

local function onPower(event)
    local caster = event.entity
    local data = type(event.data) == "table" and event.data or {}
    if not alive(caster) then return reject(caster, "dead") end
    local power = type(data.power) == "string" and POWER[data.power]
    if not power then return reject(caster, "unknown_power") end
    if not exists(caster, "Class") or kast.get(caster, "Class").id ~= CLASS_OF[power.id] then
        return reject(caster, "wrong_class")
    end
    local target = data.target
    local problem = power.damage and targetProblem(target)
    if problem then return reject(caster, problem) end
    local ready = readyAt[caster] or {}
    if (ready[power.id] or 0) > now then return reject(caster, "cooldown") end
    if (gcdUntil[caster] or 0) > now then return reject(caster, "gcd") end
    problem = power.damage and reachProblem(caster, target, power)
    if problem then return reject(caster, problem) end
    ready[power.id] = now + power.cooldown
    readyAt[caster], gcdUntil[caster] = ready, now + GCD

    local used = {caster = caster, casterName = name(caster), power = power.id, name = power.name, effect = power.effect}
    if power.damage then
        used.target, used.targetName = target, name(target)
        kast.sendNearby(caster, "combat.power", used)
        tagged[target] = tagged[target] or caster
        hit(caster, target, power.damage + (gear[caster] and gear[caster].damage or 0), caster, {power = power.name})
        if alive(target) and exists(target, "StateMachine") then startFight(target, caster) end
    elseif power.root then
        used.rooted = rootAround(caster, power)
        kast.sendNearby(caster, "combat.power", used)
    elseif power.heal then
        local health = kast.get(caster, "Health")
        used.amount = math.min(power.heal, health.max - health.current)
        kast.set(caster, "Health", {current = health.current + used.amount})
        kast.sendNearby(caster, "combat.power", used)
    else
        buffs[caster] = buffs[caster] or {}
        buffs[caster][power.id] = {power = power, endsAt = now + power.duration, left = power.absorb}
        used.duration = power.duration
        kast.sendNearby(caster, "combat.power", used)
    end
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
    local reached = not rooted(npc) and moveToward(npc, f, goal, ATTACK_RANGE * 0.8, dt)
    if reached or flatDistance(pos, goal) <= ATTACK_RANGE then
        local dx, dz = goal.x - pos.x, goal.z - pos.z
        if dx ~= 0 or dz ~= 0 then kast.set(npc, "Transform", {heading = math.atan2(dx, dz)}) end
        if now >= f.swingAt then
            local attack = NPC_ATTACKS[kast.get(npc, "Npc").npc]
            if attack then
                f.swingAt = now + attack.interval
                npcHit(npc, f.target, attack.damage)
            end
        end
    end
end

local function goHome(npc, f, dt)
    if rooted(npc) or not moveToward(npc, f, f.home, 0.1, dt) then return end
    local health = kast.get(npc, "Health")
    kast.set(npc, "Health", {current = health.max})
    if exists(npc, "Spawn") then kast.set(npc, "Transform", {heading = kast.get(npc, "Spawn").heading}) end
    fights[npc], tagged[npc] = nil, nil
    kast.fsm.change(npc, RESUME_STATE[kast.get(npc, "StateMachine").machine] or "idle")
end

-- The owner's action bar: its class's powers, in key order.
local function sendPowers(e)
    local class = kast.get(e, "Class").id
    local list = {}
    for key, p in ipairs(POWERS[class]) do
        list[key] = {id = p.id, name = p.name, key = key, cooldown = p.cooldown, effect = p.effect}
    end
    kast.sendTo(e, "class.powers", {class = class, gcd = GCD, powers = list})
end

return {
    init = function()
        kast.subscribe("client.combat.power", onPower)
        kast.subscribe("equipment.stats", function(e)
            if exists(e.entity, "Character") then gear[e.entity] = {armor = e.armor, damage = e.damage} end
        end)
    end,

    onEnter = function(e)
        if kast.has(e, "Class") then sendPowers(e) end -- systems/classes.lua ran first
        if kast.has(e, "Health") and kast.get(e, "Health").current <= 0 then killPlayer(e) end -- logged out dead
    end,

    onLeave = function(e)
        readyAt[e], gcdUntil[e], buffs[e], deadUntil[e], gear[e] = nil, nil, nil, nil, nil
    end,

    update = function(dt)
        now = now + dt
        for npc in pairs(tagged) do
            if not exists(npc, "Npc") then tagged[npc] = nil end
        end
        for npc, at in pairs(rootedUntil) do
            if at <= now or not exists(npc, "Npc") then rootedUntil[npc] = nil end
        end
        for player, list in pairs(buffs) do
            for id, buff in pairs(list) do
                if now >= buff.endsAt then fade(player, id) end
            end
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
