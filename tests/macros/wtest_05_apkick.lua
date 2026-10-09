-- WTEST-05 (19:58): ещё цвета + перебор эффектов.
return {
    desc = "WTEST-05 19:58 colors B + effects",
    rules = {
        { when = { cron = "0 58 19 * * *" }, call = "e7.color", args = { 16744448 } },
        { when = { cron = "0 0 20 * * *" }, call = "e7.color", args = { 8388736 } },
        { when = { cron = "0 2 20 * * *" }, call = "e7.effect", args = { "rainbow" } },
        { when = { cron = "0 4 20 * * *" }, call = "e7.effect", args = { "grad_static" } },
        { when = { cron = "0 6 20 * * *" }, call = "e7.effect", args = { "grad_dynamic" } },
        { when = { cron = "0 8 20 * * *" }, call = "e7.effect", args = { "colorcycle" } },
        { when = { cron = "0 10 20 * * *" }, call = "e7.effect", args = { "mono" } },
    },
}
