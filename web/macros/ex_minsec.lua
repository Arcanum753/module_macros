-- в :42:15 каждого часа, только если Wi-Fi подключён.
meta{ desc = "at 42:15 hourly, if wifi.connected" }

every("15 42 * * * *", function()
    if wifi.connected then
        e7.speed(25)
    end
end)
