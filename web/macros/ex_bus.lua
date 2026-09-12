-- Демо: ресурсная шина core_state.
-- cron-правило вызывает named-handler; cond — декларативное (ресурс+оператор+значение).
return {
    desc = "bus API demo (wifi/time)",
    handlers = {
        show = function(ev)
            if get("wifi.connected") then
                puts("ip:", get("wifi.ip"), "rssi:", get("wifi.rssi"))
            end
            puts("time valid:", get("time.valid"))
        end,
        onconn = function(ev)
            puts("wifi connected, ip", get("wifi.ip"))
        end,
    },
    rules = {
        { cron = "*/5 * * * * *", run = "show" },
        { cond = { res = "wifi.connected", op = "==", val = true }, run = "onconn" },
    },
}
