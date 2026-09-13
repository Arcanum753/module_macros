-- Демонстрация: meta_cron (гейт) + несколько bus-вызовов в одном правиле.
return {
    desc = "every 30s: set effect + speed",
    meta_cron = "*/30 * * * * *",
    rules = {
        { calls = {
            { name = "e7.effect", args = { 2 } },
            { name = "e7.speed",  args = { 30 } },
        } },
    },
}
