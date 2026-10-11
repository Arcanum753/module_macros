-- WTEST-03 (19:30): disconnect, NOT_FOUND, *_kick.
meta{ desc = "WTEST-03 19:30 errors" }

local function nf(name)
    print("NF set_slot rc=", set("wifi.set_slot", name), "name=", name)
    local v, rc = call("wifi.force_connect")
    print("NF force_connect rc=", rc, "v=", v)
end

local function r1()
    print("NF restore rc=", set("wifi.set_slot", "1"))
end

every("0 30 19 * * *", function() wifi.force_disconnect() end)
every("0 32 19 * * *", function() nf("no_such_net_zzz") end)
every("0 34 19 * * *", r1)
every("0 36 19 * * *", function() nf("") end)
every("0 38 19 * * *", r1)

every("0 40 19 * * *", function()
    wifi.force_ap_kick()
    wifi.force_connect_kick()
end)
