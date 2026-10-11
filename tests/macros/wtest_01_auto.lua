-- WTEST-01 (19:00): автономный режим + гейт macro (DENIED/BAD_VALUE) + красный.
meta{ desc = "WTEST-01 19:00 auto + gate" }

local function one(fn, ...)
    local v, rc = call(fn, ...)
    print("PROBE", fn, "rc=", rc, "v=", v)
end

local function probeAuto()
    one("wifi.force_ap")
    one("wifi.force_ap_kick")
    one("wifi.force_connect")
    one("wifi.force_connect_kick")
    one("wifi.force_scan", 10)
    one("wifi.force_disconnect")
    print("PROBE set_slot rc=", set("wifi.set_slot", "1"))
    local v, rc = call("wifi.mode", 5)
    print("PROBE wifi.mode(5) rc=", rc, "v=", v)
end

every("0 0 19 * * *", function()
    wifi.mode("auto")
    e7.mode("macro")
    e7.effect("mono")
    e7.color(16711680)
end)

every("0 3 19 * * *", probeAuto)

every("0 7 19 * * *", function()
    wifi.mode("macro")
end)
