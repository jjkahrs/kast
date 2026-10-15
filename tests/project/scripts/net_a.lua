-- kast.net across a scene switch, part 1 (run with kast_net_test_server, see tests/net_test_server.cpp): opens one
-- connection of each kind, and once all four are open asks the TCP server for a tick stream, starts two slow HTTP
-- requests and switches to net_b.scene in the same frame. Everything that answers arrives in net_b.lua.
local ca = "certs/net_test_ca.pem"
local opened = 0

return {
    init = function()
        kast.net.open{name = "tcp", type = "tcp", host = "127.0.0.1", port = 47701, framing = "line"}
        kast.net.open{name = "tls", type = "tcp", host = "localhost", port = 47702, tls = true, caFile = ca,
                      framing = "line"}
        kast.net.open{name = "ws", type = "ws", url = "ws://127.0.0.1:47703/"}
        kast.net.open{name = "wss", type = "ws", url = "wss://localhost:47704/", caFile = ca, encoding = "json"}
        kast.subscribe("net.error", function(e) kast.error("FAIL " .. tostring(e.conn) .. ": " .. e.message) end)
        kast.subscribe("net.open", function(e)
            opened = opened + 1
            if opened < 4 then return end
            kast.net.get("tcp"):send("stream 100")
            kast.net.request{url = "http://127.0.0.1:47705/slow"}
            kast.net.request{method = "POST", url = "https://localhost:47706/slow", caFile = ca, body = "hello",
                             encoding = "json"}
            kast.log("4 connections open, switching to net_b.scene")
            kast.loadScene("net_b.scene")
        end)
    end,
    update = function(dt) end,
}
