-- Shared by client and server (see health.lua). The character's quests, with primitive arrays only (component fields
-- can't hold tables): active[i] is the id of an accepted quest (at most 20), its objectives' progress is
-- progress[(i - 1) * 4 + 1 .. (i - 1) * 4 + 4] (4 per quest, unused ones 0), completed lists the quests turned in.
-- The quest definitions themselves are server-only (.server/quests/); the client learns titles and objectives from
-- the server's quest.dialog / quest.log events. Only the server changes it (systems/quests.lua): kill progress as
-- NPCs die, collect progress as the bag changes (the live count of the item, capped). The owner's client shows it
-- (scripts/quest.lua, scripts/quest_log.lua). Owner-only, saved per character.
-- Bandwidth: sent whole (a few short strings and ints) on each change: an accept, an abandon, a turn-in, a kill that
-- counts, or a bag change that moves a collect objective.
return {
    name = "Quests",
    persist = true,
    replicate = "owner",
    fields = {
        { name = "active",    type = "array", of = "string", default = {} },
        { name = "progress",  type = "array", of = "int",    default = {}, min = 0 },
        { name = "completed", type = "array", of = "string", default = {} },
    },
}
