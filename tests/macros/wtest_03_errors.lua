-- WTEST-03 (19:30): disconnect, NOT_FOUND, *_kick.
local function nf(name)
    print("NF set_slot rc=", set("wifi.set_slot", name), "name=", name)
    local v, rc = call("wifi.force_connect")
    print("NF force_connect rc=", rc, "v=", v)
end
return {
    desc = "WTEST-03 19:30 errors",
    handlers = {
        nf1 = function() nf("no_such_net_zzz") end,
        nf2 = function() nf("") end,
        r1  = function() print("NF restore rc=", set("wifi.set_slot", "1")) end,
    },
    rules = {
        { when = { cron = "0 30 19 * * *" }, call = "wifi.force_disconnect" },
        { when = { cron = "0 32 19 * * *" }, run = "nf1" },
        { when = { cron = "0 34 19 * * *" }, run = "r1" },
        { when = { cron = "0 36 19 * * *" }, run = "nf2" },
        { when = { cron = "0 38 19 * * *" }, run = "r1" },
        { when = { cron = "0 40 19 * * *" }, calls = {
            { name = "wifi.force_ap_kick" },
            { name = "wifi.force_connect_kick" },
        } },
    },
}
