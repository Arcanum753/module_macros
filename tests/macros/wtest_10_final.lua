-- WTEST-10 (20:29): финал — вернуть auto, сохранить, снимок, баннер END.
return {
    desc = "WTEST-10 20:29 final",
    handlers = {
        endb = function()
            print("set e7.mode auto rc=", set("e7.mode", "auto"))
            print("FINAL END",
                "conn=",     get("wifi.connected"),
                "ip=",       get("wifi.ip"),
                "mode=",     get("wifi.mode"),
                "target=",   get("wifi.target"),
                "slotname=", get("wifi.slot_name"),
                "tvalid=",   get("time.valid"),
                "tsrc=",     get("time.source"),
                "now=",      get("time.now_str"),
                "e7mode=",   get("e7.mode"),
                "e7fx=",     get("e7.effect"),
                "e7col=",    get("e7.color"))
            print("==== WTEST END ====")
        end,
    },
    rules = {
        { when = { cron = "0 29 20 * * *" }, calls = {
            { name = "wifi.mode", args = { "auto" } },
            { name = "wifi.save" },
        } },
        { when = { cron = "0 29 20 * * *" }, run = "endb" },
    },
}
