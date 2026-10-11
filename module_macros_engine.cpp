// ============================================================
// module_macros_engine.cpp — «барабан» сценариев (реальный Lua):
// один Lua-интерпретатор, файл = чанк (тело тикает раз в секунду),
// ресурсные namespace-прокси поверх core_state, планировщик cron/every.
// ============================================================

#include "module_macros.h"

#include "core_state/core_state.h"
#include "core_state/common_module.h"
#include "core_ntp/NtpClientLib.h"
#include "common/TimeLib.h"
#include "common_module.h"

#include <stdio.h>
#include <string.h>
#include <new>

// Единый счётчик VM-инструкций для защиты от «зависаний» сценария.
static uint32_t macroLuaSteps = 0;

// Контекст текущего исполняемого файла (тело чанка или обработчик). Всё
// исполняется в main-loop, поэтому одного статического указателя достаточно.
static MacroFile* macroCurFile = nullptr;

// При валидации/чтении меты (/macros/validate, refreshFileMeta) побочные
// действия (запись на шину, планирование) подавляются.
static bool macroSuppressSideEffects = false;

// Ключи кэшей в Lua registry (light-userdata-адреса уникальны).
static char MACRO_REG_NSMT    = 0;   // общая метатаблица namespace-прокси
static char MACRO_REG_NSCACHE = 0;   // кэш namespace-прокси (по имени ns)
static char MACRO_REG_FNCACHE = 0;   // кэш вызываемых closure (по full-имени)

// Свободный хип для диагностики (ESP32-only модуль).
static unsigned macroHeapFree() {
    unsigned v = 0;
#if defined(ESP32)
    v = (unsigned)ESP.getFreeHeap();
#endif
    return v;
}

// Форвард-объявления
static void macroLuaHook(lua_State* L, lua_Debug* ar);
static String macroLuaPopError(lua_State* L);
static void macroPushBusValue(lua_State* L, const BusValue& v);
static bool macroLuaToBusValue(lua_State* L, int idx, BusValue& out);
static void macroPushStateValue(lua_State* L, const char* full, const BusResInfo& info);
static void macroPushNsProxy(lua_State* L, const char* ns);
static void macroPushFnClosure(lua_State* L, const char* full);
static int macroEnvIndex(lua_State* L);
static int macroNsIndex(lua_State* L);
static int macroNsNewindex(lua_State* L);
static int macroFnCall(lua_State* L);
static int macroCompileSetup(lua_State* L);
static int macroCompileChunk(lua_State* L, MacroFile& f, const String& content);
static bool macroRunBody(lua_State* L, MacroFile& f);
static bool macroAddHandler(lua_State* L, MacroFile& f, uint8_t type, const String& key,
                            int fnRef, bool hasCron, const cron_expr* ex, uint32_t dueMs);
static void macroRemoveHandler(lua_State* L, MacroFile& f, uint8_t idx);
static void macroCallHandler(lua_State* L, MacroFile& f, MacroHandler& h,
                             const char* typeName, const BusValue* value);
static void macroFreeRuntime(lua_State* L, MacroFile& f);

static int macroLuaEvery(lua_State* L);
static int macroLuaAfter(lua_State* L);
static int macroLuaOnce(lua_State* L);
static int macroLuaTick(lua_State* L);
static int macroLuaOn(lua_State* L);
static int macroLuaTerm(lua_State* L);
static int macroLuaButton(lua_State* L);
static int macroLuaMeta(lua_State* L);
static int macroLuaGet(lua_State* L);
static int macroLuaSet(lua_State* L);
static int macroLuaCall(lua_State* L);
static int macroLuaHas(lua_State* L);
static int macroLuaMode(lua_State* L);
static int macroLuaInfo(lua_State* L);
static int macroLuaPuts(lua_State* L);
static int macroLuaNow(lua_State* L);
static int macroLuaClock(lua_State* L);
static int macroNoopCb(void* user, int argc, const BusValue* argv, BusValue& result);
static int macroEventTramp(void* user, int argc, const BusValue* argv, BusValue& result);

// ============================================================
// Защитный hook и ошибки
// ============================================================

static void macroLuaHook(lua_State* L, lua_Debug* ar) {
    (void)ar;
    macroLuaSteps += MACRO_LUA_HOOK_N;
    if (macroLuaSteps >= MACRO_LUA_MAX_OPS) {
        luaL_error(L, "scenario step limit exceeded (%d ops)", MACRO_LUA_MAX_OPS);
    }
}

static String macroLuaPopError(lua_State* L) {
    String out;
    if (lua_gettop(L) == 0) { return out; }

    size_t len = 0;
    const char* s = lua_tolstring(L, -1, &len);
    if (s == NULL) {
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

// ============================================================
// Мосты к ресурсной шине core_state
// ============================================================

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
    // lua_type, а не lua_isnumber: lua_isnumber() истинно и для числовых СТРОК.
    int t = lua_type(L, idx);
    if (t == LUA_TBOOLEAN) { out = BusValue::bo(lua_toboolean(L, idx) != 0); return true; }
    if (t == LUA_TNUMBER) {
        if (lua_isinteger(L, idx)) { out = BusValue::i32((int32_t)lua_tointeger(L, idx)); }
        else { out = BusValue::f32((float)lua_tonumber(L, idx)); }
        return true;
    }
    if (t == LUA_TSTRING) { out = BusValue::str(String(lua_tostring(L, idx))); return true; }
    return false;
}

// Кладёт текущее значение ресурса: BOOL→boolean, I32/F32/TIME→number,
// STR→string, ENUM→каноничное имя (строка). Только для ресурсов-состояний.
static void macroPushStateValue(lua_State* L, const char* full, const BusResInfo& info) {
    switch (info.kind) {
        case BusValue::BOOL: lua_pushboolean(L, core_state.getBool(full) ? 1 : 0); break;
        case BusValue::I32:  lua_pushinteger(L, (lua_Integer)core_state.getInt(full)); break;
        case BusValue::F32:  lua_pushnumber(L, (lua_Number)core_state.getF32(full)); break;
        case BusValue::TIME: lua_pushinteger(L, (lua_Integer)core_state.getTime(full)); break;
        case BusValue::STR:  lua_pushstring(L, core_state.getStr(full).c_str()); break;
        case BusValue::ENUM: {
            int32_t idx = core_state.getInt(full);
            const char* nm = core_state.enumNameByIndex(full, idx);
            if (nm != NULL) { lua_pushstring(L, nm); } else { lua_pushinteger(L, (lua_Integer)idx); }
            break;
        }
        default: lua_pushnil(L); break;
    }
}

// ============================================================
// Кэши namespace-прокси и вызываемых closure
// ============================================================

// Оставляет на стеке registry-таблицу под ключом key (создаёт при отсутствии).
static void macroGetRegistryTable(lua_State* L, void* key) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, key);
    if (lua_istable(L, -1)) { return; }
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, key);
}

