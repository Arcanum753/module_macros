-- Демонстрация: подписка на событие шины + cond с числовым оператором.
return {
    desc = "on wifi.just_connected + cond rssi",
    rules = {
        { when = { on = "wifi.just_connected" },
          call = "e7.speed", args = { 40 } },
        { when = { cond = { res = "wifi.rssi", op = "<", val = -70 } },
          set = "e7.brightness", value = 10 },
        { when = { cond = { res = "wifi.rssi", op = "changed" } },
          call = "e7.speed", args = { 20 } },
    },
}
