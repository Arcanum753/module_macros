-- WTEST-04 (19:42): визуальный перебор цвета цифр (effect=mono) — каждые 2 мин.
return {
    desc = "WTEST-04 19:42 colors A",
    rules = {
        { when = { cron = "0 42 19 * * *" }, call = "e7.color", args = { 16711680 } },
        { when = { cron = "0 44 19 * * *" }, call = "e7.color", args = { 65280 } },
        { when = { cron = "0 46 19 * * *" }, call = "e7.color", args = { 255 } },
        { when = { cron = "0 48 19 * * *" }, call = "e7.color", args = { 16776960 } },
        { when = { cron = "0 50 19 * * *" }, call = "e7.color", args = { 16711935 } },
        { when = { cron = "0 52 19 * * *" }, call = "e7.color", args = { 65535 } },
        { when = { cron = "0 54 19 * * *" }, call = "e7.color", args = { 16777215 } },
        { when = { cron = "0 56 19 * * *" }, call = "e7.color", args = { 16711680 } },
    },
}
