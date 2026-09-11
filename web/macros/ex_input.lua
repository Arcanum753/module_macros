-- Демо: события терминала и кнопки (macro msg / macro btn).
return {
    desc = "term/button input demo",
    rules = {
        { term = "temp 25", body = function()
            puts("sensor: temperature 25")
        end },
        { term = "button1 on", body = function()
            puts("button1 on")
        end },
        { button = "start", body = function()
            puts("button start pressed")
        end },
    }
}
