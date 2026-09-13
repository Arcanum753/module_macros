-- Демонстрация: cron прямо в правиле (when), без мета-окна.
return {
    desc = "every minute via when.cron",
    rules = {
        { when = { cron = "0 * * * * *" }, call = "e7.brightness", args = { 8 } },
    },
}
