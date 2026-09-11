-- Демо: cron каждые 10 секунд, счётчик сохраняется в замыкании.
local ticks = 0

return {
    desc = "every 10s (counter)",
    rules = {
        { cron = "*/10 * * * * *", body = function()
            ticks = ticks + 1
            puts(clock(), "fired: every 10 seconds, tick", ticks)
        end },
    }
}
