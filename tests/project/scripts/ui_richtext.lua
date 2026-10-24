-- Drives ui/richtext.rml (ctest ui_richtext_*, player_richtext_*): logs every gui.link click (RICHLINK), binds model
-- "richbind".line to BBCode in init and replaces it on frame 30 (REBOUND); frame 2 writes rich text with
-- kast.gui.setRichText and markup with kast.gui.setText, which shows it as typed (SET).
local frame = 0

return {
    init = function()
        kast.subscribe("gui.link", function(e)
            kast.log(string.format("richtext: RICHLINK target=%s id=%s document=%s", e.target, e.id, e.document))
        end)
        kast.gui.set("richbind", "line", "[color=#40c0ff]Bound[/color] [b]rich[/b] line")
    end,
    update = function()
        frame = frame + 1
        if frame == 2 then
            kast.gui.setRichText("rich_lua", "[i]Set by[/i] [b]kast.gui.setRichText[/b]")
            kast.gui.setText("rich_plain", "[b]setText[/b] <b>stays plain</b>")
            kast.log("richtext: SET")
        elseif frame == 30 then
            kast.gui.set("richbind", "line", "[color=#ff9900]Rebound[/color] [u]binding[/u]")
            kast.log("richtext: REBOUND")
        end
    end,
}
