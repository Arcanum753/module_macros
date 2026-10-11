-- WTEST-02 (19:10): target ap/sta + force_connect по двум слотам (оба роутера).
meta{ desc = "WTEST-02 19:10 target/slot" }

every("0 10 19 * * *", function()
    wifi.target = "ap"
end)

every("0 14 19 * * *", function()
    wifi.target = "sta"
end)

every("0 18 19 * * *", function()
    wifi.set_slot("0")
    wifi.force_connect()
end)

every("0 23 19 * * *", function()
    wifi.set_slot("1")
    wifi.force_connect()
end)

every("0 28 19 * * *", function()
    wifi.force_scan(5)
end)
