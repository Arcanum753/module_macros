-- Демо: cron каждые 15 секунд.
return {
    desc = "every 15s",
    rules = {
        { cron = "*/15 * * * * *", body = function()
            puts(clock(), "fired: every 15 seconds")
        end },
    }
}
