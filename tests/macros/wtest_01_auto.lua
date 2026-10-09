-- WTEST-01 (19:00): автономный режим + гейт macro (DENIED/BAD_VALUE) + красный.
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
return {
    desc = "WTEST-01 19:00 auto + gate",
    handlers = { p = function(ev) probeAuto() end },
    rules = {
        { when = { cron = "0 0 19 * * *" }, calls = {
            { name = "wifi.mode", args = { "auto" } },
            { name = "e7.mode",   args = { "macro" } },
            { name = "e7.effect", args = { "mono" } },
            { name = "e7.color",  args = { 16711680 } },
        } },
        { when = { cron = "0 3 19 * * *" }, run = "p" },
        { when = { cron = "0 7 19 * * *" }, calls = {
            { name = "wifi.mode", args = { "macro" } },
        } },
    },
}
