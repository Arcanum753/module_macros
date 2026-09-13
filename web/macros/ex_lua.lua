-- Единственный пример named-handler (тяжёлый путь).
-- Файл перечитывается и компилируется при каждом срабатывании.
-- Использовать для редкой логики с ветвлениями/циклами/замыканиями.
return {
    desc = "lua handler: complex logic (heavy path)",
    handlers = {
        report = function(ev)
            if get("wifi.connected") then
                puts("ip:", get("wifi.ip"), "rssi:", get("wifi.rssi"))
            end
            puts("time valid:", get("time.valid"))
        end,
    },
    rules = {
        { when = { cron = "0 * * * * *" }, run = "report" },
    },
}
