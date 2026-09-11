-- Демо: ресурсная шина core_state (get/cond/call_async).
return {
    desc = "bus API demo (wifi/time)",
    rules = {
        { cron = "*/5 * * * * *", body = function()
            if get("wifi.connected") then
                puts("ip:", get("wifi.ip"), "rssi:", get("wifi.rssi"))
            end
            puts("time valid:", get("time.valid"))
        end },
        { cond = function() return get("wifi.connected") end, body = function()
            puts("wifi connected, ip", get("wifi.ip"))
        end },
    }
}
