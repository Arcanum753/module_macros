-- события терминала и кнопки (полное совпадение спецификатора).
meta{ desc = "term/button events" }

term("temp 25", function()
    e7.effect = "mono"
end)

term("button1 on", function()
    e7.brightness = 5
end)

button("start", function()
    e7.speed(50)
end)
