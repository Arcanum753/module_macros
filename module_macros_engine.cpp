// ============================================================
// module_macros_engine.cpp — исполнительная машина сценариев
// (интерпретатор Lua 5.4 через EspLuaEngine + cron + таблица сущностей)
// ============================================================

#include "module_macros.h"

#include "core_state/core_state.h"
#include "core_state/common_module.h"
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
static int macroLuaGet(lua_State* L);
static int macroLuaSet(lua_State* L);
static int macroLuaCall(lua_State* L);
static int macroLuaCallAsync(lua_State* L);
static int macroLuaOn(lua_State* L);
static int macroLuaOff(lua_State* L);
static int macroLuaHas(lua_State* L);
static int macroLuaMode(lua_State* L);
static int macroLuaInfo(lua_State* L);
static int macroLuaPuts(lua_State* L);
static int macroLuaNow(lua_State* L);
static int macroLuaClock(lua_State* L);
static int macroLuaEventTramp(void* user, int argc, const BusValue* argv, BusValue& result);
static int macroLuaAsyncTramp(void* user, int argc, const BusValue* argv, BusValue& result);

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
  и вызывает его; возвращаемое значение (функция или таблица) остаётся на стеке.
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
        // Возвращаемое значение сценария (функция или таблица) остаётся на стеке.
        status = lua_pcall(L, 0, 1, 0);
    }
    lua_sethook(L, NULL, 0, 0);

    if (status != LUA_OK) {
        f.err = macroLuaPopError(L);
        if (f.err.length() == 0) { f.err = "Scenario parse error"; }
        lua_settop(L, 0);
    }
    return status;
}

// ============================================================
// Мосты к ресурсной шине core_state
// ============================================================

static LuaMacroCtx* macroLuaCtx(lua_State* L) {
    return (LuaMacroCtx*)lua_touserdata(L, lua_upvalueindex(1));
}

static void macroPushBusValue(lua_State* L, const BusValue& v) {
    switch (v.kind) {
        case BusValue::BOOL: lua_pushboolean(L, v.b ? 1 : 0); break;
        case BusValue::I32:  lua_pushinteger(L, (lua_Integer)v.i); break;
        case BusValue::ENUM: lua_pushinteger(L, (lua_Integer)v.i); break;
        case BusValue::F32:  lua_pushnumber(L, (lua_Number)v.f); break;
        case BusValue::STR:  lua_pushstring(L, v.s.c_str()); break;
        case BusValue::TIME: lua_pushinteger(L, (lua_Integer)v.t); break;
        default:             lua_pushnil(L); break;
    }
}

static bool macroLuaToBusValue(lua_State* L, int idx, BusValue& out) {
    if (lua_isboolean(L, idx)) { out = BusValue::bo(lua_toboolean(L, idx) != 0); return true; }
    if (lua_isinteger(L, idx)) { out = BusValue::i32((int32_t)lua_tointeger(L, idx)); return true; }
    if (lua_isnumber(L, idx))  { out = BusValue::f32((float)lua_tonumber(L, idx)); return true; }
    if (lua_isstring(L, idx))  { out = BusValue::str(String(lua_tostring(L, idx))); return true; }
    return false;
}

// ============================================================
// Команды Lua, регистрируемые в интерпретаторах сценариев
// ============================================================

// get("ns.field") — чтение значения ресурса
static int macroLuaGet(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    if (core_state.has(name) == false) { lua_pushnil(L); return 1; }
    BusResInfo info;
    core_state.info(name, info);
    switch (info.kind) {
        case BusValue::BOOL: macroPushBusValue(L, BusValue::bo(core_state.getBool(name))); break;
        case BusValue::I32:  macroPushBusValue(L, BusValue::i32(core_state.getInt(name))); break;
        case BusValue::ENUM: macroPushBusValue(L, BusValue::en(core_state.getInt(name))); break;
        case BusValue::F32:  macroPushBusValue(L, BusValue::f32(core_state.getF32(name))); break;
        case BusValue::STR:  macroPushBusValue(L, BusValue::str(core_state.getStr(name))); break;
        case BusValue::TIME: macroPushBusValue(L, BusValue::tm(core_state.getTime(name))); break;
        default: lua_pushnil(L); break;
    }
    return 1;
}

