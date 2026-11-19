meta{ desc = "sensors every minute (cron 0 * * * * *)" }

every("0 * * * * *", function()
    print("SENS1M", "bmp", bmp280.available, bmp280.temperature, bmp280.pressure,
          "ccs", ccs811.available, ccs811.eco2, ccs811.tvoc,
          "hdc", hdc1080.available, hdc1080.temperature, hdc1080.humidity)
end)
