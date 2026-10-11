-- WTEST-05 (19:58): ещё цвета + перебор эффектов.
meta{ desc = "WTEST-05 19:58 colors B + effects" }

every("0 58 19 * * *", function() e7.color(16744448) end)
every("0 0 20 * * *",  function() e7.color(8388736) end)
every("0 2 20 * * *",  function() e7.effect("rainbow") end)
every("0 4 20 * * *",  function() e7.effect("grad_static") end)
every("0 6 20 * * *",  function() e7.effect("grad_dynamic") end)
every("0 8 20 * * *",  function() e7.effect("colorcycle") end)
every("0 10 20 * * *", function() e7.effect("mono") end)
