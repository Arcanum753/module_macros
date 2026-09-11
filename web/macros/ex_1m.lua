-- Демо: каждая минута (в 0-ю секунду).
return {
    desc = "every minute",
    rules = {
        { cron = "0 * * * * *", body = function()
            puts(clock(), "fired: every minute")
        end },
    }
}
