-- Server only; see bat_trouble.lua.
return {
    id = "wings_for_the_elder",
    title = "Wings for the Elder",
    giver = "Elder",
    requires = {"bat_trouble"},
    text = "Bat wings make a fine poultice for old bones like mine. Bring me five of them and I will " ..
           "give you a cap that has kept my head dry for many winters.",
    progressText = "Do you have the five Bat Wings?",
    completeText = "Five wings, just as I asked. Here, the cap is yours.",
    objectives = {
        {type = "collect", item = "bat_wing", count = 5},
    },
    rewards = {gold = 15, items = {{item = "leather_cap", count = 1}}},
}