// set("ns.field", value) — запись значения, возвращает код
static int macroLuaSet(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    if (core_state.has(name) == false) { lua_pushinteger(L, BUS_ERR_NOT_FOUND); return 1; }
    BusResInfo info;
    core_state.info(name, info);
    BusValue v;
    if (lua_gettop(L) >= 2) { macroLuaToBusValue(L, 2, v); }

    // Для функций set() вызывает функцию (применяет значение), а не пишет состояние.
    int rc = BUS_ERR_BAD_TYPE;
    if (info.isFunc && !info.async) {
        BusValue res;
        rc = core_state.call(name, 1, &v, res);
        lua_pushinteger(L, rc);
        return 1;
    }

    switch (info.kind) {
        case BusValue::BOOL: rc = core_state.setBool(name, (v.kind == BusValue::BOOL) ? v.b : (v.i != 0)); break;
        case BusValue::I32:  rc = core_state.setInt(name, (int32_t)v.i); break;
        case BusValue::ENUM: rc = core_state.setInt(name, (int32_t)v.i); break;
        case BusValue::F32:  rc = core_state.setF32(name, v.f); break;
        case BusValue::STR:  rc = core_state.setStr(name, v.s); break;
        case BusValue::TIME: rc = core_state.setTime(name, v.t); break;
        default: break;
    }
    lua_pushinteger(L, rc);
    return 1;
}

// call("ns.func") — синхронный вызов; значение или (nil, rc)
static int macroLuaCall(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    int narg = lua_gettop(L) - 1;
    if (narg > CORE_STATE_MAX_ARGS) { narg = CORE_STATE_MAX_ARGS; }
    BusValue args[CORE_STATE_MAX_ARGS];
    int argc = 0;
    for (int i = 0; i < narg; i++) {
        if (macroLuaToBusValue(L, 2 + i, args[argc])) { argc++; }
    }
    BusValue result;
    int rc = core_state.call(name, argc, (argc > 0) ? args : nullptr, result);
    if (rc == BUS_OK) {
        macroPushBusValue(L, result);
        return 1;
    }
    lua_pushnil(L);
    lua_pushinteger(L, rc);
    return 2;
}

// call_async("ns.func", function(rc, value) ... end) — асинхронный вызов
static int macroLuaCallAsync(lua_State* L) {
    LuaMacroCtx* ctx = macroLuaCtx(L);
    if (ctx == NULL || ctx->file == NULL) { return luaL_error(L, "no scenario context"); }
    const char* name = luaL_checkstring(L, 1);
    if (!lua_isfunction(L, 2)) { return luaL_error(L, "usage: call_async(name, function)"); }

    MacroFile* f = ctx->file;
    if (f->asyncPending) { lua_pushinteger(L, BUS_ERR_BUSY); return 1; }

    core_state.setAsyncOwner(f);
    int rc = core_state.call_async(name, macroLuaAsyncTramp, f, 0, nullptr);
    if (rc == BUS_OK) {
        lua_pushvalue(L, 2);
        f->asyncCbRef = luaL_ref(L, LUA_REGISTRYINDEX);
        f->asyncPending = true;
    }
    lua_pushinteger(L, rc);
    return 1;
}

// on("evt", function(name, value) ... end) — подписка на событие
static int macroLuaOn(lua_State* L) {
    LuaMacroCtx* ctx = macroLuaCtx(L);
    if (ctx == NULL || ctx->file == NULL) { return luaL_error(L, "no scenario context"); }
    const char* evt = luaL_checkstring(L, 1);
    if (!lua_isfunction(L, 2)) { return luaL_error(L, "usage: on(event, function)"); }

    MacroFile* f = ctx->file;
    if (f->nSubs >= MACRO_MAX_SUBS) { lua_pushinteger(L, 0); return 1; }

    uint32_t h = core_state.on(evt, macroLuaEventTramp, f);
    if (h == 0) { lua_pushinteger(L, 0); return 1; }

    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    f->subs[f->nSubs] = h;
    f->subEvts[f->nSubs] = String(evt);
    f->subRefs[f->nSubs] = ref;
    f->nSubs++;

    lua_pushinteger(L, (lua_Integer)h);
    return 1;
}

