#ifndef _MODULE_MACROS_ENGINE_h
#define _MODULE_MACROS_ENGINE_h

// ============================================================
// module_macros_engine.h — данные и интерфейс «барабана» сценариев:
// один Lua-интерпретатор + декларативные правила (cron/cond/term/button/on/body).
// Реализация: module_macros_engine.cpp
// ============================================================

#include "main.h"

#include "ccronexpr.h"
#include "EspLuaEngine/EspLuaEngine.h"
#include "core_state/core_state_types.h"

// Ограничения (модуль предназначен только для ESP32).
// Бюджет heap модуля — MACRO_HEAP_BUDGET (100 КБ). Оценка: фиксированная
// часть (~33 КБ: один lua_State, MacroFile[30], кэш каталога, транзиент) плюс
// правила (sizeof(MacroRule) ~192 Б + строки на heap). Проверка — /macros/heap.
#define MACRO_MAX_FILES       30     ///< максимум файлов-сценариев в списке
#define MACRO_MAX_RULES       8      ///< максимум правил в одном файле
#define MACRO_MAX_ACTIONS     4      ///< максимум bus-вызовов в одном правиле
#define MACRO_CALL_MAX_ARGS   4      ///< максимум аргументов одного вызова
#define MACRO_MAX_CONDS       4      ///< максимум cond-условий в одном правиле (AND)
#define MACRO_EV_QUEUE        8      ///< размер очереди внешних событий (term/button/on)
#define MACRO_MAX_EVENT_SUBS  8      ///< максимум уникальных событий шины в подписке

#define MACRO_HEAP_BUDGET     (100UL * 1024) ///< бюджет heap модуля, байт
#define MACRO_HEAP_CRIT_PCT   95             ///< критический порог, % от бюджета

// Защита от «зависаний» сценария: счётчик VM-инструкций Lua.
#define MACRO_LUA_MAX_OPS     100000 ///< максимум инструкций Lua на один вызов
#define MACRO_LUA_HOOK_N      1000   ///< шаг срабатывания count-hook (инструкций)

// Регистрация правил: не более N файлов за тик и не более M авто-повторов.
#define MACRO_PARSE_PER_TICK  2      ///< максимум регистраций за тик
#define MACRO_PARSE_MAX_RETRY 3      ///< максимум авто-повторов после OOM

// Минимальная «правдоподобная» метка времени создания файла (2001-09-09).
#define MACRO_CREATED_MIN     1000000000UL

/// Типы правил
#define MACRO_RULE_CRON       0      ///< момент времени по cron-выражению
#define MACRO_RULE_COND       1      ///< условие (ресурс+оператор+значение), фронт false->true
#define MACRO_RULE_BUTTON     2      ///< кнопка с веб-страницы (macro btn)
#define MACRO_RULE_TERM       3      ///< команда терминала (macro msg)
#define MACRO_RULE_BODY       4      ///< тело без when (по открытию мета-окна)
#define MACRO_RULE_EVENT      5      ///< событие ресурсной шины (on="...")

/// Операторы условия cond
#define MACRO_COND_EQ         0
#define MACRO_COND_NE         1
#define MACRO_COND_LT         2
#define MACRO_COND_LE         3
#define MACRO_COND_GT         4
#define MACRO_COND_GE         5
#define MACRO_COND_CHANGED    6

/// Внешнее событие (очередь term/button/on)
typedef struct {
    uint8_t type;            ///< MACRO_RULE_BUTTON / MACRO_RULE_TERM / MACRO_RULE_EVENT
    String  spec;            ///< спецификатор (слова term/button или имя события)
} MacroEvent;

/// Одно cond-условие правила (структурно, без строковой сериализации)
typedef struct {
    String   res;            ///< "ns.field"
    uint8_t  op;             ///< MACRO_COND_*
    BusValue val;            ///< целевое значение (op != changed)
} MacroCond;

/// Одно правило файла (декларативное)
typedef struct {
    uint8_t   type;          ///< MACRO_RULE_* (первичный триггер или COND/BODY)
    String    spec;          ///< cron / term / button / имя события
    cron_expr expr;          ///< разобранный cron (для MACRO_RULE_CRON)
    time_t    next;          ///< следующее срабатывание cron (0 — не инициализировано)
    bool      lastCond;      ///< прошлое состояние (фронт для cond/body)

    // Составное условие when: 0..MACRO_MAX_CONDS cond-проверок (AND)
    MacroCond conds[MACRO_MAX_CONDS];
    uint8_t   nConds;
    String    lastVals[MACRO_MAX_CONDS]; ///< прошлые значения (для op=changed)
    bool      haveLast[MACRO_MAX_CONDS];

    // Действие: либо handler (Lua), либо setTarget, либо декларативные bus-вызовы
    String    setTarget;     ///< set="ns.field" (пусто, если действие call/calls/run)
    BusValue  setValue;      ///< значение для set (коэрсится к типу ресурса при вызове)
    String    handler;       ///< run="имя" — тяжёлый путь (перечитывает файл и компилирует Lua)
    String    handlerArgs;   ///< сериализованные аргументы handler "i:1|s:foo"
    String    actions[MACRO_MAX_ACTIONS]; ///< "ns.func|i:1|s:foo"
    uint8_t   nActions;

    uint32_t  sub;           ///< подписка на событие шины (только MACRO_RULE_EVENT)
} MacroRule;

/// Файл-сценарий: метаданные (сохраняются в JSON) + runtime-состояние
typedef struct MacroFile {
    // --- метаданные (config_macros.json) ---
    String    name;          ///< полный путь: /macros/xxx.lua
    uint8_t   prio;          ///< приоритет 0..7 (0 — высший)
    bool      run;           ///< включён пользователем
    uint32_t  created;       ///< время создания (локальное, TimeLib)
    String    metaCron;      ///< cron-выражение окна (из поля meta_cron файла; "" = без гейта)
    uint32_t  size;          ///< размер файла на FS (кэш для веб-таблицы)

    // --- runtime-состояние (не сохраняется) ---
    bool      active;        ///< файл зарегистрирован (правила разобраны)
    String    err;           ///< текст последней ошибки (пусто — ошибок нет)
    String    desc;          ///< описание из таблицы сценария
    bool      needParse;     ///< требуется (пере)регистрация правил
    uint8_t   parseFails;    ///< счётчик подряд неудачных регистраций (защита от OOM-цикла)
    MacroRule* rules;        ///< динамический массив правил (heap)
    uint8_t   nRules;        ///< число правил

    // Мета-cron (гейт окна)
    cron_expr metaExpr;
    bool      metaValid;     ///< cron разобран успешно
    bool      metaInit;      ///< next инициализирован
    time_t    metaNext;      ///< следующее срабатывание мета-cron
} MacroFile;

/// Структура конфига — сохраняется в config_macros.json
typedef struct {
    bool enabled;            ///< модуль включён
} strMacrosConfig;

// Отладочная сериализация условий правила (собирается на месте, не хранится).
String macroRuleCondStr(const MacroRule& r);
// Текстовое имя оператора cond (обратное к macroOpCode).
const char* macroCondOpName(uint8_t op);
// Текстовое имя типа правила (для printList).
const char* macroRuleTypeStr(uint8_t type);

#endif // _MODULE_MACROS_ENGINE_h
