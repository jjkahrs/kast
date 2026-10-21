-- The Meadow's Elder (maps/meadow/.server/spawns/0_0.json), a friendly quest giver: it stands still and offers the
-- quests whose giver is "Elder" (.server/quests/, systems/quests.lua). Friendly because it has no Health: the server's
-- systems/combat.lua refuses any power aimed at it ("target") and Frost Nova passes it by, so it never fights or dies,
-- and with no loot it drops nothing. Clients don't Tab- or left-click-target an NPC without Health (scripts/combat.lua);
-- a right click talks to it (scripts/quest.lua). Clients clone the scene entity "Elder" under "NPC Templates"
-- (prefabs/npc_templates.prefab, in each zone scene).
return {
    name = "Elder",
    template = "Elder",
}
