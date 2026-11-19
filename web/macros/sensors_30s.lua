meta{ desc = "sensors every 30s (cron */30)" }

every("*/30 * * * * *", function()
    print("SENS30", "bmp", bmp280.available, bmp280.temperature, bmp280.pressure,
          "ccs", ccs811.available, ccs811.eco2, ccs811.tvoc,
          "hdc", hdc1080.available, hdc1080.temperature, hdc1080.humidity)
end)
