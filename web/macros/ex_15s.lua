-- every 15s: вызов функции dual-ресурса (e7.speed(50)).
meta{ desc = "every 15s: e7.speed(50)" }

every("*/15 * * * * *", function()
    e7.speed(50)
end)