static void macroPushNsMetatable(lua_State* L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &MACRO_REG_NSMT);
    if (lua_istable(L, -1)) { return; }
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushcfunction(L, macroNsIndex);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, macroNsNewindex);
    lua_setfield(L, -2, "__newindex");
    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &MACRO_REG_NSMT);
}

static void macroPushNsProxy(lua_State* L, const char* ns) {
    macroGetRegistryTable(L, &MACRO_REG_NSCACHE);   // [cache]
    lua_getfield(L, -1, ns);                        // [cache, proxy?]
    if (lua_istable(L, -1)) { lua_remove(L, -2); return; }
    lua_pop(L, 1);                                  // [cache]

    lua_newtable(L);                                // [cache, proxy]
    lua_pushstring(L, ns);
    lua_setfield(L, -2, "__ns");
    macroPushNsMetatable(L);                        // [cache, proxy, mt]
    lua_setmetatable(L, -2);
    lua_pushvalue(L, -1);                           // [cache, proxy, proxy]
    lua_setfield(L, -3, ns);                        // cache[ns] = proxy
    lua_remove(L, -2);                              // [proxy]
}

static void macroPushFnClosure(lua_State* L, const char* full) {
    macroGetRegistryTable(L, &MACRO_REG_FNCACHE);   // [cache]
    lua_getfield(L, -1, full);                      // [cache, fn?]
    if (lua_isfunction(L, -1)) { lua_remove(L, -2); return; }
    lua_pop(L, 1);                                  // [cache]

    lua_pushstring(L, full);                        // [cache, full]
    lua_pushcclosure(L, macroFnCall, 1);            // [cache, fn]
    lua_pushvalue(L, -1);                           // [cache, fn, fn]
    lua_setfield(L, -3, full);                      // cache[full] = fn
    lua_remove(L, -2);                              // [fn]
}

// ============================================================
// Методы метатаблиц: env (глобальное окружение) и namespace-прокси
// ============================================================

// __index окружения файла: сначала namespace, затем обычные глобалы (хелперы).
static int macroEnvIndex(lua_State* L) {
    const char* key = lua_tostring(L, 2);
    if (key == NULL) { lua_pushnil(L); return 1; }
    if (core_state.hasNamespace(key)) { macroPushNsProxy(L, key); return 1; }
    lua_pushglobaltable(L);
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);      // [global, value]
    return 1;
}

// __index namespace-прокси: ns.field -> состояние/функция ресурса.
static int macroNsIndex(lua_State* L) {
    lua_getfield(L, 1, "__ns");                     // [ns]
    const char* ns = lua_tostring(L, -1);
    const char* key = lua_tostring(L, 2);
    if (ns == NULL || key == NULL) { lua_pop(L, 1); lua_pushnil(L); return 1; }

    String full = String(ns) + "." + key;
    lua_pop(L, 1);

    if (core_state.has(full.c_str()) == false) { lua_pushnil(L); return 1; }
    BusResInfo info;
    core_state.info(full.c_str(), info);
    if (info.isEvent) { lua_pushnil(L); return 1; }
    if (info.isFunc) { macroPushFnClosure(L, full.c_str()); return 1; }
    macroPushStateValue(L, full.c_str(), info);
    return 1;
}

// __newindex namespace-прокси: запись значения ресурса (ENUM — только строка).
static int macroNsNewindex(lua_State* L) {
    lua_getfield(L, 1, "__ns");
    const char* ns = lua_tostring(L, -1);
    const char* key = lua_tostring(L, 2);
    String full = String(ns != NULL ? ns : "") + "." + String(key != NULL ? key : "");
    lua_pop(L, 1);

    if (core_state.has(full.c_str()) == false) {
        return luaL_error(L, "unknown resource: %s", full.c_str());
    }
    BusResInfo info;
    core_state.info(full.c_str(), info);

    BusValue v;
    if (info.kind == BusValue::ENUM) {
        // Для ENUM принимается ТОЛЬКО буквенное имя; числа отвергаются.
        if (lua_type(L, 3) != LUA_TSTRING) {
            return luaL_error(L, "enum %s requires name, got number", full.c_str());
        }
        const char* name = lua_tostring(L, 3);
        int idx = core_state.enumIndexByName(full.c_str(), name, nullptr);
        if (idx < 0) {
            return luaL_error(L, "unknown enum value '%s' for %s", name, full.c_str());
        }
        v = BusValue::en(idx);
    } else {
        if (macroLuaToBusValue(L, 3, v) == false) {
            return luaL_error(L, "bad value for %s", full.c_str());
        }
    }

    if (macroSuppressSideEffects) { return 0; }
    int rc = core_state.applyValue(full.c_str(), v);
    if (rc != BUS_OK && macroCurFile != NULL) {
        macroCurFile->err = String("set ") + full + ": " + ns_core_state::busErrStr(rc);
    }
    return 0;
}

