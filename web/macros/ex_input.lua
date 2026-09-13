-- Демонстрация: события терминала и кнопки — полностью декларативно.
return {
    desc = "term/button -> set resource",
    rules = {
        { when = { term = "temp 25" },    set = "e7.effect",     value = 1 },
        { when = { term = "button1 on" }, set = "e7.brightness", value = 5 },
        { when = { button = "start" },    call = "e7.speed",     args = { 50 } },
    },
}
