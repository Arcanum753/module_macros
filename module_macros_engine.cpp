// ============================================================
// module_macros_engine.cpp — исполнительная машина сценариев
// (интерпретатор Lua 5.4 через EspLuaEngine + cron + таблица сущностей)
// ============================================================

#include "module_macros.h"

#include "core_ntp/NtpClientLib.h"
#include "common/TimeLib.h"

#include <stdio.h>
#include <string.h>
#include <new>

// Единый счётчик VM-инструкций для защиты от «зависаний» сценария.
// Безопасен, т.к. интерпретаторы исполняются строго последовательно
// в main-loop (разбор в тике/begin, тела в тике и событиях).
static uint32_t macroLuaSteps = 0;

// Форвард-объявления вспомогательных функций Lua-движка
static void macroLuaHook(lua_State* L, lua_Debug* ar);
static String macroLuaPopError(lua_State* L);
static int macroLuaRunChunk(MacroFile& f, const String& content);
static int macroLuaRegisterEntity(lua_State* L, uint8_t type);
static int macroLuaRegCron(lua_State* L);
static int macroLuaRegCond(lua_State* L);
static int macroLuaRegTerm(lua_State* L);
static int macroLuaRegButton(lua_State* L);
static int macroLuaPuts(lua_State* L);
static int macroLuaNow(lua_State* L);
static int macroLuaClock(lua_State* L);

// ============================================================
// Движок сценариев
// ============================================================

// Защитный hook: вызывается Lua каждые MACRO_LUA_HOOK_N инструкций VM.
/** Счётчик инструкций Lua для прерывания «зависших» сценариев.
  При превышении MACRO_LUA_MAX_OPS поднимает ошибку (обрабатывается pcall).
 \param L состояние
 \param ar отладочная информация (не используется)
 */
static void macroLuaHook(lua_State* L, lua_Debug* ar) {
    (void)ar;
    macroLuaSteps += MACRO_LUA_HOOK_N;
    if (macroLuaSteps >= MACRO_LUA_MAX_OPS) {
        luaL_error(L, "scenario step limit exceeded (%d ops)", MACRO_LUA_MAX_OPS);
    }
}

/** Забирает текст ошибки Lua с вершины стека и очищает стек.
 \param L состояние
 \return текст ошибки (может быть пустым)
 */
static String macroLuaPopError(lua_State* L) {
    String out;
    if (lua_gettop(L) == 0) { return out; }

    size_t len = 0;
    const char* s = lua_tolstring(L, -1, &len);
    if (s == NULL) {
        // Ошибка не является строкой — конвертируем (например таблица)
        luaL_tolstring(L, -1, &len);
        s = lua_tostring(L, -1);
    }
    if (s != NULL && len > 0) {
        out.reserve((unsigned int)len);
        for (size_t i = 0; i < len; i++) { out += s[i]; }
    }
    lua_settop(L, 0);
    return out;
}

/** Исполняет файл-сценарий в состоянии файла под защитой hook'а.
  Загружает содержимое как chunk (имя "@путь" для сообщений об ошибках)
  и вызывает его; регистрация правил происходит на верхнем уровне.
 \param f сценарий
 \param content текст сценария
 \return код возврата Lua (LUA_OK при успехе); текст ошибки — в f.err
 */
static int macroLuaRunChunk(MacroFile& f, const String& content) {
    lua_State* L = f.lua ? f.lua->getLuaState() : NULL;
    if (L == NULL) { f.err = "No interpreter"; return LUA_ERRRUN; }

    String cn = "@";
    cn += f.name;

    macroLuaSteps = 0;
    lua_sethook(L, macroLuaHook, LUA_MASKCOUNT, MACRO_LUA_HOOK_N);

    int status = luaL_loadbuffer(L, content.c_str(), content.length(), cn.c_str());
    if (status == LUA_OK) {
        status = lua_pcall(L, 0, 0, 0);
    }
    lua_sethook(L, NULL, 0, 0);

    if (status != LUA_OK) {
        f.err = macroLuaPopError(L);
        if (f.err.length() == 0) { f.err = "Scenario parse error"; }
        lua_settop(L, 0);
    } else {
        lua_settop(L, 0);
    }
    return status;
}

