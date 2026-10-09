-- WTEST-08: контрольные снимки состояния (wifi + e7 + time) в ключевых точках.
local function snap(tag)
    print("SNAP", tag,
        "conn=",     get("wifi.connected"),
        "ip=",       get("wifi.ip"),
        "rssi=",     get("wifi.rssi"),
        "mode=",     get("wifi.mode"),
        "target=",   get("wifi.target"),
        "slot=",     get("wifi.slot"),
        "slotname=", get("wifi.slot_name"),
        "ap=",       get("wifi.ap_mode"),
        "apc=",      get("wifi.ap_clients"),
        "tvalid=",   get("time.valid"),
        "tsrc=",     get("time.source"),
        "e7mode=",   get("e7.mode"),
        "e7fx=",     get("e7.effect"),
        "e7br=",     get("e7.brightness"),
        "e7col=",    get("e7.color"))
end
return {
    desc = "WTEST-08 checkpoints",
    handlers = { s = function(ev) snap(ev.spec) end },
    rules = {
        { when = { cron = "0 21 19 * * *" }, run = "s" },
        { when = { cron = "0 26 19 * * *" }, run = "s" },
        { when = { cron = "0 41 19 * * *" }, run = "s" },
        { when = { cron = "0 1 20 * * *" }, run = "s" },
        { when = { cron = "0 15 20 * * *" }, run = "s" },
    },
}
