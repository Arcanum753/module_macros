-- WTEST-06 (20:12): яркость границы + e7 ошибки/гейтинг + e7.save.
local function bad()
    print("E7 effect bogus rc=",   set("e7.effect", "bogus"))
    print("E7 effect 9 rc=",       set("e7.effect", 9))
    local v1, rc1 = call("e7.speed", 0)
    local v2, rc2 = call("e7.speed", 51)
    print("E7 speed 0 rc=",        rc1, "v=", v1)
    print("E7 speed 51 rc=",       rc2, "v=", v2)
    print("E7 brightness 300 rc=", set("e7.brightness", 300))
    print("E7 color -1 rc=",       set("e7.color", -1))
end
local function deny()
    print("E7 set off rc=",     set("e7.mode", "off"))
    print("E7 effect mono rc=", set("e7.effect", "mono"))
    print("E7 set auto rc=",    set("e7.mode", "auto"))
    print("E7 effect mono rc=", set("e7.effect", "mono"))
    print("E7 set macro rc=",   set("e7.mode", "macro"))
end
return {
    desc = "WTEST-06 20:12 brightness+errors",
    handlers = {
        bad  = function() bad() end,
        deny = function() deny() end,
        save = function() print("E7 save rc=", call("e7.save")) end,
    },
    rules = {
        { when = { cron = "0 12 20 * * *" }, call = "e7.brightness", args = { 5 } },
        { when = { cron = "0 14 20 * * *" }, call = "e7.brightness", args = { 50 } },
        { when = { cron = "0 16 20 * * *" }, call = "e7.brightness", args = { 255 } },
        { when = { cron = "0 18 20 * * *" }, run = "bad" },
        { when = { cron = "0 20 20 * * *" }, run = "deny" },
        { when = { cron = "0 22 20 * * *" }, run = "save" },
        { when = { cron = "0 24 20 * * *" }, call = "e7.color", args = { 16711680 } },
    },
}