// ============================================================
// Команды Lua, регистрируемые в интерпретаторах сценариев
// ============================================================

/** Регистрирует сущность (cron/cond/term/button) в таблице сценария.
  Допустимо только во время разбора файла. Команды получают контекст
  файла как userdata-upvalue (передаётся через EspLuaEngine::registerFunction).
 \param L состояние Lua
 \param type тип сущности (MACRO_ENT_*)
 \return число результатов Lua (0)
 */
static int macroLuaRegisterEntity(lua_State* L, uint8_t type) {
    LuaMacroCtx* ctx = (LuaMacroCtx*)lua_touserdata(L, lua_upvalueindex(1));
    if (ctx == NULL || ctx->file == NULL) {
        return luaL_error(L, "no scenario context");
    }
    if (ctx->parsing == false) {
        return luaL_error(L, "rules can be registered only during file parse");
    }

    MacroFile* f = ctx->file;
    int narg = lua_gettop(L);

    String spec;
    if (type == MACRO_ENT_COND) {
        // cond(function-условие, function-тело)
        if (narg < 2) { return luaL_error(L, "usage: cond(function, function)"); }
        luaL_checktype(L, 1, LUA_TFUNCTION);
        luaL_checktype(L, 2, LUA_TFUNCTION);
    } else {
        // cron/term/button(specifier, function-тело)
        if (narg < 2) { return luaL_error(L, "usage: rule(specifier, function)"); }
        const char* s = luaL_checkstring(L, 1);
        luaL_checktype(L, 2, LUA_TFUNCTION);
        spec = String(s);
        spec.trim();
        if (spec.length() == 0) { return luaL_error(L, "empty specifier"); }
    }

    if (f->nEnts >= MACRO_MAX_ENTS) {
        return luaL_error(L, "file rule limit exceeded");
    }

    MacroEntity& e = f->ents[f->nEnts];
    e.type    = type;
    e.spec    = spec;
    e.bodyRef = LUA_NOREF;
    e.condRef = LUA_NOREF;
    e.lastCond = false;
    e.next     = 0;

    if (type == MACRO_ENT_CRON) {
        const char* perr = NULL;
        cron_expr cx;
        memset(&cx, 0, sizeof(cx));
        cron_parse_expr(spec.c_str(), &cx, &perr);
        if (perr) {
            return luaL_error(L, "cron expression error: %s (%s)", perr, spec.c_str());
        }
        e.expr = cx;
    }

    // Тело — аргумент 2 (функция)
    lua_pushvalue(L, 2);
    e.bodyRef = luaL_ref(L, LUA_REGISTRYINDEX);

    // Условие cond — аргумент 1 (функция)
    if (type == MACRO_ENT_COND) {
        lua_pushvalue(L, 1);
        e.condRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    f->nEnts++;
    DEBUGMACROS("[MACRO] reg: type=%d\r\n", (int)type);
    return 0;
}

static int macroLuaRegCron(lua_State* L)   { return macroLuaRegisterEntity(L, MACRO_ENT_CRON); }
static int macroLuaRegCond(lua_State* L)   { return macroLuaRegisterEntity(L, MACRO_ENT_COND); }
static int macroLuaRegTerm(lua_State* L)   { return macroLuaRegisterEntity(L, MACRO_ENT_TERM); }
static int macroLuaRegButton(lua_State* L) { return macroLuaRegisterEntity(L, MACRO_ENT_BUTTON); }

// puts/print — вывод в последовательный порт с префиксом [MACRO]
/** Функции Lua puts/print: выводят аргументы в терминал с префиксом [MACRO].
 \param L состояние Lua
 \return число результатов Lua (0)
 */
static int macroLuaPuts(lua_State* L) {
    int n = lua_gettop(L);
    Serial.printf("[MACRO] ");
    for (int i = 1; i <= n; i++) {
        bool pushed = false;
        size_t len = 0;
        const char* s = NULL;
        if (lua_isstring(L, i) || lua_isnumber(L, i)) {
            s = lua_tolstring(L, i, &len);
        } else {
            // Булевы/nil/таблицы — через luaL_tolstring (__tostring)
            s = luaL_tolstring(L, i, &len);
            pushed = true;
        }
        if (s != NULL && len > 0) {
            Serial.write((const uint8_t*)s, len);
        }
        if (pushed) { lua_pop(L, 1); }
        if (i < n) { Serial.print(" "); }
    }
    Serial.printf("\r\n");
    return 0;
}

// now — текущие секунды (локальное «наивное» время TimeLib)
/** Функция Lua now: возвращает текущие секунды (локальное время TimeLib).
 \param L состояние Lua
 \return число результатов Lua (1 — целые секунды)
 */
static int macroLuaNow(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)now());
    return 1;
}

