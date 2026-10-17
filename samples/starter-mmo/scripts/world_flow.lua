-- World scene flow: shows the props of the map the player is on (MapProps groups) on entering and after a portal (the
-- Terrain switches by itself, docs/engine/terrain.html), and sends the player back to login when the world connection drops or
-- the auth session expires, telling the login screen why through the "session" game machine (fsms/session.lua).
local PARKED = -1000 -- ponytail: props of other maps are moved out of view, not destroyed; fine for a few props

local homes = {} -- MapProps entity -> its scene position

local function showMap(map)
    for e, home in pairs(homes) do
        local here = kast.get(e, "MapProps").map == map
        kast.set(e, "Transform", {position = {x = home.x, y = here and home.y or PARKED, z = home.z}})
    end
end

local function backToLogin(reason)
    if kast.fsm.state("session") == "fresh" then kast.fsm.change("session", reason) end
    if kast.auth and kast.auth.isLoggedIn() then kast.auth.logout() end
    kast.loadScene("scenes/login.scene")
end

return {
    init = function()
        for e in kast.query("MapProps") do homes[e] = kast.get(e, "Transform").position end
        showMap(10)
        kast.subscribe("world.entered", function(e) showMap(e.map) end)
        kast.subscribe("world.mapChanged", function(e)
            kast.log(string.format("world: map changed to %d at %.1f, %.1f", e.map, e.position.x, e.position.z))
            showMap(e.map)
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
