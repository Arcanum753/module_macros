-- Демо: срабатывание в конкретную минуту и секунду каждого часа.
-- Сейчас задано: 42-я минута, 15-я секунда (00:42:15, 01:42:15, ...).
-- Поля cron: секунды минуты часы день-мес месяц день-нед.
-- Запуск: macro run ex_minsec.lua

local function show(msg)
    puts(clock())
    puts(msg)
end

cron("15 42 * * * *", function()
    show("fired: every hour at 00:42:15")
end)