// Вызов функции ресурса: e7.speed(50) -> core_state.call (ENUM — строка).
static int macroFnCall(lua_State* L) {
    const char* full = lua_tostring(L, lua_upvalueindex(1));
    if (full == NULL) { lua_pushnil(L); lua_pushinteger(L, BUS_ERR_NOT_FOUND); return 2; }

    int top = lua_gettop(L);
    if (top > CORE_STATE_MAX_ARGS) { top = CORE_STATE_MAX_ARGS; }
    BusValue args[CORE_STATE_MAX_ARGS];
    int argc = 0;
    for (int i = 1; i <= top; i++) {
        if (macroLuaToBusValue(L, i, args[argc])) { argc++; }
    }

    if (macroSuppressSideEffects) {
        lua_pushnil(L);
        lua_pushinteger(L, BUS_OK);
        return 2;
    }

    BusResInfo info;
    bool haveInfo = (core_state.info(full, info) == BUS_OK);

    // ENUM-функция (dual ENUM): первый аргумент — буквенное имя, не число.
    if (haveInfo && argc >= 1 && info.kind == BusValue::ENUM) {
        if (lua_type(L, 1) != LUA_TSTRING) {
            return luaL_error(L, "enum %s requires name, got number", full);
        }
        const char* name = lua_tostring(L, 1);
        int idx = core_state.enumIndexByName(full, name, nullptr);
        if (idx < 0) {
            return luaL_error(L, "unknown enum value '%s' for %s", name, full);
        }
        args[0] = BusValue::en(idx);
    }

    // call на чистом state-ресурсе = set (как и раньше).
    if (haveInfo && !info.isFunc && info.kind != BusValue::NONE) {
        BusValue v = (argc > 0) ? args[0] : BusValue();
        int rc = core_state.applyValue(full, v);
        BusValue rv = core_state.valueToKind(v, info.kind);
        macroPushBusValue(L, rv);
        lua_pushinteger(L, rc);
        return 2;
    }

    BusValue result;
    int rc = core_state.call(full, argc, (argc > 0) ? args : nullptr, result);
    if (rc == BUS_OK) {
        macroPushBusValue(L, result);
        lua_pushinteger(L, rc);
        return 2;
    }
    lua_pushnil(L);
    lua_pushinteger(L, rc);
    return 2;
}

// ============================================================
// Компиляция чанка и создание окружения
// ============================================================

// Создаёт per-file окружение (_ENV) и таблицу state; refs кладёт в macroCurFile.
// Выполняется под lua_pcall (аллокации таблиц вне защиты → abort()).
static int macroCompileSetup(lua_State* L) {
    lua_newtable(L);                 // [chunk, state]
    int stateIdx = lua_gettop(L);
    lua_newtable(L);                 // [chunk, state, env]
    int envIdx = lua_gettop(L);
    lua_newtable(L);                 // [chunk, state, env, mt]
    lua_pushcfunction(L, macroEnvIndex);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, envIdx);

    lua_pushvalue(L, stateIdx);
    lua_setfield(L, envIdx, "state");

    lua_pushvalue(L, envIdx);
    lua_setupvalue(L, 1, 1);         // chunk._ENV = env (pops env-copy)

    if (macroCurFile != NULL) {
        lua_pushvalue(L, stateIdx);
        macroCurFile->stateRef = luaL_ref(L, LUA_REGISTRYINDEX);
        lua_pushvalue(L, envIdx);
        macroCurFile->envRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    lua_pushvalue(L, 1);             // вернуть чанк
    return 1;
}

// Компилирует content как чанк с песочницей; результат — ref в f.chunkRef.
static int macroCompileChunk(lua_State* L, MacroFile& f, const String& content) {
    if (L == NULL) { f.err = "No interpreter"; return LUA_ERRRUN; }

    f.chunkRef = LUA_NOREF;
    f.envRef = LUA_NOREF;
    f.stateRef = LUA_NOREF;

    String cn = "@";
    cn += f.name;

    macroLuaSteps = 0;
    lua_sethook(L, macroLuaHook, LUA_MASKCOUNT, MACRO_LUA_HOOK_N);

    int status = luaL_loadbuffer(L, content.c_str(), content.length(), cn.c_str());
    if (status == LUA_OK) {
        macroCurFile = &f;
        lua_pushcfunction(L, macroCompileSetup);
        lua_insert(L, -2);                 // [setup, chunk]
        status = lua_pcall(L, 1, 1, 0);    // -> [chunk]
        macroCurFile = nullptr;
    }
    lua_sethook(L, NULL, 0, 0);

    if (status != LUA_OK) {
        f.err = macroLuaPopError(L);
        if (f.err.length() == 0) { f.err = "Scenario parse error"; }
        lua_settop(L, 0);
        return status;
    }
    f.chunkRef = luaL_ref(L, LUA_REGISTRYINDEX);   // pops chunk
    return LUA_OK;
}

// Исполняет тело чанка (top-level) текущего файла.
static bool macroRunBody(lua_State* L, MacroFile& f) {
    if (L == NULL || f.chunkRef == LUA_NOREF) { return false; }

    lua_rawgeti(L, LUA_REGISTRYINDEX, f.chunkRef);   // [chunk]
    macroCurFile = &f;
    macroLuaSteps = 0;
    lua_sethook(L, macroLuaHook, LUA_MASKCOUNT, MACRO_LUA_HOOK_N);
    int status = lua_pcall(L, 0, 0, 0);
    lua_sethook(L, NULL, 0, 0);
    macroCurFile = nullptr;

    if (status != LUA_OK) {
        f.err = macroLuaPopError(L);
        return false;
    }
    return true;
}

// Освобождает refs/обработчики runtime-файла (без подписок).
static void macroFreeRuntime(lua_State* L, MacroFile& f) {
    if (L != NULL) {
        if (f.handlers != NULL) {
            for (uint8_t i = 0; i < f.nHandlers; i++) {
                if (f.handlers[i].fnRef != LUA_NOREF) {
                    luaL_unref(L, LUA_REGISTRYINDEX, f.handlers[i].fnRef);
                }
            }
        }
        if (f.chunkRef != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, f.chunkRef); }
        if (f.envRef != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, f.envRef); }
        if (f.stateRef != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, f.stateRef); }
    }
    if (f.handlers != NULL) { delete[] f.handlers; f.handlers = NULL; }
    f.nHandlers = 0;
    f.chunkRef = f.envRef = f.stateRef = LUA_NOREF;
}

