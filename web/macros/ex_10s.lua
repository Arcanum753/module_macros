-- Демо: срабатывание каждые 10 секунд.
-- Запуск: macro run ex_10s.lua
-- После синхронизации NTP раз в 10 секунд печатается время.

local tick_count = 0

local function show(msg)
    puts(clock())
    puts(msg)
end

cron("*/10 * * * * *", function()
    tick_count = tick_count + 1
    show("fired: every 10 seconds")
end)
