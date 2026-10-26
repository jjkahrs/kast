-- The Meadow's notice board (meadow.scene's "Notice Board": a World-mode, interactive WorldUI on ui/notice_board.rml,
-- fixed on its post beside the spawn). Its Next notice button publishes "notice.next" with the board's entity; this
-- turns to the next of NOTICES and shows the page number. Client-only: the server knows nothing of it.
--
-- Bandwidth: none; everything here is local.
local NOTICES = { -- the first is ui/notice_board.rml's own
    "Bats in the Cavern! The Elder pays for their wings. The gate is east of here.",
    "The Guard patrols the meadow. Strike him and he strikes back.",
    "Weary? Use the Meadow Shrine (E) to be refreshed.",
}
local page = 1

return {
    init = function()
        kast.subscribe("notice.next", function(e)
            page = page % #NOTICES + 1
            kast.gui.setText(e.entity, "notice_text", NOTICES[page])
            kast.gui.setText(e.entity, "notice_page", string.format("%d / %d", page, #NOTICES))
            kast.log(string.format("notice board: page %d", page))
        end)
    end,
    update = function(dt) end,
}