// ============================================================
// Реестр обработчиков файла
// ============================================================

// Добавляет/заменяет обработчик. При замене расписание сохраняется (тело файла
// выполняется каждый тик и перерегистрирует функции — расписание не сбрасывается).
static bool macroAddHandler(lua_State* L, MacroFile& f, uint8_t type, const String& key,
                            int fnRef, bool hasCron, const cron_expr* ex, uint32_t dueMs) {
    if (f.handlers == NULL) {
        luaL_unref(L, LUA_REGISTRYINDEX, fnRef);
        return false;
    }
    for (uint8_t i = 0; i < f.nHandlers; i++) {
        if (f.handlers[i].type == type && f.handlers[i].key == key) {
            if (f.handlers[i].fnRef != LUA_NOREF) {
                luaL_unref(L, LUA_REGISTRYINDEX, f.handlers[i].fnRef);
            }
            f.handlers[i].fnRef = fnRef;
            if (hasCron && ex != NULL) { f.handlers[i].expr = *ex; }
            return true;
        }
    }
    if (f.nHandlers >= MACRO_MAX_HANDLERS) {
        f.err = "too many handlers";
        luaL_unref(L, LUA_REGISTRYINDEX, fnRef);
        return false;
    }
    MacroHandler& h = f.handlers[f.nHandlers];
    h.type = type;
    h.key = key;
    h.fnRef = fnRef;
    h.next = 0;
    h.dueMs = dueMs;
    if (hasCron && ex != NULL) { h.expr = *ex; } else { memset(&h.expr, 0, sizeof(h.expr)); }
    f.nHandlers++;
    return true;
}

static void macroRemoveHandler(lua_State* L, MacroFile& f, uint8_t idx) {
    if (idx >= f.nHandlers) { return; }
    if (f.handlers[idx].fnRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, f.handlers[idx].fnRef);
    }
    for (uint8_t j = idx; j + 1 < f.nHandlers; j++) { f.handlers[j] = f.handlers[j + 1]; }
    f.nHandlers--;
}

// Строит таблицу события в защищённом вызове (аллокации вне pcall → abort()).
struct MacroEvCtx { const char* type; const char* spec; const BusValue* value; };
static MacroEvCtx macroEvCtx;
static String macroEvSpec;   // живёт до конца pcall (spec без служебного суффикса)

static int macroPushEventTableC(lua_State* L) {
    lua_newtable(L);
    lua_pushstring(L, macroEvCtx.type ? macroEvCtx.type : "");
    lua_setfield(L, -2, "type");
    lua_pushstring(L, macroEvCtx.spec ? macroEvCtx.spec : "");
    lua_setfield(L, -2, "spec");
    if (macroEvCtx.value != NULL) {
        macroPushBusValue(L, *macroEvCtx.value);
        lua_setfield(L, -2, "value");
    }
    return 1;
}

// Вызывает обработчик с таблицей события {type, spec, value?}.
static void macroCallHandler(lua_State* L, MacroFile& f, MacroHandler& h,
                             const char* typeName, const BusValue* value) {
    if (h.fnRef == LUA_NOREF) { return; }
    if (macroHeapFree() < MACRO_LUA_MIN_HEAP) { f.err = "low memory"; return; }

    lua_rawgeti(L, LUA_REGISTRYINDEX, h.fnRef);      // [fn]
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }

    macroEvCtx.type = typeName;
    // spec без служебного суффикса "@<source:line>" (cron/interval).
    macroEvSpec = h.key;
    int at = macroEvSpec.indexOf('@');
    if (at >= 0) { macroEvSpec = macroEvSpec.substring(0, at); }
    macroEvCtx.spec = macroEvSpec.c_str();
    macroEvCtx.value = value;
    lua_pushlightuserdata(L, &macroEvCtx);
    lua_pushcclosure(L, macroPushEventTableC, 1);    // [fn, evbuilder]

    macroLuaSteps = 0;
    lua_sethook(L, macroLuaHook, LUA_MASKCOUNT, MACRO_LUA_HOOK_N);
    int status = lua_pcall(L, 0, 1, 0);              // -> [fn, ev]
    if (status == LUA_OK) {
        macroCurFile = &f;
        status = lua_pcall(L, 1, 0, 0);
        macroCurFile = nullptr;
    }
    lua_sethook(L, NULL, 0, 0);

    if (status != LUA_OK) {
        String e = macroLuaPopError(L);
        if (e.length() > 0 && f.err.length() == 0) { f.err = e; }
    }
}

// ============================================================
// Глобальные функции сценария
// ============================================================

// Ключ планировщика по месту вызова (source:line) — идемпотентность after/once/tick.
static String macroCallSite(lua_State* L) {
    lua_Debug ar;
    if (lua_getstack(L, 1, &ar) == 0) { return String("?"); }
    lua_getinfo(L, "Sl", &ar);
    String s = (ar.short_src != NULL) ? String(ar.short_src) : String("?");
    s += ":";
    s += String(ar.currentline);
    return s;
}

static int macroLuaEvery(lua_State* L) {
    if (macroSuppressSideEffects || macroCurFile == NULL) { return 0; }
    if (!lua_isfunction(L, 2)) { return luaL_error(L, "every: function required"); }

    uint8_t type;
    String key;
    cron_expr ex;
    bool hasCron = false;
    uint32_t dueMs = 0;
    memset(&ex, 0, sizeof(ex));

    if (lua_type(L, 1) == LUA_TNUMBER) {
        int secs = (int)lua_tointeger(L, 1);
        if (secs < 1) { secs = 1; }
        type = MACRO_H_INTERVAL;
        key = String(secs);
        dueMs = millis() + (uint32_t)secs * 1000UL;
    } else if (lua_isstring(L, 1)) {
        type = MACRO_H_CRON;
        String pattern = String(lua_tostring(L, 1));
        // Ключ = шаблон + место вызова: два разных every() в одном файле не
        // перекрываются, а повторная регистрация из того же места — идемпотентна.
        key = pattern + "@" + macroCallSite(L);
        String norm = ns_module_macros::macroCronNormalize(pattern);
        const char* perr = NULL;
        cron_parse_expr(norm.c_str(), &ex, &perr);
        if (perr != NULL) { return luaL_error(L, "cron error: %s", perr); }
        hasCron = true;
    } else {
        return luaL_error(L, "every: pattern or seconds required");
    }

    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    macroAddHandler(L, *macroCurFile, type, key, ref, hasCron, &ex, dueMs);
    return 0;
}

