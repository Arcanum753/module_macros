-- Демо: срабатывание каждые 15 секунд.
-- Запуск: macro run ex_15s.lua

local tick_count = 0

local function show(msg)
    puts(clock())
    puts(msg)
end

cron("*/15 * * * * *", function()
    tick_count = tick_count + 1
    show("fired: every 15 seconds")
end)
