-- every 10s: effect = "rainbow" (ENUM — только буквенное имя).
meta{ desc = "every 10s: effect = rainbow" }

every("*/10 * * * * *", function()
    e7.effect = "rainbow"
end)
