-- WTEST-08: контрольные снимки состояния (wifi + e7 + time) в ключевых точках.
meta{ desc = "WTEST-08 checkpoints" }

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

every("0 21 19 * * *", function(ev) snap(ev.spec) end)
every("0 26 19 * * *", function(ev) snap(ev.spec) end)
every("0 41 19 * * *", function(ev) snap(ev.spec) end)
every("0 1 20 * * *",  function(ev) snap(ev.spec) end)
every("0 15 20 * * *", function(ev) snap(ev.spec) end)
