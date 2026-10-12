-- Logs which scene is running once Play reaches scene B.
return {
    init = function() kast.log("running " .. tostring(kast.currentScene())) end,
    update = function(dt) end,
}
