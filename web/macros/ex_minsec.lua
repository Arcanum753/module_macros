-- Демо: одна иголочка = один cron (в мета-конфиге).
return {
    desc = "at 42:15 every hour",
    handlers = {
        tick = function(ev) puts(clock(), "fired:", ev.type, ev.spec) end,
    },
    rules = {
        { run = "tick" },
    },
}