// off(handle) — снять подписку
static int macroLuaOff(lua_State* L) {
    LuaMacroCtx* ctx = macroLuaCtx(L);
    if (ctx == NULL || ctx->file == NULL) { return luaL_error(L, "no scenario context"); }
    uint32_t h = (uint32_t)luaL_checkinteger(L, 1);

    MacroFile* f = ctx->file;
    core_state.off(h);
    for (uint8_t i = 0; i < f->nSubs; i++) {
        if (f->subs[i] == h) {
            f->subs[i] = 0;
            f->subEvts[i] = "";
            f->subRefs[i] = LUA_NOREF;
            for (uint8_t j = i; j + 1 < f->nSubs; j++) {
                f->subs[j] = f->subs[j + 1];
                f->subEvts[j] = f->subEvts[j + 1];
                f->subRefs[j] = f->subRefs[j + 1];
            }
            f->nSubs--;
            break;
        }
    }
    lua_pushboolean(L, 1);
    return 1;
}

// has("ns.field") — зарегистрирован ли ресурс
static int macroLuaHas(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    lua_pushboolean(L, core_state.has(name) ? 1 : 0);
    return 1;
}

// mode("namespace" [, value]) — чтение/запись режима модуля
static int macroLuaMode(lua_State* L) {
    const char* ns = luaL_checkstring(L, 1);
    if (lua_gettop(L) >= 2) {
        int m = (int)luaL_checkinteger(L, 2);
        lua_pushinteger(L, core_state.mode(ns, m));
        return 1;
    }
    lua_pushinteger(L, core_state.mode(ns, -1));
    return 1;
}

// info("ns.field") — метаданные ресурса (таблица или nil)
static int macroLuaInfo(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    BusResInfo info;
    if (core_state.info(name, info) != BUS_OK) { lua_pushnil(L); return 1; }

    lua_newtable(L);
    lua_pushstring(L, info.desc ? info.desc : "");
    lua_setfield(L, -2, "desc");
    lua_pushstring(L, ns_core_state::kindName(info.kind));
    lua_setfield(L, -2, "kind");
    lua_pushboolean(L, info.writable ? 1 : 0);
    lua_setfield(L, -2, "rw");
    lua_pushboolean(L, info.isEvent ? 1 : 0);
    lua_setfield(L, -2, "event");
    lua_pushboolean(L, info.isFunc ? 1 : 0);
    lua_setfield(L, -2, "func");
    lua_pushboolean(L, info.async ? 1 : 0);
    lua_setfield(L, -2, "async");
    if (info.enumCount > 0) {
        lua_newtable(L);
        for (uint8_t i = 0; i < info.enumCount; i++) {
            lua_pushstring(L, info.enumVals[i]);
            lua_rawseti(L, -2, (lua_Integer)(i + 1));
        }
        lua_setfield(L, -2, "enum");
    }
    return 1;
}

// puts/print — вывод в последовательный порт с префиксом [MACRO]
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

static int macroLuaNow(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)now());
    return 1;
}

static int macroLuaClock(lua_State* L) {
    time_t tnow = (time_t)now();
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
             (int)hour(tnow), (int)minute(tnow), (int)second(tnow));
    lua_pushstring(L, buf);
    return 1;
}

// ============================================================
// Разбор таблицы сценария
// ============================================================

