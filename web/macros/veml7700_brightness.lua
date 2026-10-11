-- Эталонный пример: тело файла исполняется раз в секунду.
-- Требует namespace veml7700 (модуль с полями available/lux).
-- Новый стиль: логика — обычный Lua-код в теле, дублирование записи
-- отсекает шина (e7.brightness — dual-ресурс), поэтому state не нужен.
meta{ desc = "night: brightness by veml7700 lux" }

-- Ночное окно 22:00..08:00, яркость по уровням освещённости.
if time.hour >= 22 or time.hour < 8 then
    if veml7700.available then
        local lux = veml7700.lux
        if lux < 20 then
            e7.brightness = 5
        elseif lux < 100 then
            e7.brightness = 30
        else
            e7.brightness = 80
        end
    end
end
