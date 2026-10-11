-- подписка на событие шины + реакция на rssi через state (фронты).
meta{ desc = "on wifi.just_connected + rssi edge via state" }

on("wifi.just_connected", function(ev)
    print("connected:", ev.spec)
    e7.speed(40)
end)

-- тело файла исполняется каждый тик: храним прошлое состояние в state.
if wifi.rssi < -70 then
    if state.weak ~= true then
        state.weak = true
        e7.brightness = 10
    end
else
    if state.weak ~= false then
        state.weak = false
        e7.brightness = 20
    end
end
