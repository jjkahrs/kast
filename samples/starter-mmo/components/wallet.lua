-- Shared by client and server (see health.lua). Owner-only: only the owning player's client ever receives it.
return {
    name = "Wallet",
    persist = true,
    replicate = "owner",
    fields = {
        { name = "gold", type = "int", default = 0, min = 0 },
    },
}
