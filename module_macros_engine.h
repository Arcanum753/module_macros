#ifndef _MODULE_MACROS_ENGINE_h
#define _MODULE_MACROS_ENGINE_h

// ============================================================
// module_macros_engine.h — декларативные данные «барабана» сценариев:
// один Lua-интерпретатор, файл = Lua-чанк (тело тикает раз в секунду),
// ресурсные namespace-прокси поверх core_state, планировщик cron/every.
// Реализация: module_macros_engine.cpp
// ============================================================

#include "main.h"

#include "ccronexpr.h"
#include "EspLuaEngine/EspLuaEngine.h"
#include "core_state/core_state_types.h"

// Ограничения (модуль предназначен только для ESP32).
// Бюджет heap модуля — MACRO_HEAP_BUDGET (100 КБ). Оценка: фиксированная
// часть (один lua_State, MacroFile[40]) плюс активные файлы
// (chunk/env/state + массив обработчиков). Проверка — /macros/heap.
#define MACRO_MAX_FILES       40     ///< максимум файлов-сценариев в списке
#define MACRO_MAX_RUNNING     12     ///< максимум одновременно запущенных сценариев
#define MACRO_MAX_HANDLERS    8      ///< максимум обработчиков в одном файле
#define MACRO_EV_QUEUE        8      ///< размер очереди внешних событий (term/button/on)
#define MACRO_MAX_EVENT_SUBS  8      ///< максимум уникальных событий шины в подписке

#define MACRO_HEAP_BUDGET     (100UL * 1024) ///< бюджет heap модуля, байт
#define MACRO_HEAP_CRIT_PCT   95             ///< критический порог, % от бюджета

// Защита от «зависаний» сценария: счётчик VM-инструкций Lua.
#define MACRO_LUA_MAX_OPS     100000 ///< максимум инструкций Lua на один вызов
#define MACRO_LUA_HOOK_N      1000   ///< шаг срабатывания count-hook (инструкций)

// Минимальный свободный heap для ЛЮБОЙ работы с Lua (разбор/исполнение). Если
// памяти меньше — операция откладывается/отклоняется, но устройство не падает:
// незащищённые аллокации Lua вне pcall вызывают abort().
#define MACRO_LUA_MIN_HEAP    (24UL * 1024) ///< порог heap для запуска Lua, байт

// Регистрация файлов: не более N за тик и не более M авто-повторов.
#define MACRO_PARSE_PER_TICK  2      ///< максимум регистраций за тик
#define MACRO_PARSE_MAX_RETRY 3      ///< максимум авто-повторов после OOM
#define MACRO_BODY_PER_TICK   6      ///< максимум тел сценариев, исполняемых за тик (round-robin)

// Минимальная «правдоподобная» метка времени создания файла (2001-09-09).
#define MACRO_CREATED_MIN     1000000000UL

// Скачок времени вперёд больше порога (NTP-синк, смена TZ, ручной time.set):
// расписания пропускают «прошлые» срабатывания (пересчёт next без выполнения).
#define MACRO_CRON_JUMP_MAX_S 120

/// Типы обработчиков файла (per-file registry)
#define MACRO_H_CRON      0   ///< every("cron-выражение", fn)
#define MACRO_H_INTERVAL  1   ///< every(N, fn) — каждые N секунд
#define MACRO_H_AFTER     2   ///< after(N, fn) — один раз через N секунд
#define MACRO_H_ONCE      3   ///< once(fn) — на следующем тике
#define MACRO_H_TICK      4   ///< tick(fn) — каждый тик (в дополнение к телу)
#define MACRO_H_TERM      5   ///< term(spec, fn) — терминал "macro msg"
#define MACRO_H_BUTTON    6   ///< button(name, fn) — "macro btn"
#define MACRO_H_ON        7   ///< on(event, fn) — событие ресурсной шины

/// Один обработчик файла: зарегистрированная Lua-функция + расписание/ключ.
typedef struct {
    uint8_t  type;      ///< MACRO_H_*
    String   key;       ///< pattern / секунды / callsite / spec / имя события
    int      fnRef;     ///< ссылка Lua registry на closure (LUA_NOREF = -1)
    cron_expr expr;     ///< разобранный cron (только MACRO_H_CRON)
    time_t   next;      ///< следующий момент, epoch (CRON; 0 — нет)
    uint32_t dueMs;     ///< следующий момент, millis() (INTERVAL/AFTER/ONCE)
} MacroHandler;

/// Внешнее событие (очередь term/button/on)
typedef struct {
    uint8_t  type;      ///< MACRO_H_TERM / MACRO_H_BUTTON / MACRO_H_ON
    String   spec;      ///< спецификатор (слова term/button или имя события)
    BusValue value;     ///< значение события шины (для on)
} MacroEvent;

/// Файл-сценарий: метаданные (сохраняются в JSON) + runtime-состояние
typedef struct MacroFile {
    // --- метаданные (config_macros.json) ---
    String    name;          ///< полный путь: /macros/xxx.lua
    uint8_t   prio;          ///< приоритет 0..7 (0 — высший)
    bool      run;           ///< включён пользователем
    uint32_t  created;       ///< время создания (локальное, TimeLib)
    uint32_t  size;          ///< размер файла на FS (кэш для веб-таблицы)

    // --- runtime-состояние (не сохраняется) ---
    bool      active;        ///< файл скомпилирован (чанк + окружение живы)
    String    err;           ///< текст последней ошибки (пусто — ошибок нет)
    String    desc;          ///< описание из meta{ desc = ... }
    bool      needParse;     ///< требуется (пере)компиляция файла
    uint8_t   parseFails;    ///< счётчик подряд неудачных компиляций (защита от OOM)

    int       chunkRef;      ///< Lua registry: скомпилированный чанк (LUA_NOREF = -1)
    int       envRef;        ///< Lua registry: окружение файла (_ENV)
    int       stateRef;      ///< Lua registry: персистентная таблица state

    MacroHandler* handlers;  ///< динамический массив обработчиков (heap)
    uint8_t   nHandlers;     ///< число зарегистрированных обработчиков
} MacroFile;

/// Структура конфига — сохраняется в config_macros.json
typedef struct {
    bool enabled;            ///< модуль включён
} strMacrosConfig;

// Валидация сценария без регистрации файла: компиляция + пробный прогон тела
// (побочные bus-действия и планирование подавлены). Возвращает true, если
// ошибок нет; err — первая ошибка. Стек Lua очищается.
bool macroValidateScript(EspLuaEngine& eng, const String& content, String& err);

// Регистрирует глобальные функции сценария (every/on/print/…) на интерпретаторе.
// Нужна для изолированного движка валидации (веб-задача не трогает _lua).
bool macroRegisterHelpers(EspLuaEngine& eng);

// Читает только описание сценария (meta{desc=...}) без запуска живого файла —
// для заполнения веб-таблицы у остановленных файлов. Прогон в изолированной
// песочнице с подавлением bus-действий. Стек Lua очищается.
bool macroReadScenarioMeta(EspLuaEngine& eng, const String& content,
                           String& desc, String& err);

#endif // _MODULE_MACROS_ENGINE_h
