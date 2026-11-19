meta{ desc = "sensors every 5 minutes (cron 0 */5 * * * *)" }

every("0 */5 * * * *", function()
    print("SENS5M", "bmp", bmp280.available, bmp280.temperature, bmp280.pressure,
          "ccs", ccs811.available, ccs811.eco2, ccs811.tvoc,
          "hdc", hdc1080.available, hdc1080.temperature, hdc1080.humidity)
end)
