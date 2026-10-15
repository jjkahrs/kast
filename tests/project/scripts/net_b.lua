-- kast.net across a scene switch, part 2: re-acquires net_a.lua's connections by name and checks that they are the
-- same sockets (each server's "id" answer is 1: one accept, no reconnect), that every tick of the stream started
-- before the switch arrives in order, and that both requests started before the switch answer here. Logs one
-- "net_b: PASS" or "net_b: FAIL ..." line.
local names = {"tcp", "tls", "ws", "wss"}
local ticks, done, ids, wss, responses = 0, false, {}, nil, 0
local elapsed, reported = 0, false

local function fail(why)
    if not reported then kast.error("FAIL " .. why) end
    reported = true
end

return {
    init = function()
        for _, name in ipairs(names) do
            local h = kast.net.get(name)
            if not h or h:state() ~= "open" then return fail(name .. " did not survive the switch") end
        end
        kast.net.get("tcp"):send("id")
        kast.net.get("tls"):send("id")
        kast.net.get("ws"):send("id")
        kast.net.get("wss"):send({hello = "wss", n = 1})
        kast.subscribe("net.message", function(e)
            if e.conn == "wss" then
                wss = e.data
            elseif string.sub(e.data, 1, 3) == "id " then
                ids[e.conn] = e.data
            elseif string.sub(e.data, 1, 5) == "tick " then
                ticks = ticks + 1
                if tonumber(string.sub(e.data, 6)) ~= ticks then fail("tick out of order: " .. e.data) end
            elseif e.data == "done 100" then
                done = true
            end
        end)
        kast.subscribe("net.http.response", function(e)
            local body = e.body
            if type(body) == "table" then body = body.method .. " " .. body.target .. " " .. body.body end
            kast.log(string.format("response %d: %d %s", e.id, e.status, tostring(body)))
            responses = responses + 1
        end)
        kast.subscribe("net.error", function(e) fail(tostring(e.conn or e.id) .. ": " .. e.message) end)
        kast.subscribe("net.http.error", function(e) fail("request " .. e.id .. ": " .. e.message) end)
    end,
    update = function(dt)
        elapsed = elapsed + dt
        if reported then return end
        local same = ids.tcp == "id 1" and ids.tls == "id 1" and ids.ws == "id 1"
        if done and ticks == 100 and same and wss and wss.hello == "wss" and responses == 2 then
            kast.log("PASS: 4 connections kept across kast.loadScene, 100/100 ticks, 2/2 responses")
            reported = true
        elseif elapsed > 8 then
            fail(string.format("timed out: ticks=%d done=%s ids=%s,%s,%s wss=%s responses=%d", ticks, tostring(done),
                 tostring(ids.tcp), tostring(ids.tls), tostring(ids.ws), tostring(wss and wss.hello), responses))
        end
    end,
}
