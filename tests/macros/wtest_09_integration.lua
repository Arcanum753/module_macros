-- WTEST-09 (20:28): сохранить e7-конфиг (пока e7 в macro), затем финал вернёт auto.
return {
    desc = "WTEST-09 20:28 e7.save",
    rules = {
        { when = { cron = "0 28 20 * * *" }, call = "e7.save" },
    },
}
