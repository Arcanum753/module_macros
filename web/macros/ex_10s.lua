-- Демо: одна иголочка = один cron. Cron вынесен в мета-конфиг (колонка cron).
-- Правило без when исполняется по открытию мета-окна.
return {
    desc = "every 10s",
    handlers = {
        tick = function(ev) puts(clock(), "fired:", ev.type, ev.spec) end,
    },
    rules = {
        { run = "tick" },
    },
}
