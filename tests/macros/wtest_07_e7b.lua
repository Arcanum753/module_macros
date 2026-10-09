-- WTEST-07 (20:25): чередование AP/STA в конце теста.
return {
    desc = "WTEST-07 20:25 integration ap/sta",
    rules = {
        { when = { cron = "0 25 20 * * *" }, set = "wifi.target", value = "ap" },
        { when = { cron = "0 26 20 * * *" }, set = "wifi.target", value = "sta" },
        { when = { cron = "0 27 20 * * *" }, set = "wifi.target", value = "ap" },
    },
}
