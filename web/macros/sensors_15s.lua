meta{ desc = "sensors every 15s (cron */15)" }

every("*/15 * * * * *", function()
    print("SENS15", "bmp", bmp280.available, bmp280.temperature, bmp280.pressure,
          "ccs", ccs811.available, ccs811.eco2, ccs811.tvoc,
          "hdc", hdc1080.available, hdc1080.temperature, hdc1080.humidity)
end)