// clock — текущее время в формате ЧЧ:ММ:СС
/** Функция Lua clock: возвращает текущее время в формате ЧЧ:ММ:СС.
 \param L состояние Lua
 \return число результатов Lua (1 — строка)
 */
static int macroLuaClock(lua_State* L) {
    time_t tnow = (time_t)now();
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
             (int)hour(tnow), (int)minute(tnow), (int)second(tnow));
    lua_pushstring(L, buf);
    return 1;
}

// ============================================================
// Методы класса (движок сценариев)
// ============================================================

/** Освобождает интерпретатор файла и очищает таблицу его сущностей.
 \param f сценарий
 */
void CLASS_MODULE_MACROS::destroyScript(MacroFile& f) {
    if (f.lua) {
        delete f.lua;
        f.lua = NULL;
    }
    for (uint8_t i = 0; i < f.nEnts; i++) {
        f.ents[i].spec    = "";
        f.ents[i].bodyRef = LUA_NOREF;
        f.ents[i].condRef = LUA_NOREF;
    }
    f.nEnts = 0;
    f.active = false;
    f.ctx.file = NULL;
    f.ctx.parsing = false;
}

/** Разбирает файл сценария: читает содержимое из FS, создаёт состояние Lua,
  регистрирует команды модуля, исполняет файл и заполняет таблицу сущностей.
 \param f сценарий (runtime-часть)
 \return true — разбор успешен и есть хотя бы одно правило
 */
bool CLASS_MODULE_MACROS::parseScript(MacroFile& f) {
    f.err = "";
    destroyScript(f);

    String content = readFile(f.name);
    if (content.length() == 0) {
        f.err = "File is empty or unreadable";
        DEBUGMACROS("%s: %s\r\n", __FUNCTION__, f.err.c_str());
        return false;
    }

    EspLuaEngine* eng = new (std::nothrow) EspLuaEngine();
    if (eng == NULL || eng->getLuaState() == NULL) {
        delete eng;
        f.err = "No memory for interpreter";
        DEBUGMACROS("%s: %s\r\n", __FUNCTION__, f.err.c_str());
        return false;
    }
    f.lua = eng;

    f.ctx.file = &f;
    f.ctx.parsing = false;

    // Регистрация команд и функций сценария в состояние файла.
    // Контекст (&f.ctx) передаётся как userdata-upvalue командам регистрации.
    bool reg =
        eng->registerFunction("cron",   macroLuaRegCron,   &f.ctx) &&
        eng->registerFunction("cond",   macroLuaRegCond,   &f.ctx) &&
        eng->registerFunction("term",   macroLuaRegTerm,   &f.ctx) &&
        eng->registerFunction("button", macroLuaRegButton, &f.ctx) &&
        eng->registerFunction("puts",   macroLuaPuts) &&
        eng->registerFunction("print",  macroLuaPuts) &&
        eng->registerFunction("clock",  macroLuaClock) &&
        eng->registerFunction("now",    macroLuaNow);
    if (reg == false) {
        f.err = "Lua init error";
        DEBUGMACROS("%s: %s\r\n", __FUNCTION__, f.err.c_str());
        destroyScript(f);
        return false;
    }

    // Исполнение файла по верхнему уровню: регистрация правил происходит
    // при вызовах cron/cond/term/button внутри chunk'а.
    f.ctx.parsing = true;
    int status = macroLuaRunChunk(f, content);
    f.ctx.parsing = false;

    if (status != LUA_OK) {
        if (f.err.length() == 0) { f.err = "Scenario parse error"; }
        DEBUGMACROS("%s: %s -> %s\r\n", __FUNCTION__, f.name.c_str(), f.err.c_str());
        destroyScript(f);
        return false;
    }

    if (f.nEnts == 0) {
        f.err = "No rules registered in the scenario";
        DEBUGMACROS("%s: %s -> %s\r\n", __FUNCTION__, f.name.c_str(), f.err.c_str());
        destroyScript(f);
        return false;
    }

    f.active = true;
    DEBUGMACROS("%s: %s -> %d ent\r\n", __FUNCTION__, f.name.c_str(), f.nEnts);
    return true;
}