static int macroLuaAfter(lua_State* L) {
    if (macroSuppressSideEffects || macroCurFile == NULL) { return 0; }
    if (!lua_isfunction(L, 2)) { return luaL_error(L, "after: function required"); }
    int secs = (int)luaL_checkinteger(L, 1);
    if (secs < 0) { secs = 0; }
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    macroAddHandler(L, *macroCurFile, MACRO_H_AFTER, String("after:") + macroCallSite(L),
                    ref, false, nullptr, millis() + (uint32_t)secs * 1000UL);
    return 0;
}

static int macroLuaOnce(lua_State* L) {
    if (macroSuppressSideEffects || macroCurFile == NULL) { return 0; }
    if (!lua_isfunction(L, 1)) { return luaL_error(L, "once: function required"); }
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    macroAddHandler(L, *macroCurFile, MACRO_H_ONCE, String("once:") + macroCallSite(L),
                    ref, false, nullptr, millis());
    return 0;
}

static int macroLuaTick(lua_State* L) {
    if (macroSuppressSideEffects || macroCurFile == NULL) { return 0; }
    if (!lua_isfunction(L, 1)) { return luaL_error(L, "tick: function required"); }
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    macroAddHandler(L, *macroCurFile, MACRO_H_TICK, String("tick:") + macroCallSite(L),
                    ref, false, nullptr, 0);
    return 0;
}

static int macroLuaOn(lua_State* L) {
    if (macroSuppressSideEffects || macroCurFile == NULL) { return 0; }
    const char* name = luaL_checkstring(L, 1);
    if (!lua_isfunction(L, 2)) { return luaL_error(L, "on: function required"); }
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    macroAddHandler(L, *macroCurFile, MACRO_H_ON, String(name), ref, false, nullptr, 0);
    module_macros.ensureEventSub(String(name));
    return 0;
}

static int macroLuaTerm(lua_State* L) {
    if (macroSuppressSideEffects || macroCurFile == NULL) { return 0; }
    const char* spec = luaL_checkstring(L, 1);
    if (!lua_isfunction(L, 2)) { return luaL_error(L, "term: function required"); }
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    macroAddHandler(L, *macroCurFile, MACRO_H_TERM, String(spec), ref, false, nullptr, 0);
    return 0;
}

static int macroLuaButton(lua_State* L) {
    if (macroSuppressSideEffects || macroCurFile == NULL) { return 0; }
    const char* name = luaL_checkstring(L, 1);
    if (!lua_isfunction(L, 2)) { return luaL_error(L, "button: function required"); }
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    macroAddHandler(L, *macroCurFile, MACRO_H_BUTTON, String(name), ref, false, nullptr, 0);
    return 0;
}

static int macroLuaMeta(lua_State* L) {
    if (macroCurFile == NULL || !lua_istable(L, 1)) { return 0; }
    lua_getfield(L, 1, "desc");
    if (lua_isstring(L, -1)) { macroCurFile->desc = String(lua_tostring(L, -1)); }
    lua_pop(L, 1);
    return 0;
}

static int macroLuaGet(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    if (core_state.has(name) == false) { lua_pushnil(L); return 1; }
    BusResInfo info;
    core_state.info(name, info);
    if (info.isFunc && info.kind == BusValue::NONE) { lua_pushnil(L); return 1; }
    macroPushStateValue(L, name, info);
    return 1;
}

static int macroLuaSet(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    if (core_state.has(name) == false) { lua_pushinteger(L, BUS_ERR_NOT_FOUND); return 1; }
    BusResInfo info;
    core_state.info(name, info);

    BusValue v;
    if (lua_gettop(L) >= 2) { macroLuaToBusValue(L, 2, v); }

    if (info.kind == BusValue::ENUM) {
        if (lua_type(L, 2) != LUA_TSTRING) { lua_pushinteger(L, BUS_ERR_BAD_VALUE); return 1; }
        int idx = core_state.enumIndexByName(name, lua_tostring(L, 2), nullptr);
        if (idx < 0) { lua_pushinteger(L, BUS_ERR_BAD_VALUE); return 1; }
        v = BusValue::en(idx);
    }

    if (macroSuppressSideEffects) { lua_pushinteger(L, BUS_OK); return 1; }
    lua_pushinteger(L, core_state.applyValue(name, v));
    return 1;
}

static int macroLuaCall(lua_State* L) {
    if (macroSuppressSideEffects) {
        lua_pushnil(L);
        lua_pushinteger(L, BUS_OK);
        return 2;
    }
    const char* name = luaL_checkstring(L, 1);
    int narg = lua_gettop(L) - 1;
    if (narg > CORE_STATE_MAX_ARGS) { narg = CORE_STATE_MAX_ARGS; }
    BusValue args[CORE_STATE_MAX_ARGS];
    int argc = 0;
    for (int i = 0; i < narg; i++) {
        if (macroLuaToBusValue(L, 2 + i, args[argc])) { argc++; }
    }

    BusResInfo info;
    bool haveInfo = (core_state.info(name, info) == BUS_OK);
    if (haveInfo && argc >= 1 && info.kind == BusValue::ENUM) {
        if (lua_type(L, 2) != LUA_TSTRING) {
            lua_pushnil(L);
            lua_pushinteger(L, BUS_ERR_BAD_VALUE);
            return 2;
        }
        int idx = core_state.enumIndexByName(name, lua_tostring(L, 2), nullptr);
        if (idx < 0) {
            lua_pushnil(L);
            lua_pushinteger(L, BUS_ERR_BAD_VALUE);
            return 2;
        }
        args[0] = BusValue::en(idx);
    }

    // call на чистом state = set.
    if (haveInfo && !info.isFunc && info.kind != BusValue::NONE) {
        BusValue v = (argc > 0) ? args[0] : BusValue();
        int rc = core_state.applyValue(name, v);
        BusValue rv = core_state.valueToKind(v, info.kind);
        macroPushBusValue(L, rv);
        lua_pushinteger(L, rc);
        return 2;
    }

    BusValue result;
    int rc = core_state.call(name, argc, (argc > 0) ? args : nullptr, result);
    if (rc == BUS_OK) {
        macroPushBusValue(L, result);
        lua_pushinteger(L, rc);
        return 2;
    }
    lua_pushnil(L);
    lua_pushinteger(L, rc);
    return 2;
}

