-- Демо: cron каждые 30 секунд.
return {
    desc = "every 30s",
    rules = {
        { cron = "*/30 * * * * *", body = function()
            puts(clock(), "fired: every 30 seconds")
        end },
    }
}
