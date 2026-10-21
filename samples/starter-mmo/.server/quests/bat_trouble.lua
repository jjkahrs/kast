-- Server only (a dot-folder: the editor's Build leaves it out of game.pak). systems/quests.lua reads every file here
-- with kast.definitions(".server/quests"); the fields are explained there.
return {
    id = "bat_trouble",
    title = "Bat Trouble",
    giver = "Elder",
    text = "Bats from the Cavern east of here have been frightening travellers on the road. " ..
           "Take the portal, slay three of them and come back to me.",
    progressText = "The bats still swarm in the Cavern. Have you slain three of them?",
    completeText = "Three fewer bats! The road is safer already. Take this for your trouble.",
    objectives = {
        {type = "kill", npc = "Bat", count = 3, label = "Bats slain"},
    },
    rewards = {gold = 10},
}