/** Синхронизирует интерпретаторы с метой: останавливает удалённые/выключенные
  файлы и переразбирает запущенные.
 */
void CLASS_MODULE_MACROS::rebuildScripts() {
    for (uint8_t i = 0; i < _fileCount; i++) {
        MacroFile& f = _files[i];
        if (f.run == false) {
            destroyScript(f);
            continue;
        }
        if (_fs->exists(f.name) == false) {
            f.err = "File is missing";
            destroyScript(f);
            continue;
        }
        // Полная пересборка: разрушаем и разбираем заново
        parseScript(f);
    }
}

/** Выполняет функцию-тело правила в интерпретаторе файла (по ссылке registry).
 \param f сценарий
 \param bodyRef ссылка LUA_REGISTRYINDEX на функцию
 \return пустая строка при успехе, иначе текст ошибки
 */
String CLASS_MODULE_MACROS::runBody(MacroFile& f, int bodyRef) {
    if (f.lua == NULL || f.active == false) { return "No interpreter"; }
    if (bodyRef == LUA_NOREF) { return "No body function"; }

    lua_State* L = f.lua->getLuaState();
    macroLuaSteps = 0;
    lua_sethook(L, macroLuaHook, LUA_MASKCOUNT, MACRO_LUA_HOOK_N);

    lua_rawgeti(L, LUA_REGISTRYINDEX, bodyRef);
    int status = lua_pcall(L, 0, 0, 0);
    lua_sethook(L, NULL, 0, 0);

    String errTxt;
    if (status != LUA_OK) {
        errTxt = macroLuaPopError(L);
        if (errTxt.length() == 0) { errTxt = "Scenario execution error"; }
        lua_settop(L, 0);
    } else {
        lua_settop(L, 0);
    }
    return errTxt;
}

// Выполнение тела с записью ошибки в файл (метод класса: доступ к runBody)
/** Выполняет тело сущности и записывает ошибку в f.err (с выводом в журнал).
 \param f сценарий
 \param e сущность (cron/cond/term/button)
 */
void CLASS_MODULE_MACROS::execBody(MacroFile& f, MacroEntity& e) {
    String err = runBody(f, e.bodyRef);
    if (err.length() > 0) {
        f.err = err;
        DEBUGMACROS("[MACRO] %s exec error: %s\r\n", f.name.c_str(), err.c_str());
    }
}

// Вычисление Lua-условия cond; возвращает истину/ложь, при ошибке пишет текст в errTxt
/** Вычисляет условие cond-правила (функция из condRef).
 \param f сценарий
 \param e сущность cond
 \param errTxt текст ошибки (пуст при успехе)
 \return истинность условия
 */
bool CLASS_MODULE_MACROS::evalCondEntity(MacroFile& f, MacroEntity& e, String& errTxt) {
    errTxt = "";
    if (f.lua == NULL || f.active == false) {
        errTxt = "No interpreter";
        return false;
    }
    if (e.condRef == LUA_NOREF) {
        errTxt = "No condition function";
        return false;
    }

    lua_State* L = f.lua->getLuaState();
    macroLuaSteps = 0;
    lua_sethook(L, macroLuaHook, LUA_MASKCOUNT, MACRO_LUA_HOOK_N);

    lua_rawgeti(L, LUA_REGISTRYINDEX, e.condRef);
    int status = lua_pcall(L, 0, 1, 0);
    lua_sethook(L, NULL, 0, 0);

    if (status != LUA_OK) {
        errTxt = macroLuaPopError(L);
        if (errTxt.length() == 0) { errTxt = "Condition evaluation error"; }
        lua_settop(L, 0);
        return false;
    }

    bool truth = (lua_toboolean(L, -1) != 0);
    lua_settop(L, 0);
    return truth;
}

