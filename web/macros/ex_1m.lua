-- Демо: срабатывание каждую минуту (в 0-ю секунду).
-- Запуск: macro run ex_1m.lua

local tick_count = 0

local function show(msg)
    puts(clock())
    puts(msg)
end

cron("0 * * * * *", function()
    tick_count = tick_count + 1
    show("fired: every minute")
end)
