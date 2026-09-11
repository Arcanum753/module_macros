-- Демо: конкретные минута и секунда (каждый час в 00:42:15).
return {
    desc = "at 42:15 every hour",
    rules = {
        { cron = "15 42 * * * *", body = function()
            puts(clock(), "fired: hourly at 42:15")
        end },
    }
}