static int macroLuaHas(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    lua_pushboolean(L, core_state.has(name) ? 1 : 0);
    return 1;
}

static int macroLuaMode(lua_State* L) {
    const char* ns = luaL_checkstring(L, 1);
    if (lua_gettop(L) >= 2) {
        if (macroSuppressSideEffects) { lua_pushinteger(L, BUS_OK); return 1; }
        int m;
        if (lua_type(L, 2) == LUA_TNUMBER) {
            m = (int)lua_tointeger(L, 2);
        } else if (lua_type(L, 2) == LUA_TSTRING) {
            String full = String(ns) + ".mode";
            int idx = core_state.enumIndexByName(full.c_str(), lua_tostring(L, 2), nullptr);
            if (idx < 0) { lua_pushinteger(L, BUS_ERR_BAD_VALUE); return 1; }
            m = idx;
        } else {
            lua_pushinteger(L, BUS_ERR_BAD_TYPE);
            return 1;
        }
        lua_pushinteger(L, core_state.mode(ns, m));
        return 1;
    }
    lua_pushinteger(L, core_state.mode(ns, -1));
    return 1;
}

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
// Методы класса (движок «барабана»)
// ============================================================

bool CLASS_MODULE_MACROS::initLua() {
    if (_lua != NULL) { return true; }
    size_t heapBefore = ESP.getFreeHeap();
    _lua = new (std::nothrow) EspLuaEngine();
    if (_lua == NULL || _lua->getLuaState() == NULL) {
        delete _lua;
        _lua = NULL;
        _luaHeapBytes = 0;
        DEBUGMACROS("initLua: no memory for interpreter\r\n");
        return false;
    }
    if (macroRegisterHelpers(*_lua) == false) {
        DEBUGMACROS("initLua: register failed\r\n");
        delete _lua;
        _lua = NULL;
        _luaHeapBytes = 0;
        return false;
    }
    size_t heapAfter = ESP.getFreeHeap();
    _luaHeapBytes = (heapBefore > heapAfter) ? (heapBefore - heapAfter) : 0;
    return true;
}

// Регистрирует глобальные функции сценария на интерпретаторе. Используется как
// для единого _lua, так и для изолированного движка валидации (веб-задача).
bool macroRegisterHelpers(EspLuaEngine& eng) {
    return
        eng.registerFunction("every",  macroLuaEvery)  &&
        eng.registerFunction("after",  macroLuaAfter)  &&
        eng.registerFunction("once",   macroLuaOnce)   &&
        eng.registerFunction("tick",   macroLuaTick)   &&
        eng.registerFunction("on",     macroLuaOn)     &&
        eng.registerFunction("term",   macroLuaTerm)   &&
        eng.registerFunction("button", macroLuaButton) &&
        eng.registerFunction("meta",   macroLuaMeta)   &&
        eng.registerFunction("get",    macroLuaGet)    &&
        eng.registerFunction("set",    macroLuaSet)    &&
        eng.registerFunction("call",   macroLuaCall)   &&
        eng.registerFunction("has",    macroLuaHas)    &&
        eng.registerFunction("mode",   macroLuaMode)   &&
        eng.registerFunction("info",   macroLuaInfo)   &&
        eng.registerFunction("puts",   macroLuaPuts)   &&
        eng.registerFunction("print",  macroLuaPuts)   &&
        eng.registerFunction("log",    macroLuaPuts)   &&
        eng.registerFunction("clock",  macroLuaClock)  &&
        eng.registerFunction("now",    macroLuaNow);
}

/** Освобождает runtime файла (refs, обработчики) и снимает неиспользуемые подписки. */
void CLASS_MODULE_MACROS::unregisterFile(MacroFile& f) {
    lua_State* L = (_lua != NULL) ? _lua->getLuaState() : NULL;
    macroFreeRuntime(L, f);
    f.active = false;
    pruneEventSubs();
}

/** Регистрирует файл: компилирует чанк (однажды) и создаёт окружение/state. */
bool CLASS_MODULE_MACROS::registerFile(MacroFile& f) {
    f.err = "";
    f.desc = "";
    f.needParse = false;
    unregisterFile(f);

    String content = readFile(f.name);
    if (content.length() == 0) {
        f.err = "File is empty or unreadable";
        DEBUGMACROS("%s: %s\r\n", __FUNCTION__, f.err.c_str());
        return false;
    }
    if (initLua() == false) {
        f.err = "No memory for interpreter";
        f.parseFails++;
        if (f.parseFails < MACRO_PARSE_MAX_RETRY) { f.needParse = true; }
        return false;
    }
    if (macroHeapFree() < MACRO_LUA_MIN_HEAP) {
        f.err = "low memory";
        f.parseFails++;
        if (f.parseFails < MACRO_PARSE_MAX_RETRY) { f.needParse = true; }
        return false;
    }

    lua_State* L = _lua->getLuaState();
    if (macroCompileChunk(L, f, content) != LUA_OK) {
        DEBUGMACROS("%s: %s -> %s\r\n", __FUNCTION__, f.name.c_str(), f.err.c_str());
        return false;
    }

    f.handlers = new (std::nothrow) MacroHandler[MACRO_MAX_HANDLERS];
    if (f.handlers == NULL) {
        f.err = "No memory for handlers";
        f.parseFails++;
        if (f.parseFails < MACRO_PARSE_MAX_RETRY) { f.needParse = true; }
        macroFreeRuntime(L, f);
        return false;
    }
    f.nHandlers = 0;

    f.active = true;
    f.parseFails = 0;
    DEBUGMACROS("%s: %s ok (heap %u)\r\n", __FUNCTION__, f.name.c_str(), macroHeapFree());
    return true;
}

