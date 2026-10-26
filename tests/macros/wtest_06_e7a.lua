-- WTEST-06 (20:12): яркость границы + e7 ошибки/гейтинг + e7.save.
meta{ desc = "WTEST-06 20:12 brightness+errors" }

local function bad()
    print("E7 effect bogus rc=",   set("e7.effect", "bogus"))
    print("E7 effect 9 rc=",       set("e7.effect", 9))
    local v1, rc1 = call("e7.speed", 0)
    local v2, rc2 = call("e7.speed", 51)
    print("E7 speed 0 rc=",        rc1, "v=", v1)
    print("E7 speed 51 rc=",       rc2, "v=", v2)
    print("E7 brightness 300 rc=", set("e7.brightness", 300))
    print("E7 color bad rc=",      set("e7.color", "#GGGGGG"))
    print("E7 color short rc=",    set("e7.color", "#FFF"))
end

local function deny()
    print("E7 set off rc=",     set("e7.mode", "off"))
    print("E7 effect mono rc=", set("e7.effect", "mono"))
    print("E7 set auto rc=",    set("e7.mode", "auto"))
    print("E7 effect mono rc=", set("e7.effect", "mono"))
    print("E7 set macro rc=",   set("e7.mode", "macro"))
end

local function save()
    print("E7 save rc=", call("e7.save"))
end

every("0 12 20 * * *", function() e7.brightness = 5 end)
every("0 14 20 * * *", function() e7.brightness = 50 end)
every("0 16 20 * * *", function() e7.brightness = 255 end)
every("0 18 20 * * *", bad)
every("0 20 20 * * *", deny)
every("0 22 20 * * *", save)
every("0 24 20 * * *", function() e7.color("#FF0000") end)
