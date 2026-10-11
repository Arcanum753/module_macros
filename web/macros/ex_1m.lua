-- каждая минута: яркость = 8.
meta{ desc = "every minute: brightness = 8" }

every("0 * * * * *", function()
    e7.brightness = 8
end)