static bool macroAddEntity(lua_State* L, MacroFile& f, uint8_t type, const String& spec,
                          int bodyIdx, int condIdx) {
    if (f.nEnts >= MACRO_MAX_ENTS) { return false; }

    MacroEntity& e = f.ents[f.nEnts];
    e.type = type;
    e.spec = spec;
    e.bodyRef = LUA_NOREF;
    e.condRef = LUA_NOREF;
    e.lastCond = false;
    e.next = 0;

    if (type == MACRO_ENT_CRON) {
        const char* perr = NULL;
        cron_expr cx;
        memset(&cx, 0, sizeof(cx));
        cron_parse_expr(spec.c_str(), &cx, &perr);
        if (perr) { f.err = String("cron error: ") + perr; return false; }
        e.expr = cx;
    }

    lua_pushvalue(L, bodyIdx);
    e.bodyRef = luaL_ref(L, LUA_REGISTRYINDEX);
    if (type == MACRO_ENT_COND) {
        lua_pushvalue(L, condIdx);
        e.condRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    f.nEnts++;
    return true;
}

// Разбор одного правила из таблицы rules[i]: {cron=|cond=|term=|button=, body=}
static void macroParseRule(lua_State* L, MacroFile& f, int ruleIdx) {
    ruleIdx = lua_absindex(L, ruleIdx);

    lua_getfield(L, ruleIdx, "body");
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }
    int bodyIdx = lua_absindex(L, -1);
    bool added = false;

    lua_getfield(L, ruleIdx, "cron");
    if (lua_isstring(L, -1)) {
        added = macroAddEntity(L, f, MACRO_ENT_CRON, String(lua_tostring(L, -1)), bodyIdx, -1);
    }
    lua_pop(L, 1);

    if (!added) {
        lua_getfield(L, ruleIdx, "term");
        if (lua_isstring(L, -1)) {
            added = macroAddEntity(L, f, MACRO_ENT_TERM, String(lua_tostring(L, -1)), bodyIdx, -1);
        }
        lua_pop(L, 1);
    }
    if (!added) {
        lua_getfield(L, ruleIdx, "button");
        if (lua_isstring(L, -1)) {
            added = macroAddEntity(L, f, MACRO_ENT_BUTTON, String(lua_tostring(L, -1)), bodyIdx, -1);
        }
        lua_pop(L, 1);
    }
    if (!added) {
        lua_getfield(L, ruleIdx, "cond");
        if (lua_isfunction(L, -1)) {
            added = macroAddEntity(L, f, MACRO_ENT_COND, "", bodyIdx, lua_absindex(L, -1));
        }
        lua_pop(L, 1);
    }

    lua_pop(L, 1); // body
}

// ============================================================
// Методы класса (движок сценариев)
// ============================================================

/** Освобождает интерпретатор файла и очищает его сущности/подписки.
 \param f сценарий
 */
void CLASS_MODULE_MACROS::destroyScript(MacroFile& f) {
    if (f.lua) {
        for (uint8_t i = 0; i < f.nSubs; i++) {
            if (f.subs[i] != 0) { core_state.off(f.subs[i]); }
        }
        core_state.asyncCancelFor(&f);
        delete f.lua;
        f.lua = NULL;
    }
    for (uint8_t i = 0; i < f.nEnts; i++) {
        f.ents[i].spec    = "";
        f.ents[i].bodyRef = LUA_NOREF;
        f.ents[i].condRef = LUA_NOREF;
    }
    f.nEnts = 0;
    f.nSubs = 0;
    f.asyncPending = false;
    f.asyncCbRef = LUA_NOREF;
    f.active = false;
    f.ctx.file = NULL;
    f.ctx.parsing = false;
}

/** Разбирает файл сценария: читает содержимое, создаёт состояние Lua,
  регистрирует команды, исполняет файл и заполняет таблицу сущностей
  из возвращённой функции/таблицы.
 \param f сценарий (runtime-часть)
 \return true — разбор успешен и есть хотя бы одно правило
 */