/** Валидирует сценарий: компиляция + пробный прогон тела (без побочных действий). */
bool macroValidateScript(EspLuaEngine& eng, const String& content, String& err) {
    err = "";
    lua_State* L = eng.getLuaState();
    if (L == NULL) { err = "No interpreter"; return false; }
    if (macroHeapFree() < MACRO_LUA_MIN_HEAP) { err = "low memory"; return false; }

    MacroFile tf;
    tf.name = "@validate";
    tf.chunkRef = tf.envRef = tf.stateRef = LUA_NOREF;
    tf.handlers = NULL;
    tf.nHandlers = 0;
    tf.err = "";

    macroSuppressSideEffects = true;
    int status = macroCompileChunk(L, tf, content);
    if (status == LUA_OK) {
        macroRunBody(L, tf);
        if (tf.err.length() > 0) { status = LUA_ERRRUN; }
    }
    macroSuppressSideEffects = false;

    bool ok = (status == LUA_OK);
    if (!ok) { err = (tf.err.length() > 0) ? tf.err : String("Scenario parse error"); }

    macroFreeRuntime(L, tf);
    lua_settop(L, 0);
    return ok;
}

/** Читает только desc из meta{} без запуска живого файла (изолированная песочница). */
bool macroReadScenarioMeta(EspLuaEngine& eng, const String& content,
                           String& desc, String& err) {
    desc = "";
    err = "";
    lua_State* L = eng.getLuaState();
    if (L == NULL) { err = "No interpreter"; return false; }
    if (macroHeapFree() < MACRO_LUA_MIN_HEAP) { err = "low memory"; return false; }

    MacroFile tf;
    tf.name = "@meta";
    tf.chunkRef = tf.envRef = tf.stateRef = LUA_NOREF;
    tf.handlers = NULL;
    tf.nHandlers = 0;
    tf.err = "";

    macroSuppressSideEffects = true;
    int status = macroCompileChunk(L, tf, content);
    if (status == LUA_OK) { macroRunBody(L, tf); }
    macroSuppressSideEffects = false;

    desc = tf.desc;
    bool ok = (status == LUA_OK && tf.err.length() == 0);
    if (!ok) { err = (tf.err.length() > 0) ? tf.err : String("Scenario parse error"); }

    macroFreeRuntime(L, tf);
    lua_settop(L, 0);
    return ok;
}

/** Заполняет desc остановленного файла (без запуска живого сценария). */
void CLASS_MODULE_MACROS::refreshFileMeta(MacroFile& f) {
    if (f.active) { return; }
    if (_fs == NULL || _fs->exists(f.name) == false) { return; }
    if (initLua() == false) { return; }
    if (macroHeapFree() < MACRO_LUA_MIN_HEAP) { return; }

    String content = readFile(f.name);
    if (content.length() == 0) { return; }

    String desc, err;
    if (macroReadScenarioMeta(*_lua, content, desc, err)) { f.desc = desc; }
}

/** Пере(компилирует) только помеченные файлы (needParse), не более N за тик. */
void CLASS_MODULE_MACROS::rebuildScripts() {
    uint8_t done = 0;
    for (uint8_t i = 0; i < _fileCount; i++) {
        MacroFile& f = _files[i];
        if (f.run == false) {
            if (f.active) { unregisterFile(f); }
            if (f.needParse) { refreshFileMeta(f); f.needParse = false; }
            continue;
        }
        if (f.needParse == false) { continue; }
        if (_fs->exists(f.name) == false) {
            f.err = "File is missing";
            f.needParse = false;
            unregisterFile(f);
            continue;
        }
        registerFile(f);
        done++;
        if (done >= MACRO_PARSE_PER_TICK) { break; }
    }
}

bool CLASS_MODULE_MACROS::anyNeedParse() {
    for (uint8_t i = 0; i < _fileCount; i++) {
        if (_files[i].run && _files[i].needParse) { return true; }
    }
    return false;
}

// ============================================================
// Подписки на события шины
// ============================================================

static int macroNoopCb(void* user, int argc, const BusValue* argv, BusValue& result) {
    (void)user; (void)argc; (void)argv; (void)result;
    return BUS_OK;
}

static int macroEventTramp(void* user, int argc, const BusValue* argv, BusValue& result) {
    (void)user; (void)result;
    if (argc < 1) { return BUS_ERR_BAD_ARGC; }
    BusValue v = (argc >= 2) ? argv[1] : BusValue();
    module_macros.fireToken(MACRO_H_ON, argv[0].s, v);
    return BUS_OK;
}

void CLASS_MODULE_MACROS::ensureEventSub(const String& name) {
    for (uint8_t i = 0; i < _nEvtSubs; i++) {
        if (_evtSubs[i].name == name) { return; }
    }
    if (_nEvtSubs >= MACRO_MAX_EVENT_SUBS) { return; }
    uint32_t h = core_state.on(name.c_str(), macroEventTramp, nullptr);
    if (h == 0) { return; }
    _evtSubs[_nEvtSubs].name = name;
    _evtSubs[_nEvtSubs].handle = h;
    _nEvtSubs++;
}

