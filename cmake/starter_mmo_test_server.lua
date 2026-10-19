-- TEST ONLY (ctest starter_mmo_live_*, world_self_check's starter-mmo loot check): a server system added next to
-- samples/starter-mmo's systems/ in a test-time copy of its server Lua, never to the sample itself. It lets the test
-- driver set up known states that real play would take minutes (or luck) to reach:
--   "test.seed" {seed}            math.randomseed(seed): loot rolls after it are the same on every run.
--   "test.bag"  {items, counts}   replaces the sender's bag (arrays of up to 16; missing slots are empty).
--   "test.equipment" {}           takes off all the sender's gear without putting it in the bag (each stage starts
--                                 bare), and publishes the bare totals as systems/inventory.lua would.
-- Each answers the sender with "test.done" {what}.
return {
    init = function()
        kast.subscribe("client.test.seed", function(event)
            math.randomseed(event.data.seed)
            kast.sendTo(event.entity, "test.done", {what = "seed"})
        end)
        kast.subscribe("client.test.bag", function(event)
            local items, counts = {}, {}
            for i = 1, 16 do
                items[i], counts[i] = event.data.items[i] or "", event.data.counts[i] or 0
            end
            kast.set(event.entity, "Inventory", {items = items, counts = counts})
            kast.sendTo(event.entity, "test.done", {what = "bag"})
        end)
        kast.subscribe("client.test.equipment", function(event)
            kast.set(event.entity, "Equipment", {items = {"", "", "", "", "", "", ""}})
            kast.publish("equipment.stats", {entity = event.entity, armor = 0, damage = 0, stamina = 0})
            kast.sendTo(event.entity, "test.done", {what = "equipment"})
        end)
    end,
    update = function(dt) end,
}