/** Обрабатывает очередь внешних событий: сопоставляет term/button-правила
  запущенных файлов и выполняет их тела.
 */
void CLASS_MODULE_MACROS::drainEvents() {
    while (_evIn != _evOut) {
        MacroEvent ev = _evQueue[_evOut];
        _evOut = (_evOut + 1) % MACRO_EV_QUEUE;

        for (uint8_t i = 0; i < _fileCount; i++) {
            MacroFile& f = _files[i];
            if (f.run == false || f.active == false) { continue; }
            for (uint8_t j = 0; j < f.nEnts; j++) {
                MacroEntity& e = f.ents[j];
                if (e.type == ev.type && e.spec == ev.spec) {
                    const char* tn = (ev.type == MACRO_ENT_TERM) ? "term" : "button";
                    Serial.printf("[MACRO] condition %s \"%s\" fired\r\n", tn, ev.spec.c_str());
                    execBody(f, e);
                }
            }
        }
    }
}

/** Один шаг исполнительной машины (раз в секунду): при изменении списка
  пересобирает сценарии, затем обрабатывает события и исполняет cron/cond-правила.
 */
void CLASS_MODULE_MACROS::tickStep() {
    // Пересборка интерпретаторов при изменении списка
    if (_scriptRev != _lastScriptRev) {
        _lastScriptRev = _scriptRev;
        rebuildScripts();
    }

    if (_config.enabled == false) { return; }

    // Внешние события (term/button)
    drainEvents();

    bool ntpSync = (NTP.getLastNTPSync() > 0);
    if (ntpSync && !_ntpWasSynced) {
        _ntpWasSynced = true;
        DEBUGMACROS("[MACRO] NTP synced, cron activated\r\n");

        // Файлы, добавленные до синхронизации времени (created == 0),
        // получают реальную дату создания
        bool changed = false;
        for (uint8_t i = 0; i < _fileCount; i++) {
            if (_files[i].created == 0) {
                _files[i].created = (uint32_t)now();
                changed = true;
            }
        }
        if (changed) { saveConfig(); }
    }

    // Проход по запущенным файлам и их сущностям
    for (uint8_t i = 0; i < _fileCount; i++) {
        MacroFile& f = _files[i];
        if (f.run == false || f.active == false) { continue; }

        for (uint8_t j = 0; j < f.nEnts; j++) {
            MacroEntity& e = f.ents[j];

            if (e.type == MACRO_ENT_CRON) {
                if (ntpSync == false) { continue; }

                if (e.next == 0) {
                    // Инициализация следующего момента после синхронизации времени
                    time_t nx = cron_next(&e.expr, (time_t)now());
                    e.next = nx;
                    continue;
                }
                if (e.next == (time_t)-1) { continue; } // расписание не имеет ближайших моментов

                if ((time_t)now() >= e.next) {
                    DEBUGMACROS("[MACRO] %s cron(%s)\r\n", f.name.c_str(), e.spec.c_str());
                    execBody(f, e);
                    e.next = cron_next(&e.expr, (time_t)now());
                }
            }

            if (e.type == MACRO_ENT_COND) {
                String errTxt;
                bool truth = evalCondEntity(f, e, errTxt);
                if (errTxt.length() > 0) {
                    f.err = errTxt;
                    DEBUGMACROS("[MACRO] %s cond error: %s\r\n", f.name.c_str(), errTxt.c_str());
                    continue;
                }
                if (truth && e.lastCond == false) {
                    // Фронт false -> true
                    DEBUGMACROS("[MACRO] %s cond(true)\r\n", f.name.c_str());
                    e.lastCond = true;
                    execBody(f, e);
                } else if (truth == false) {
                    e.lastCond = false;
                }
            }
        }
    }
}