void CLASS_MODULE_MACROS::pruneEventSubs() {
    for (int i = (int)_nEvtSubs - 1; i >= 0; i--) {
        bool used = false;
        for (uint8_t fi = 0; fi < _fileCount && !used; fi++) {
            MacroFile& f = _files[fi];
            if (f.active == false || f.handlers == NULL) { continue; }
            for (uint8_t hi = 0; hi < f.nHandlers; hi++) {
                if (f.handlers[hi].type == MACRO_H_ON && f.handlers[hi].key == _evtSubs[i].name) {
                    used = true; break;
                }
            }
        }
        if (used == false) {
            core_state.off(_evtSubs[i].handle);
            for (uint8_t j = i; j + 1 < _nEvtSubs; j++) { _evtSubs[j] = _evtSubs[j + 1]; }
            _nEvtSubs--;
        }
    }
}

// ============================================================
// Исполнительная машина
// ============================================================

void CLASS_MODULE_MACROS::drainEvents() {
    if (_lua == NULL) { return; }
    lua_State* L = _lua->getLuaState();
    if (L == NULL) { return; }

    while (_evIn != _evOut) {
        MacroEvent ev = _evQueue[_evOut];
        _evOut = (_evOut + 1) % MACRO_EV_QUEUE;

        const char* typeName =
            (ev.type == MACRO_H_TERM) ? "term" :
            (ev.type == MACRO_H_BUTTON) ? "button" : "on";

        for (uint8_t i = 0; i < _fileCount; i++) {
            MacroFile& f = _files[i];
            if (f.run == false || f.active == false || f.handlers == NULL) { continue; }
            for (uint8_t j = 0; j < f.nHandlers; j++) {
                MacroHandler& h = f.handlers[j];
                if (h.type != ev.type || h.key != ev.spec) { continue; }
                const BusValue* vp = (ev.type == MACRO_H_ON) ? &ev.value : nullptr;
                macroCallHandler(L, f, h, typeName, vp);
            }
        }
    }
}

void CLASS_MODULE_MACROS::tickStep() {
    MacroLockGuard lk(_lock);   // сериализация с веб-обработчиками (AsyncTCP)

    rebuildScripts();

    if (_config.enabled == false) { return; }

    drainEvents();

    bool ntpSync = (NTP.getLastNTPSync() > 0);
    if (ntpSync && !_ntpWasSynced) {
        _ntpWasSynced = true;
        DEBUGMACROS("[MACRO] NTP synced, cron activated\r\n");
        for (uint8_t i = 0; i < _fileCount; i++) {
            if (_files[i].created < MACRO_CREATED_MIN) {
                _files[i].created = (uint32_t)now();
            }
        }
    }

    if (_lua == NULL) { return; }
    lua_State* L = _lua->getLuaState();
    if (L == NULL) { return; }

    // Скачок времени: cron-расписания пересчитываются без выполнения.
    time_t tNow = (time_t)now();
    int64_t tickDiff = (int64_t)tNow - (int64_t)_lastTickNow;
    bool bigJump = (_lastTickNow == 0)
        || (tickDiff > MACRO_CRON_JUMP_MAX_S) || (tickDiff < -MACRO_CRON_JUMP_MAX_S);
    time_t jumpBase = (tNow > _lastTickNow) ? tNow : _lastTickNow;
    _lastTickNow = tNow;

    // 1) Тела сценариев — round-robin, не более MACRO_BODY_PER_TICK за тик.
    // Тело перерегистрирует обработчики и выполняет реактивные операторы; при
    // большом числе активных сценариев нагрузка на loop-задачу ограничена.
    if (_fileCount > 0) {
        for (uint8_t k = 0; k < MACRO_BODY_PER_TICK; k++) {
            uint8_t i = _bodyCursor;
            _bodyCursor = (uint8_t)((_bodyCursor + 1) % _fileCount);
            MacroFile& f = _files[i];
            if (f.run == false || f.active == false || f.handlers == NULL) { continue; }
            macroRunBody(L, f);
        }
    }

    // 2) Обработчики и расписания — для всех активных файлов на каждом тике.
    uint32_t ms = millis();
    for (uint8_t i = 0; i < _fileCount; i++) {
        MacroFile& f = _files[i];
        if (f.run == false || f.active == false || f.handlers == NULL) { continue; }

        if (bigJump) {
            for (uint8_t j = 0; j < f.nHandlers; j++) {
                MacroHandler& h = f.handlers[j];
                if (h.type != MACRO_H_CRON) { continue; }
                if (h.next == 0 || h.next == (time_t)-1) { continue; }
                h.next = cron_next(&h.expr, jumpBase);
            }
        }

        // 2a) tick-обработчики
        for (uint8_t j = 0; j < f.nHandlers; j++) {
            if (f.handlers[j].type == MACRO_H_TICK) {
                macroCallHandler(L, f, f.handlers[j], "tick", nullptr);
            }
        }

        // 2b) расписания (cron/interval/after/once)
        for (uint8_t j = 0; j < f.nHandlers;) {
            MacroHandler& h = f.handlers[j];
            bool fired = false;
            bool oneShot = (h.type == MACRO_H_AFTER || h.type == MACRO_H_ONCE);

            if (h.type == MACRO_H_CRON) {
                if (ntpSync) {
                    if (h.next == 0) {
                        h.next = cron_next(&h.expr, tNow);
                    } else if (h.next != (time_t)-1 && tNow >= h.next) {
                        macroCallHandler(L, f, h, "cron", nullptr);
                        h.next = cron_next(&h.expr, tNow);
                        fired = true;
                    }
                }
            } else if (h.type == MACRO_H_INTERVAL || h.type == MACRO_H_AFTER) {
                if ((int32_t)(ms - h.dueMs) >= 0) {
                    macroCallHandler(L, f, h, (h.type == MACRO_H_AFTER) ? "after" : "every", nullptr);
                    fired = true;
                }
            } else if (h.type == MACRO_H_ONCE) {
                macroCallHandler(L, f, h, "once", nullptr);
                fired = true;
            }

            if (fired && oneShot) { macroRemoveHandler(L, f, j); continue; }
            if (fired && h.type == MACRO_H_INTERVAL) {
                h.dueMs = ms + (uint32_t)h.key.toInt() * 1000UL;
            }
            j++;
        }
    }
}
