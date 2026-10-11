-- WTEST-09 (20:28): сохранить e7-конфиг (пока e7 в macro), затем финал вернёт auto.
meta{ desc = "WTEST-09 20:28 e7.save" }

every("0 28 20 * * *", function() e7.save() end)
