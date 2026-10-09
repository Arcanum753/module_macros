-- WTEST-02 (19:10): target ap/sta + force_connect по двум слотам (оба роутера).
return {
    desc = "WTEST-02 19:10 target/slot",
    rules = {
        { when = { cron = "0 10 19 * * *" }, set = "wifi.target", value = "ap" },
        { when = { cron = "0 14 19 * * *" }, set = "wifi.target", value = "sta" },
        { when = { cron = "0 18 19 * * *" }, calls = {
            { name = "wifi.set_slot",     args = { "0" } },
            { name = "wifi.force_connect" },
        } },
        { when = { cron = "0 23 19 * * *" }, calls = {
            { name = "wifi.set_slot",     args = { "1" } },
            { name = "wifi.force_connect" },
        } },
        { when = { cron = "0 28 19 * * *" }, call = "wifi.force_scan", args = { 5 } },
    },
}
