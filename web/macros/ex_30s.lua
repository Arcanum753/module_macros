-- Демо: срабатывание каждые 30 секунд.
-- Запуск: macro run ex_30s.lua

local tick_count = 0

local function show(msg)
    puts(clock())
    puts(msg)
end

cron("*/30 * * * * *", function()
    tick_count = tick_count + 1
    show("fired: every 30 seconds")
end)