bool CLASS_MODULE_MACROS::parseScript(MacroFile& f) {
    f.err = "";
    f.desc = "";
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

    bool reg =
        eng->registerFunction("get",        macroLuaGet,      &f.ctx) &&
        eng->registerFunction("set",        macroLuaSet,      &f.ctx) &&
        eng->registerFunction("call",       macroLuaCall,     &f.ctx) &&
        eng->registerFunction("call_async", macroLuaCallAsync, &f.ctx) &&
        eng->registerFunction("on",         macroLuaOn,       &f.ctx) &&
        eng->registerFunction("off",        macroLuaOff,      &f.ctx) &&
        eng->registerFunction("has",        macroLuaHas,      &f.ctx) &&
        eng->registerFunction("mode",       macroLuaMode,     &f.ctx) &&
        eng->registerFunction("info",       macroLuaInfo,     &f.ctx) &&
        eng->registerFunction("puts",       macroLuaPuts) &&
        eng->registerFunction("print",      macroLuaPuts) &&
        eng->registerFunction("clock",      macroLuaClock) &&
        eng->registerFunction("now",        macroLuaNow);
    if (reg == false) {
        f.err = "Lua init error";
        DEBUGMACROS("%s: %s\r\n", __FUNCTION__, f.err.c_str());
        destroyScript(f);
        return false;
    }

    int status = macroLuaRunChunk(f, content);
    if (status != LUA_OK) {
        if (f.err.length() == 0) { f.err = "Scenario parse error"; }
        DEBUGMACROS("%s: %s -> %s\r\n", __FUNCTION__, f.name.c_str(), f.err.c_str());
        destroyScript(f);
        return false;
    }

    lua_State* L = eng->getLuaState();
    if (lua_isfunction(L, -1)) {
        // Вариант 1: скрипт вернул функцию — одно тело.
        int b = lua_absindex(L, -1);
        macroAddEntity(L, f, MACRO_ENT_BODY, "", b, -1);
    } else if (lua_istable(L, -1)) {
        // Вариант 2: таблица {desc, body, rules}
        int tbl = lua_absindex(L, -1);

        lua_getfield(L, tbl, "desc");
        if (lua_isstring(L, -1)) { f.desc = String(lua_tostring(L, -1)); }
        lua_pop(L, 1);

        lua_getfield(L, tbl, "body");
        if (lua_isfunction(L, -1)) {
            int b = lua_absindex(L, -1);
            macroAddEntity(L, f, MACRO_ENT_BODY, "", b, -1);
        }
        lua_pop(L, 1);

        lua_getfield(L, tbl, "rules");
        if (lua_istable(L, -1)) {
            int ridx = lua_absindex(L, -1);
            int n = (int)lua_rawlen(L, ridx);
            for (int i = 1; i <= n; i++) {
                lua_rawgeti(L, ridx, i);
                if (lua_istable(L, -1)) { macroParseRule(L, f, lua_absindex(L, -1)); }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    } else {
        f.err = "no return value";
    }
    lua_settop(L, 0);

    if (f.err == "no return value") {
        DEBUGMACROS("%s: %s\r\n", __FUNCTION__, f.err.c_str());
        destroyScript(f);
        return false;
    }

    if (f.nEnts == 0) {
        f.err = "no return value";
        DEBUGMACROS("%s: %s -> %s\r\n", __FUNCTION__, f.name.c_str(), f.err.c_str());
        destroyScript(f);
        return false;
    }

    // Мета-cron (гейт окна) из web-таблицы. Невалидный cron → скрипт не исполняется.
    f.metaValid = true;
    f.metaInit = false;
    f.metaNext = 0;
    if (f.metaCron.length() > 0) {
        const char* perr = NULL;
        memset(&f.metaExpr, 0, sizeof(f.metaExpr));
        cron_parse_expr(f.metaCron.c_str(), &f.metaExpr, &perr);
        if (perr) {
            f.metaValid = false;
            f.err = "cron error";
        }
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

/** Выполняет тело сущности и записывает ошибку в f.err (с выводом в журнал).
 \param f сценарий
 \param e сущность (cron/cond/term/button/body)
 */
void CLASS_MODULE_MACROS::execBody(MacroFile& f, MacroEntity& e) {
    String err = runBody(f, e.bodyRef);
    if (err.length() > 0) {
        f.err = err;
        DEBUGMACROS("[MACRO] %s exec error: %s\r\n", f.name.c_str(), err.c_str());
    }
}

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

// ============================================================
// Обработчики событий/async шины
// ============================================================

static int macroLuaEventTramp(void* user, int argc, const BusValue* argv, BusValue& result) {
    (void)result;
    MacroFile* f = (MacroFile*)user;
    if (f == NULL || f->lua == NULL) { return BUS_ERR_INTERNAL; }
    if (argc < 1) { return BUS_ERR_BAD_ARGC; }

    String evt = argv[0].s;
    BusValue val = (argc > 1) ? argv[1] : BusValue();

    int ref = LUA_NOREF;
    for (uint8_t i = 0; i < f->nSubs; i++) {
        if (f->subEvts[i] == evt && f->subRefs[i] != LUA_NOREF) { ref = f->subRefs[i]; break; }
    }
    if (ref == LUA_NOREF) { return BUS_OK; }

    lua_State* L = f->lua->getLuaState();
    macroLuaSteps = 0;
    lua_sethook(L, macroLuaHook, LUA_MASKCOUNT, MACRO_LUA_HOOK_N);

    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_pushstring(L, evt.c_str());
    macroPushBusValue(L, val);
    int status = lua_pcall(L, 2, 0, 0);
    lua_sethook(L, NULL, 0, 0);

    if (status != LUA_OK) {
        f->err = macroLuaPopError(L);
    } else {
        lua_settop(L, 0);
    }
    return BUS_OK;
}

static int macroLuaAsyncTramp(void* user, int argc, const BusValue* argv, BusValue& result) {
    (void)result;
    MacroFile* f = (MacroFile*)user;
    if (f == NULL || f->lua == NULL) { return BUS_ERR_INTERNAL; }
    if (!f->asyncPending || f->asyncCbRef == LUA_NOREF) { return BUS_ERR_INTERNAL; }

    int rc = (argc > 0) ? (int)argv[0].i : 0;
    BusValue val = (argc > 1) ? argv[1] : BusValue();

    lua_State* L = f->lua->getLuaState();
    int ref = f->asyncCbRef;
    f->asyncPending = false;
    f->asyncCbRef = LUA_NOREF;

    macroLuaSteps = 0;
    lua_sethook(L, macroLuaHook, LUA_MASKCOUNT, MACRO_LUA_HOOK_N);

    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_pushinteger(L, rc);
    macroPushBusValue(L, val);
    int status = lua_pcall(L, 2, 0, 0);
    lua_sethook(L, NULL, 0, 0);

    if (status != LUA_OK) {
        f->err = macroLuaPopError(L);
    } else {
        lua_settop(L, 0);
    }
    return BUS_OK;
}

// ============================================================
// Мета-cron: открыто ли окно в текущую секунду
// ============================================================

static bool macroMetaGate(MacroFile& f, bool ntpSync) {
    if (f.metaCron.length() == 0) { return true; }
    if (f.metaValid == false) { return false; }
    if (ntpSync == false) { return false; }

    time_t t = (time_t)now();
    time_t from = (t > 0) ? (t - 1) : 0;
    time_t n = cron_next(&f.metaExpr, from);
    return (n != (time_t)-1 && n <= t);
}

/** Обрабатывает очередь внешних событий: сопоставляет term/button-правила
  запущенных файлов и выполняет их тела (с учётом мета-cron).
 */
void CLASS_MODULE_MACROS::drainEvents() {
    bool ntpSync = (NTP.getLastNTPSync() > 0);
    while (_evIn != _evOut) {
        MacroEvent ev = _evQueue[_evOut];
        _evOut = (_evOut + 1) % MACRO_EV_QUEUE;

        for (uint8_t i = 0; i < _fileCount; i++) {
            MacroFile& f = _files[i];
            if (f.run == false || f.active == false) { continue; }
            if (macroMetaGate(f, ntpSync) == false) { continue; }
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
  пересобирает сценарии, затем обрабатывает события и исполняет cron/cond/body.
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

        bool gate = macroMetaGate(f, ntpSync);

        for (uint8_t j = 0; j < f.nEnts; j++) {
            MacroEntity& e = f.ents[j];

            if (e.type == MACRO_ENT_BODY) {
                if (f.metaCron.length() > 0) {
                    if (gate && e.lastCond == false) {
                        e.lastCond = true;
                        DEBUGMACROS("[MACRO] %s body(meta)\r\n", f.name.c_str());
                        execBody(f, e);
                    } else if (gate == false) {
                        e.lastCond = false;
                    }
                } else if (e.lastCond == false) {
                    e.lastCond = true;
                    DEBUGMACROS("[MACRO] %s body\r\n", f.name.c_str());
                    execBody(f, e);
                }
                continue;
            }

            if (gate == false) { continue; }

            if (e.type == MACRO_ENT_CRON) {
                if (ntpSync == false) { continue; }

                if (e.next == 0) {
                    time_t nx = cron_next(&e.expr, (time_t)now());
                    e.next = nx;
                    continue;
                }
                if (e.next == (time_t)-1) { continue; }

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
