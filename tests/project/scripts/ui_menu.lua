-- Reacts to the main menu GUI (ui/main_menu.rml): its buttons publish "menu.start" / "menu.quit" with
-- {id = <button id>, document = <rml path>}.
return {
    init = function()
        kast.subscribe("menu.start", function(event)
            kast.log("Menu start clicked: " .. event.id .. " in " .. event.document)
        end)
        kast.subscribe("menu.quit", function(event)
            kast.log("Menu quit clicked: " .. event.id .. " in " .. event.document)
        end)
    end,
    update = function(dt) end,
}
