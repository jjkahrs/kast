-- World flow, in every scene where the player can be connected (scenes/characters.scene and each zone's scene): loads
-- the scene of the map the player is on when it enters the world or a portal moves it to another map (each zone has
-- its own scene, whose Terrain names its map), and sends the player back to login when the world connection drops or
-- the auth session expires, telling the login screen why through the "session" game machine (fsms/session.lua).
-- After a switch the new scene's scripts get world.entered again (engine replication), which is already its scene.
local SCENES = {[10] = "scenes/meadow.scene", [11] = "scenes/cavern.scene"} -- map id -> its zone scene

local function loadZone(map)
    local scene = SCENES[map]
    if not scene then return kast.warn("world: no scene for map " .. tostring(map)) end
    if scene ~= kast.currentScene() then kast.loadScene(scene) end
end

local function backToLogin(reason)
    if kast.fsm.state("session") == "fresh" then kast.fsm.change("session", reason) end
    if kast.auth and kast.auth.isLoggedIn() then kast.auth.logout() end
    kast.loadScene("scenes/login.scene")
end

return {
    init = function()
        kast.subscribe("world.entered", function(e) loadZone(e.map) end)
        kast.subscribe("world.mapChanged", function(e)
            kast.log(string.format("world: map changed to %d at %.1f, %.1f", e.map, e.position.x, e.position.z))
            loadZone(e.map)
        end)
        kast.subscribe("world.disconnected", function(e)
            kast.warn("world: disconnected (" .. e.code .. ")")
            backToLogin("disconnected")
        end)
        kast.subscribe("auth.session.expired", function()
            if kast.world then kast.world.disconnect() end
            backToLogin("expired")
        end)
    end,
    update = function(dt) end,
}
