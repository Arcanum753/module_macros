-- WTEST-00: bootstrap. Первое валидное время (NTP/DS3231): TZ=UTC+3, macro, баннер.
-- Только on time.synced (без условия) — чтобы не повторять на каждом реконнекте STA.
meta{ desc = "WTEST-00 boot: tz+macro on sync" }

local function start(ev)
    print("==== WTEST START (", ev.spec, ") ====")
    print("tz before=", time.tz, "now=", time.now_str, "src=", time.source)
    print("set tz rc=", set("time.tz", 30))
    local _, rcSave = call("time.save")
    print("time.save rc=", rcSave)
    print("wifi.mode rc=", set("wifi.mode", "macro"))
    print("e7.mode rc=", set("e7.mode", "macro"))
    print("==== WTEST READY ====")
end

on("time.synced", start)
