#ifndef _MODULE_MACROS_ENGINE_h
#define _MODULE_MACROS_ENGINE_h

// ============================================================
// module_macros_engine.h — данные и интерфейс исполнительной машины
// сценариев (Lua 5.4 через EspLuaEngine + cron/ccronexpr).
// Реализация: module_macros_engine.cpp
// ============================================================

#include "main.h"

#include "ccronexpr.h"
#include "EspLuaEngine/EspLuaEngine.h"

// Ограничения прототипа (модуль предназначен только для ESP32)
#define MACRO_MAX_FILES       16     ///< максимум файлов-сценариев в списке
#define MACRO_MAX_ENTS        24     ///< максимум сущностей в одном файле
#define MACRO_EV_QUEUE        8      ///< размер очереди внешних событий (term/button)

// Защита от «зависаний» сценария: счётчик VM-инструкций Lua (аналог
// старого лимита TCL_MAX_STEPS=100000 в pTcl).
#define MACRO_LUA_MAX_OPS     100000 ///< максимум инструкций Lua на один вызов
#define MACRO_LUA_HOOK_N      1000   ///< шаг срабатывания count-hook (инструкций)

/// Типы сущностей сценария
#define MACRO_ENT_CRON        0      ///< «момент времени» по cron-выражению
#define MACRO_ENT_COND        1      ///< «условие» по фронту false->true
#define MACRO_ENT_BUTTON      2      ///< событие с веб-страницы / внешнего вызова (macro btn)
#define MACRO_ENT_TERM        3      ///< событие из терминала (macro msg)

/// Внешнее событие (очередь term/button)
typedef struct {
    uint8_t type;            ///< MACRO_ENT_BUTTON или MACRO_ENT_TERM
    String  spec;            ///< спецификатор (может содержать несколько слов-параметров)
} MacroEvent;

struct MacroFile;

/// Контекст разбора/исполнения Lua-файла. Указатель на этот контекст
/// передаётся в команды сценария как userdata-upvalue (см. macroLuaReg*).
typedef struct {
    struct MacroFile* file;  ///< файл, которому принадлежит интерпретатор
    bool parsing;            ///< true — идёт разбор файла (регистрация сущностей)
} LuaMacroCtx;

/// Одна сущность сценария (строка «таблицы условий и моментов времени»)
typedef struct {
    uint8_t   type;          ///< MACRO_ENT_*
    String    spec;          ///< cron-выражение / слова term|button (для cond не используется)
    int       bodyRef;       ///< ссылка LUA_REGISTRYINDEX на функцию-тело
    int       condRef;       ///< ссылка LUA_REGISTRYINDEX на условие (только MACRO_ENT_COND), иначе LUA_NOREF
    cron_expr expr;          ///< разобранное cron-выражение (для MACRO_ENT_CRON)
    time_t    next;          ///< следующее срабатывание cron (0 — не инициализировано)
    bool      lastCond;      ///< предыдущее состояние условия (для MACRO_ENT_COND)
} MacroEntity;

/// Файл-сценарий: метаданные (сохраняются в JSON) + runtime-состояние
typedef struct MacroFile {
    // --- метаданные (config_macros.json) ---
    String    name;          ///< полный путь: /macros/xxx.lua
    uint8_t   prio;          ///< приоритет 0..7 (0 — высший)
    bool      run;           ///< включён пользователем
    uint32_t  created;       ///< время создания (локальное, TimeLib)

    // --- runtime-состояние (не сохраняется) ---
    bool      active;        ///< файл запущен и успешно разобран
    String    err;           ///< текст последней ошибки (пусто — ошибок нет)
    EspLuaEngine* lua;       ///< интерпретатор Lua файла (только когда active)
    LuaMacroCtx  ctx;        ///< контекст команд Lua (ctx.file указывает на этот файл)
    MacroEntity ents[MACRO_MAX_ENTS];
    uint8_t   nEnts;         ///< число сущностей в ents
} MacroFile;

/// Структура конфига — сохраняется в config_macros.json
typedef struct {
    bool enabled;            ///< модуль включён
} strMacrosConfig;

#endif // _MODULE_MACROS_ENGINE_h
