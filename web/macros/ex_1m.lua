-- Демо: одна иголочка = один cron (в мета-конфиге).
return {
    desc = "every minute",
    handlers = {
        tick = function(ev) puts(clock(), "fired:", ev.type, ev.spec) end,
    },
    rules = {
        { run = "tick" },
    },
}
