-- Демонстрация: meta_cron (гейт) + декларативный bus-вызов функции.
return {
    desc = "every 15s: call e7.speed(50)",
    meta_cron = "*/15 * * * * *",
    rules = {
        { call = "e7.speed", args = { 50 } },
    },
}
