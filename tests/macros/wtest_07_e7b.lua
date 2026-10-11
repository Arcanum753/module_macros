-- WTEST-07 (20:25): чередование AP/STA в конце теста.
meta{ desc = "WTEST-07 20:25 integration ap/sta" }

every("0 25 20 * * *", function() wifi.target = "ap" end)
every("0 26 20 * * *", function() wifi.target = "sta" end)
every("0 27 20 * * *", function() wifi.target = "ap" end)
