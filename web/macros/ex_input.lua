-- Демо: события терминала и кнопки (macro msg / macro btn).
-- Мета-cron пуст — сценарий активен всегда.
return {
    desc = "term/button input demo (no meta window)",
    handlers = {
        temp  = function(ev) puts("sensor: temperature 25") end,
        b1    = function(ev) puts("button1 on") end,
        start = function(ev) puts("button start pressed") end,
    },
    rules = {
        { term = "temp 25", run = "temp" },
        { term = "button1 on", run = "b1" },
        { button = "start", run = "start" },
    },
}
