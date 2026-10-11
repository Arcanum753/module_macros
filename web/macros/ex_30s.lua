-- every 30s: эффект + скорость (запись ENUM + вызов функции).
meta{ desc = "every 30s: effect = grad_static, speed(30)" }

every("*/30 * * * * *", function()
    e7.effect = "grad_static"
    e7.speed(30)
end)
