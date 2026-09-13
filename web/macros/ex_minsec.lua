-- Демонстрация: составной триггер — cron И условие ресурса.
-- Сработает в :42:15 каждого часа, только если Wi-Fi подключён.
return {
    desc = "at 42:15 hourly, if wifi.connected",
    rules = {
        { when = {
              cron = "15 42 * * * *",
              cond = { res = "wifi.connected", op = "==", val = true },
          },
          call = "e7.speed", args = { 25 } },
    },
}
