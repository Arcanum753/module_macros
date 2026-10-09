-- WTEST-00: bootstrap. Первое валидное время (NTP/DS3231): TZ=UTC+3, macro, баннер.
-- Только on time.synced (без cond) — cond повторялся на каждом реконнекте STA.
return {
    desc = "WTEST-00 boot: tz+macro on sync",
    handlers = {
        start = function(ev)
            print("==== WTEST START (", ev.spec, ") ====")
            print("tz before=", get("time.tz"), "now=", get("time.now_str"), "src=", get("time.source"))
            print("set tz rc=", set("time.tz", 30))
            local _, rcSave = call("time.save")
            print("time.save rc=", rcSave)
            print("wifi.mode rc=", set("wifi.mode", "macro"))
            print("e7.mode rc=", set("e7.mode", "macro"))
            print("==== WTEST READY ====")
        end,
    },
    rules = {
        { when = { on = "time.synced" }, run = "start" },
    },
}
