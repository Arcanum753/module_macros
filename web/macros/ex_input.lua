-- Демо: правила ввода с параметрами из терминала.
-- Примеры:
--   macro run ex_input.lua
--   macro msg temp 25      - сработает term("temp 25", ...)
--   macro msg button1 on   - сработает term("button1 on", ...)
--   macro btn start        - сработает button("start", ...)
-- При совпадении параметров модуль печатает «условие ... сработало»
-- и выполняет тело правила в интерпретаторе этого файла.

local function report(msg)
    puts(clock())
    puts(msg)
end

term("temp 25", function()
    report("sensor: temperature 25")
end)

term("button1 on", function()
    report("button1 is ON")
end)

button("start", function()
    report("button start pressed")
end)
