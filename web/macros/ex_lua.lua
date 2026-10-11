-- каждый час: печать IP (если подключено) и времени.
meta{ desc = "hourly: print ip and clock" }

every("0 * * * * *", function()
    if wifi.connected then
        print("ip:", wifi.ip)
    end
    print(clock())
end)
