-- TEST ONLY (ctest starter_mmo_hud_masks, cmake/starter_mmo_hud_masks.cmake): added to a copy of samples/starter-mmo's
-- meadow.scene, never to the sample itself. Offline, so it plays the server's part on the bus: the local player enters
-- with a mage's three powers (a 4 s global cooldown), the Guard is selected ("target.selected", kind npc), 25 chat lines
-- fill the chat box and the server confirms a Firebolt (8 s cooldown). SHOT_AFTER frames later it logs HUDSHOT (for
-- --screenshot-on-log): Firebolt about three quarters cooled down, Frost Nova and Heal half way through the GCD.
local START, SHOT_AFTER, CHAT_LINES = 20, 120, 25
local ME = 1
local frame = 0

return {
    update = function(dt)
        frame = frame + 1
        if frame == START then
            kast.publish("server.welcome", {name = "Tester"})
            kast.publish("world.entered", {entityId = ME, map = 10, position = {x = 128, y = 5.43, z = 128}, heading = 0})
            kast.publish("server.class.powers", {class = "mage", gcd = 4, powers = {
                {id = "firebolt", name = "Firebolt", key = 1, cooldown = 8, effect = "bolt"},
                {id = "frost_nova", name = "Frost Nova", key = 2, cooldown = 12, effect = "nova"},
                {id = "heal", name = "Heal", key = 3, cooldown = 0, effect = "heal"}}})
            kast.publish("target.selected", {entityId = 9001, name = "Guard", kind = "npc",
                                             health = {current = 38, max = 50}})
            for i = 1, CHAT_LINES do kast.publish("chat.local", {text = "Chat line " .. i}) end
            kast.publish("server.combat.power", {caster = ME, power = "firebolt", name = "Firebolt"})
        elseif frame == START + SHOT_AFTER then
            kast.log("test hud masks: HUDSHOT")
        end
    end,
}
