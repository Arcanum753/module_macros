-- WTEST-04 (19:42): визуальный перебор цвета цифр (effect=mono) — каждые 2 мин.
meta{ desc = "WTEST-04 19:42 colors A" }

every("0 42 19 * * *", function() e7.color(16711680) end)
every("0 44 19 * * *", function() e7.color(65280) end)
every("0 46 19 * * *", function() e7.color(255) end)
every("0 48 19 * * *", function() e7.color(16776960) end)
every("0 50 19 * * *", function() e7.color(16711935) end)
every("0 52 19 * * *", function() e7.color(65535) end)
every("0 54 19 * * *", function() e7.color(16777215) end)
every("0 56 19 * * *", function() e7.color(16711680) end)
