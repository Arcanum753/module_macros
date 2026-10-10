// ============================================================
// module_macros_engine.cpp — «барабан» сценариев:
// один Lua-интерпретатор + декларативные правила
// (cron/cond/term/button/on/body) через ресурсную шину core_state.
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
static uint32_t macroLuaSteps = 0;

// При валидации сценария (/macros/validate) bus-функции set/call/mode — no-op,
// чтобы top-level чанка не давал побочных эффектов на шину.
static bool macroValidateNoSideEffects = false;

// Свободный хип для диагностики (ESP32-only модуль).
static unsigned macroHeapFree() {
    unsigned v = 0;
#if defined(ESP32)
    v = (unsigned)ESP.getFreeHeap();
#endif
    return v;
}

// Форвард-объявления статических помощников
static void macroLuaHook(lua_State* L, lua_Debug* ar);
static String macroLuaPopError(lua_State* L);
static void macroPushBusValue(lua_State* L, const BusValue& v);
static bool macroLuaToBusValue(lua_State* L, int idx, BusValue& out);
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
static int macroLoadChunk(lua_State* L, MacroFile& f, const String& content);
static const char* macroRuleTypeName(uint8_t type);
static void macroPushEventTable(lua_State* L, MacroRule& r, const BusValue* value);
static bool macroReadRes(const String& name, BusValue& out);
static bool macroCompare(const BusValue& cur, uint8_t op, const BusValue& want);
static uint8_t macroOpCode(const char* s);
static int macroParseArgs(const String& s, BusValue* out, int max);
static String macroArgsToStr(lua_State* L, int argsIdx);
static String macroValueToStr(lua_State* L, int idx);
static bool macroParseRule(lua_State* L, MacroFile& f, int tidx, MacroRule& r);
static String macroNumText(const BusValue& v);
static int macroResolveEnum(const char* res, const BusValue& in, BusValue& out, String& err);

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
// Песочница и загрузка чанка
// ============================================================

// Кладёт на стек свежее окружение (_ENV): запись — в свою таблицу,
// чтение — через __index в глобальную таблицу (там наши C-функции).
static void macroPushSandbox(lua_State* L) {
    lua_newtable(L);              // env
    lua_newtable(L);              // metatable
    lua_pushglobaltable(L);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
}

// Обёртка для ПРОТЕКТИРОВАННОЙ установки песочницы: вызывается под lua_pcall,
// чтобы аллокации таблиц не улетали вне защиты (иначе память → abort()).
// Аргумент 1 — чанк; возвращает тот же чанк с подставленным _ENV.
static int macroChunkSetup(lua_State* L) {
    macroPushSandbox(L);
    lua_setupvalue(L, 1, 1);      // chunk._ENV = env
    lua_pushvalue(L, 1);          // вернуть чанк
    return 1;
}

// Загружает content как чанк с песочницей и исполняет; при успехе
// на стеке остаётся одно возвращённое значение (таблица).
static int macroLoadChunk(lua_State* L, MacroFile& f, const String& content) {
    if (L == NULL) { f.err = "No interpreter"; return LUA_ERRRUN; }

    String cn = "@";
    cn += f.name;

    macroLuaSteps = 0;
    lua_sethook(L, macroLuaHook, LUA_MASKCOUNT, MACRO_LUA_HOOK_N);

    int status = luaL_loadbuffer(L, content.c_str(), content.length(), cn.c_str());
    if (status == LUA_OK) {
        // Песочница ставится в защищённом вызове (аллокации таблиц вне pcall → abort).
        lua_pushcfunction(L, macroChunkSetup);
        lua_insert(L, -2);                 // [setup, chunk]
        status = lua_pcall(L, 1, 1, 0);    // -> [chunk]
    }
    if (status == LUA_OK) {
        status = lua_pcall(L, 0, 1, 0);    // исполняем чанк -> [result]
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
    // lua_type, а не lua_isnumber: lua_isnumber() истинно и для числовых СТРОК,
    // из-за чего "0"/"1" превращались в F32 и STR-функции (wifi.set_slot) получали пустую строку.
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

// Форматтер числового/булева значения для сообщения об ошибке ENUM.
// Контракт: вызывается ТОЛЬКО для kind == I32/F32/BOOL/TIME.
// Для прочих kind возвращает "?" (защита от случайного вызова).
static String macroNumText(const BusValue& v) {
    switch (v.kind) {
        case BusValue::I32:  return String((long)v.i);
        case BusValue::TIME: return String((long)v.t);
        case BusValue::BOOL: return String(v.b ? "true" : "false");
        case BusValue::F32:  return String(v.f, 3);
        default:             return String("?");
    }
}

// Резолв значения для ENUM-ресурса: строка → индекс по enumVals (без учёта регистра),
// ENUM → как есть, числовые (I32/F32/BOOL/TIME) → ошибка.
// rc: BUS_OK / BUS_ERR_BAD_VALUE; err — человекочитаемая причина.
static int macroResolveEnum(const char* res, const BusValue& in, BusValue& out, String& err) {
    err = "";
    if (in.kind == BusValue::ENUM) { out = in; return BUS_OK; }
    if (in.kind == BusValue::STR) {
        int idx = core_state.enumIndexByName(res, in.s.c_str(), nullptr);
        if (idx < 0) {
            err = String(res) + ": unknown enum value '" + in.s + "'";
            return BUS_ERR_BAD_VALUE;
        }
        out = BusValue::en((int32_t)idx);
        return BUS_OK;
    }
    err = String(res) + ": enum requires name, got number " + macroNumText(in);
    return BUS_ERR_BAD_VALUE;
}

// ============================================================
// Команды Lua, доступные внутри named-handler
// ============================================================

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

static int macroLuaSet(lua_State* L) {
    if (macroValidateNoSideEffects) { lua_pushinteger(L, BUS_OK); return 1; }
    const char* name = luaL_checkstring(L, 1);
    if (core_state.has(name) == false) { lua_pushinteger(L, BUS_ERR_NOT_FOUND); return 1; }
    BusResInfo info;
    core_state.info(name, info);
    BusValue v;
    if (lua_gettop(L) >= 2) { macroLuaToBusValue(L, 2, v); }

    // ENUM: строку резолвим в индекс, числа отвергаем (BUS_ERR_BAD_VALUE).
    if (info.kind == BusValue::ENUM) {
        BusValue rv;
        String e;
        if (macroResolveEnum(name, v, rv, e) != BUS_OK) {
            lua_pushinteger(L, BUS_ERR_BAD_VALUE);
            return 1;
        }
        v = rv;
    }

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

static int macroLuaCall(lua_State* L) {
    if (macroValidateNoSideEffects) {
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

    // ENUM-функция (dual state+func, например wifi.mode): первый аргумент-строку
    // резолвим в индекс; числа отвергаем.
    BusResInfo info;
    bool haveInfo = (core_state.info(name, info) == BUS_OK);
    if (haveInfo && argc >= 1 && info.kind == BusValue::ENUM) {
        BusValue rv;
        String e;
        if (macroResolveEnum(name, args[0], rv, e) != BUS_OK) {
            lua_pushnil(L);
            lua_pushinteger(L, BUS_ERR_BAD_VALUE);
            return 2;
        }
        args[0] = rv;
    }

    // call на чистом state = set (идентично декларативному call/calls).
    if (haveInfo && !info.isFunc && info.kind != BusValue::NONE) {
        BusValue v = (argc > 0) ? args[0] : BusValue();
        int src = BUS_ERR_BAD_TYPE;
        switch (info.kind) {
            case BusValue::BOOL: src = core_state.setBool(name, (v.kind == BusValue::BOOL) ? v.b : (v.i != 0)); break;
            case BusValue::I32:  src = core_state.setInt(name, (int32_t)v.i); break;
            case BusValue::ENUM: src = core_state.setInt(name, (int32_t)v.i); break;
            case BusValue::F32:  src = core_state.setF32(name, v.f); break;
            case BusValue::STR:  src = core_state.setStr(name, v.s); break;
            case BusValue::TIME: src = core_state.setTime(name, v.t); break;
            default: break;
        }
        // Всегда (value, rc): успех тоже возвращает код, чтобы `v, rc = call(...)`
        // давал rc без ветвления в сценарии.
        BusValue rv = core_state.valueToKind(v, info.kind);
        macroPushBusValue(L, rv);
        lua_pushinteger(L, src);
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
        if (macroValidateNoSideEffects) { lua_pushinteger(L, BUS_OK); return 1; }
        int m = (int)luaL_checkinteger(L, 2);
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
// Сериализация значений и аргументов
// ============================================================

static String macroValueToStr(lua_State* L, int idx) {
    // lua_type, а не lua_isnumber: числовая строка ("0") должна остаться s:0 (STR),
    // иначе STR-функции шины (wifi.set_slot) получают F32 и читают пустую строку.
    int t = lua_type(L, idx);
    if (t == LUA_TBOOLEAN) { return String(lua_toboolean(L, idx) ? "b:1" : "b:0"); }
    if (t == LUA_TNUMBER) {
        if (lua_isinteger(L, idx)) {
            char b[24]; snprintf(b, sizeof(b), "i:%ld", (long)lua_tointeger(L, idx)); return String(b);
        }
        char b[32]; snprintf(b, sizeof(b), "f:%.4f", (double)lua_tonumber(L, idx)); return String(b);
    }
    if (t == LUA_TSTRING) { return String("s:") + String(lua_tostring(L, idx)); }
    return String("s:");
}

static String macroArgsToStr(lua_State* L, int argsIdx) {
    String out;
    if (!lua_istable(L, argsIdx)) { return out; }
    argsIdx = lua_absindex(L, argsIdx);
    int n = (int)lua_rawlen(L, argsIdx);
    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, argsIdx, i);
        if (i > 1) { out += '|'; }
        out += macroValueToStr(L, -1);
        lua_pop(L, 1);
    }
    return out;
}

static int macroStrToValue(const String& token, BusValue& out) {
    if (token.startsWith("b:")) { out = BusValue::bo(token.charAt(2) == '1'); return 1; }
    if (token.startsWith("i:")) { out = BusValue::i32((int32_t)token.substring(2).toInt()); return 1; }
    if (token.startsWith("f:")) { out = BusValue::f32(token.substring(2).toFloat()); return 1; }
    if (token.startsWith("s:")) { out = BusValue::str(token.substring(2)); return 1; }
    out = BusValue::str(token);
    return 1;
}

static int macroParseArgs(const String& s, BusValue* out, int max) {
    int n = 0;
    int start = 0;
    while (n < max) {
        int sep = s.indexOf('|', start);
        String tok = (sep < 0) ? s.substring(start) : s.substring(start, sep);
        if (tok.length() > 0) { macroStrToValue(tok, out[n]); n++; }
        if (sep < 0) { break; }
        start = sep + 1;
        if (start > (int)s.length()) { break; }
    }
    return n;
}

static uint8_t macroOpCode(const char* s) {
    if (s == NULL) { return MACRO_COND_EQ; }
    if (!strcmp(s, "==") || !strcmp(s, "=")) { return MACRO_COND_EQ; }
    if (!strcmp(s, "!=")) { return MACRO_COND_NE; }
    if (!strcmp(s, "<"))  { return MACRO_COND_LT; }
    if (!strcmp(s, "<=")) { return MACRO_COND_LE; }
    if (!strcmp(s, ">"))  { return MACRO_COND_GT; }
    if (!strcmp(s, ">=")) { return MACRO_COND_GE; }
    if (!strcmp(s, "changed")) { return MACRO_COND_CHANGED; }
    return MACRO_COND_EQ;
}

// Текстовое имя оператора cond (обратное к macroOpCode).
const char* macroCondOpName(uint8_t op) {
    switch (op) {
        case MACRO_COND_EQ:      return "==";
        case MACRO_COND_NE:      return "!=";
        case MACRO_COND_LT:      return "<";
        case MACRO_COND_LE:      return "<=";
        case MACRO_COND_GT:      return ">";
        case MACRO_COND_GE:      return ">=";
        case MACRO_COND_CHANGED: return "changed";
        default:                 return "?";
    }
}

static const char* macroRuleTypeName(uint8_t type) {
    switch (type) {
        case MACRO_RULE_CRON:   return "cron";
        case MACRO_RULE_COND:   return "cond";
        case MACRO_RULE_TERM:   return "term";
        case MACRO_RULE_BUTTON: return "button";
        case MACRO_RULE_EVENT:  return "on";
        default:                return "body";
    }
}

// Внешний доступ к имени типа правила (для printList/логов).
const char* macroRuleTypeStr(uint8_t type) {
    return macroRuleTypeName(type);
}

// Отладочная сериализация cond-условий правила (собирается на месте).
String macroRuleCondStr(const MacroRule& r) {
    String out;
    for (uint8_t i = 0; i < r.nConds; i++) {
        if (i > 0) { out += ';'; }
        out += r.conds[i].res;
        out += '|';
        out += macroCondOpName(r.conds[i].op);
        if (r.conds[i].op != MACRO_COND_CHANGED) {
            out += '|';
            const BusValue& v = r.conds[i].val;
            switch (v.kind) {
                case BusValue::BOOL: out += (v.b ? "true" : "false"); break;
                case BusValue::I32:  out += String((long)v.i); break;
                case BusValue::ENUM: {
                    const char* nm = core_state.enumNameByIndex(r.conds[i].res.c_str(), (int)v.i);
                    if (nm != nullptr) { out += nm; } else { out += String((long)v.i); }
                    break;
                }
                case BusValue::F32:  out += String(v.f); break;
                case BusValue::STR:  out += v.s; break;
                case BusValue::TIME: out += String((long)v.t); break;
                default:             out += "-"; break;
            }
        }
    }
    return out;
}

// ============================================================
// Чтение/сравнение ресурсов (декларативные условия)
// ============================================================

static bool macroReadRes(const String& name, BusValue& out) {
    if (core_state.has(name.c_str()) == false) { return false; }
    BusResInfo info;
    core_state.info(name.c_str(), info);
    switch (info.kind) {
        case BusValue::BOOL: out = BusValue::bo(core_state.getBool(name.c_str())); break;
        case BusValue::I32:  out = BusValue::i32(core_state.getInt(name.c_str())); break;
        case BusValue::ENUM: out = BusValue::en(core_state.getInt(name.c_str())); break;
        case BusValue::F32:  out = BusValue::f32(core_state.getF32(name.c_str())); break;
        case BusValue::STR:  out = BusValue::str(core_state.getStr(name.c_str())); break;
        case BusValue::TIME: out = BusValue::tm(core_state.getTime(name.c_str())); break;
        default: return false;
    }
    return true;
}

static bool macroCompare(const BusValue& cur, uint8_t op, const BusValue& want) {
    if (op == MACRO_COND_CHANGED) { return false; }

    if (cur.kind == BusValue::STR || cur.kind == BusValue::NONE ||
        want.kind == BusValue::STR || want.kind == BusValue::NONE) {
        const char* a = (cur.kind == BusValue::STR) ? cur.s.c_str() : "";
        const char* b = (want.kind == BusValue::STR) ? want.s.c_str() : "";
        if (op == MACRO_COND_EQ) { return strcmp(a, b) == 0; }
        if (op == MACRO_COND_NE) { return strcmp(a, b) != 0; }
        return false;
    }

    double a = (cur.kind == BusValue::F32) ? (double)cur.f
             : (cur.kind == BusValue::BOOL) ? (cur.b ? 1.0 : 0.0)
             : (cur.kind == BusValue::TIME) ? (double)cur.t : (double)cur.i;
    double b = (want.kind == BusValue::F32) ? (double)want.f
             : (want.kind == BusValue::BOOL) ? (want.b ? 1.0 : 0.0)
             : (want.kind == BusValue::TIME) ? (double)want.t : (double)want.i;
    switch (op) {
        case MACRO_COND_EQ: return a == b;
        case MACRO_COND_NE: return a != b;
        case MACRO_COND_LT: return a < b;
        case MACRO_COND_LE: return a <= b;
        case MACRO_COND_GT: return a > b;
        case MACRO_COND_GE: return a >= b;
        default: return false;
    }
}

// ============================================================
// Разбор таблицы сценария (новый формат)
// ============================================================

// Добавляет одно cond-условие из таблицы idx в структурный массив правила.
static bool macroCondAddOne(lua_State* L, MacroFile& f, int idx, MacroRule& r) {
    if (r.nConds >= MACRO_MAX_CONDS) { f.err = String("too many conds (max ") + String((unsigned)MACRO_MAX_CONDS) + ")"; return false; }
    idx = lua_absindex(L, idx);

    lua_getfield(L, idx, "res");
    String res = lua_isstring(L, -1) ? String(lua_tostring(L, -1)) : String();
    lua_pop(L, 1);
    if (res.length() == 0) { f.err = "cond: res required"; return false; }

    lua_getfield(L, idx, "op");
    String op = lua_isstring(L, -1) ? String(lua_tostring(L, -1)) : String("==");
    lua_pop(L, 1);
    uint8_t opc = macroOpCode(op.c_str());

    MacroCond& c = r.conds[r.nConds];
    c.res = res;
    c.op = opc;
    c.val = BusValue();

    if (opc != MACRO_COND_CHANGED) {
        lua_getfield(L, idx, "val");
        if (macroLuaToBusValue(L, lua_gettop(L), c.val) == false) {
            if (lua_isstring(L, -1)) { c.val = BusValue::str(String(lua_tostring(L, -1))); }
        }
        lua_pop(L, 1);

        // Best-effort: для уже зарегистрированного ENUM-ресурса резолвим значение
        // при парсинге (ранняя диагностика); иначе — резолв при исполнении.
        if (core_state.has(res.c_str())) {
            BusResInfo info;
            core_state.info(res.c_str(), info);
            if (info.kind == BusValue::ENUM) {
                BusValue rv;
                String e;
                if (macroResolveEnum(res.c_str(), c.val, rv, e) != BUS_OK) {
                    f.err = e;
                    return false;
                }
                c.val = rv;
            }
        }
    }
    r.nConds++;
    return true;
}

// cond = {res=,op=,val=} либо { {..}, {..} } (AND), до MACRO_MAX_CONDS.
static bool macroParseConds(lua_State* L, MacroFile& f, int condIdx, MacroRule& r) {
    lua_getfield(L, condIdx, "res");
    bool single = lua_isstring(L, -1);
    lua_pop(L, 1);

    if (single) { return macroCondAddOne(L, f, condIdx, r); }

    int n = (int)lua_rawlen(L, condIdx);
    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, condIdx, i);
        bool ok = false;
        if (lua_istable(L, -1)) { ok = macroCondAddOne(L, f, lua_absindex(L, -1), r); }
        lua_pop(L, 1);
        if (!ok) { return false; }
    }
    return true;
}

// Разбирает одно правило таблицы в MacroRule. Возвращает true при успехе.
static bool macroParseRule(lua_State* L, MacroFile& f, int tidx, MacroRule& r) {
    tidx = lua_absindex(L, tidx);

    r.type = MACRO_RULE_BODY;
    r.spec = "";
    r.next = 0;
    r.lastCond = false;
    r.nConds = 0;
    for (uint8_t i = 0; i < MACRO_MAX_CONDS; i++) {
        r.conds[i] = MacroCond();
        r.lastVals[i] = "";
        r.haveLast[i] = false;
    }
    r.setTarget = "";
    r.setValue = BusValue();
    r.handler = "";
    r.handlerArgs = "";
    r.nActions = 0;
    r.sub = 0;
    memset(&r.expr, 0, sizeof(r.expr));

    // Триггеры допустимы только внутри when = {...} (плоский формат не поддерживается).
    const char* trigs[] = { "cron", "cond", "term", "button", "on" };
    for (uint8_t i = 0; i < 5; i++) {
        lua_getfield(L, tidx, trigs[i]);
        bool present = lua_isstring(L, -1) || lua_istable(L, -1);
        lua_pop(L, 1);
        if (present) {
            f.err = "triggers must be inside when = {...}";
            return false;
        }
    }

    // --- when: составное условие (AND) ---
    lua_getfield(L, tidx, "when");
    if (lua_istable(L, -1)) {
        int widx = lua_absindex(L, -1);

        // Первичный триггер: максимум один из cron/term/button/on.
        uint8_t primaries = 0;
        lua_getfield(L, widx, "cron");
        if (lua_isstring(L, -1)) { r.type = MACRO_RULE_CRON; r.spec = String(lua_tostring(L, -1)); primaries++; }
        lua_pop(L, 1);
        lua_getfield(L, widx, "term");
        if (lua_isstring(L, -1)) { r.type = MACRO_RULE_TERM; r.spec = String(lua_tostring(L, -1)); primaries++; }
        lua_pop(L, 1);
        lua_getfield(L, widx, "button");
        if (lua_isstring(L, -1)) { r.type = MACRO_RULE_BUTTON; r.spec = String(lua_tostring(L, -1)); primaries++; }
        lua_pop(L, 1);
        lua_getfield(L, widx, "on");
        if (lua_isstring(L, -1)) { r.type = MACRO_RULE_EVENT; r.spec = String(lua_tostring(L, -1)); primaries++; }
        lua_pop(L, 1);

        if (primaries > 1) {
            lua_pop(L, 1);
            f.err = "when: at most one of cron/term/button/on";
            return false;
        }

        // Проверочные cond-условия (0..MACRO_MAX_CONDS).
        lua_getfield(L, widx, "cond");
        if (lua_istable(L, -1)) {
            bool ok = macroParseConds(L, f, lua_absindex(L, -1), r);
            lua_pop(L, 1);
            if (!ok) { lua_pop(L, 1); return false; }
        } else {
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    // Только cond-условия без первичного триггера — правило-наблюдатель (фронт).
    if (r.type == MACRO_RULE_BODY && r.nConds > 0) { r.type = MACRO_RULE_COND; }

    // Разбор cron-выражения первичного триггера.
    if (r.type == MACRO_RULE_CRON) {
        const char* perr = NULL;
        cron_parse_expr(r.spec.c_str(), &r.expr, &perr);
        if (perr) { f.err = String("cron error: ") + perr; return false; }
    }

    // --- действие: ровно одно из run/call/calls/set ---
    uint8_t nAct = 0;
    uint8_t actKind = 0;   // 1=run, 2=call, 3=calls, 4=set

    lua_getfield(L, tidx, "run");
    if (lua_isstring(L, -1)) { actKind = 1; nAct++; r.handler = String(lua_tostring(L, -1)); }
    lua_pop(L, 1);

    lua_getfield(L, tidx, "call");
    if (lua_isstring(L, -1)) { actKind = 2; nAct++; }
    lua_pop(L, 1);

    lua_getfield(L, tidx, "calls");
    if (lua_istable(L, -1)) { actKind = 3; nAct++; }
    lua_pop(L, 1);

    lua_getfield(L, tidx, "set");
    if (lua_isstring(L, -1)) { actKind = 4; nAct++; }
    lua_pop(L, 1);

    if (nAct == 0) {
        f.err = String("rule has no action (set/call/calls/run): ") + macroRuleTypeName(r.type);
        return false;
    }
    if (nAct > 1) { f.err = "rule has multiple actions"; return false; }

    if (actKind == 1) {
        lua_getfield(L, tidx, "args");
        if (lua_istable(L, -1)) { r.handlerArgs = macroArgsToStr(L, lua_absindex(L, -1)); }
        lua_pop(L, 1);
        return true;
    }

    if (actKind == 4) {
        lua_getfield(L, tidx, "set");
        r.setTarget = String(lua_tostring(L, -1));
        lua_pop(L, 1);

        lua_getfield(L, tidx, "value");
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            f.err = "set: value required";
            return false;
        }
        bool ok = macroLuaToBusValue(L, lua_gettop(L), r.setValue);
        lua_pop(L, 1);
        if (!ok) { f.err = "set: bad value"; return false; }

        // Проверка ресурса (best-effort): чистая функция/событие — не значение.
        if (core_state.has(r.setTarget.c_str())) {
            BusResInfo info;
            core_state.info(r.setTarget.c_str(), info);
            if (info.kind == BusValue::NONE) {
                f.err = String("set: use call= for function resource ") + r.setTarget;
                return false;
            }
            if (info.kind == BusValue::ENUM) {
                BusValue rv;
                String e;
                if (macroResolveEnum(r.setTarget.c_str(), r.setValue, rv, e) != BUS_OK) {
                    f.err = e;
                    return false;
                }
                r.setValue = rv;
            }
        }
        return true;
    }

    if (actKind == 3) {
        // calls = { {name=,args=}, ... } либо { {"name", {args}}, ... }
        lua_getfield(L, tidx, "calls");
        int cidx = lua_absindex(L, -1);
        int n = (int)lua_rawlen(L, cidx);
        if (n > MACRO_MAX_ACTIONS) {
            lua_pop(L, 1);
            f.err = String("too many calls (max ") + String((unsigned)MACRO_MAX_ACTIONS) + ")";
            return false;
        }
        for (int i = 1; i <= n && r.nActions < MACRO_MAX_ACTIONS; i++) {
            lua_rawgeti(L, cidx, i);
            if (lua_istable(L, -1)) {
                int eidx = lua_absindex(L, -1);
                String name;
                String args;
                lua_getfield(L, eidx, "name");
                if (lua_isstring(L, -1)) { name = String(lua_tostring(L, -1)); }
                lua_pop(L, 1);
                lua_getfield(L, eidx, "args");
                if (lua_istable(L, -1)) { args = macroArgsToStr(L, lua_absindex(L, -1)); }
                lua_pop(L, 1);
                if (name.length() == 0) {
                    // Форма {"name", {args}}
                    lua_rawgeti(L, eidx, 1);
                    if (lua_isstring(L, -1)) { name = String(lua_tostring(L, -1)); }
                    lua_pop(L, 1);
                    lua_rawgeti(L, eidx, 2);
                    if (lua_istable(L, -1)) { args = macroArgsToStr(L, lua_absindex(L, -1)); }
                    lua_pop(L, 1);
                }
                if (name.length() > 0) {
                    // Best-effort (только диагностика): числовой/невалидный ENUM-аргумент
                    // отсекаем при парсинге; r.actions[] не трогаем (резолв при исполнении).
                    if (args.length() > 0 && core_state.has(name.c_str())) {
                        BusResInfo info;
                        core_state.info(name.c_str(), info);
                        if (info.kind == BusValue::ENUM) {
                            BusValue av[MACRO_CALL_MAX_ARGS];
                            int an = macroParseArgs(args, av, MACRO_CALL_MAX_ARGS);
                            if (an > 0) {
                                BusValue rv;
                                String e;
                                if (macroResolveEnum(name.c_str(), av[0], rv, e) != BUS_OK) {
                                    lua_pop(L, 2);
                                    f.err = e;
                                    return false;
                                }
                            }
                        }
                    }
                    String act = name;
                    if (args.length() > 0) { act += '|'; act += args; }
                    r.actions[r.nActions++] = act;
                }
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        if (r.nActions == 0) { f.err = "calls: empty"; return false; }
        return true;
    }

    // actKind == 2: одиночный call = "ns.func"
    lua_getfield(L, tidx, "call");
    String cname = String(lua_tostring(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, tidx, "args");
    String cargs;
    if (lua_istable(L, -1)) { cargs = macroArgsToStr(L, lua_absindex(L, -1)); }
    lua_pop(L, 1);

    // Best-effort (только диагностика): невалидный ENUM-аргумент — ошибка правила.
    if (cargs.length() > 0 && core_state.has(cname.c_str())) {
        BusResInfo info;
        core_state.info(cname.c_str(), info);
        if (info.kind == BusValue::ENUM) {
            BusValue av[MACRO_CALL_MAX_ARGS];
            int an = macroParseArgs(cargs, av, MACRO_CALL_MAX_ARGS);
            if (an > 0) {
                BusValue rv;
                String e;
                if (macroResolveEnum(cname.c_str(), av[0], rv, e) != BUS_OK) {
                    f.err = e;
                    return false;
                }
            }
        }
    }
    String act = cname;
    if (cargs.length() > 0) { act += '|'; act += cargs; }
    r.actions[r.nActions++] = act;
    return true;
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
    bool reg =
        _lua->registerFunction("get",   macroLuaGet)  &&
        _lua->registerFunction("set",   macroLuaSet)  &&
        _lua->registerFunction("call",  macroLuaCall) &&
        _lua->registerFunction("has",   macroLuaHas)  &&
        _lua->registerFunction("mode",  macroLuaMode) &&
        _lua->registerFunction("info",  macroLuaInfo) &&
        _lua->registerFunction("puts",  macroLuaPuts) &&
        _lua->registerFunction("print", macroLuaPuts) &&
        _lua->registerFunction("clock", macroLuaClock)&&
        _lua->registerFunction("now",   macroLuaNow);
    if (reg == false) {
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

/** Освобождает правила файла и снимает неиспользуемые подписки. */
void CLASS_MODULE_MACROS::unregisterFile(MacroFile& f) {
    if (f.rules != NULL) { delete[] f.rules; f.rules = NULL; }
    f.nRules = 0;
    f.active = false;
    pruneEventSubs();
}

/** Регистрирует файл: читает его, разбирает rules в «барабане» и закрывает. */
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
    int status = macroLoadChunk(L, f, content);
    if (status != LUA_OK) {
        if (f.err.length() == 0) { f.err = "Scenario parse error"; }
        DEBUGMACROS("%s: %s -> %s\r\n", __FUNCTION__, f.name.c_str(), f.err.c_str());
        return false;
    }

    if (!lua_istable(L, -1)) {
        f.err = "legacy/invalid format: expected table {handlers,rules}";
        DEBUGMACROS("%s: %s\r\n", __FUNCTION__, f.err.c_str());
        lua_settop(L, 0);
        return false;
    }
    int tbl = lua_absindex(L, -1);

    lua_getfield(L, tbl, "desc");
    if (lua_isstring(L, -1)) { f.desc = String(lua_tostring(L, -1)); }
    lua_pop(L, 1);

    lua_getfield(L, tbl, "meta_cron");
    if (lua_isstring(L, -1)) { f.metaCron = String(lua_tostring(L, -1)); }
    else { f.metaCron = ""; }
    lua_pop(L, 1);

    lua_getfield(L, tbl, "rules");
    if (!lua_istable(L, -1)) {
        f.err = "no rules table";
        DEBUGMACROS("%s: %s\r\n", __FUNCTION__, f.err.c_str());
        lua_settop(L, 0);
        return false;
    }
    int rt = lua_absindex(L, -1);
    int n = (int)lua_rawlen(L, rt);
    if (n <= 0) {
        f.err = "no rules";
        lua_settop(L, 0);
        return false;
    }
    if (n > MACRO_MAX_RULES) {
        f.err = String("too many rules (max ") + String((unsigned)MACRO_MAX_RULES) + ")";
        DEBUGMACROS("%s: %s\r\n", __FUNCTION__, f.err.c_str());
        lua_settop(L, 0);
        return false;
    }

    f.rules = new (std::nothrow) MacroRule[n];
    if (f.rules == NULL) {
        f.err = "No memory for rules";
        f.parseFails++;
        if (f.parseFails < MACRO_PARSE_MAX_RETRY) { f.needParse = true; }
        lua_settop(L, 0);
        return false;
    }
    f.nRules = 0;

    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, rt, i);
        if (lua_istable(L, -1)) {
            MacroRule& r = f.rules[f.nRules];
            if (macroParseRule(L, f, lua_absindex(L, -1), r)) {
                f.nRules++;
            } else {
                DEBUGMACROS("%s: %s rule %d: %s\r\n", __FUNCTION__, f.name.c_str(), i, f.err.c_str());
                break;
            }
        }
        lua_pop(L, 1);
    }
    lua_settop(L, 0);

    if (f.nRules == 0) {
        if (f.err.length() == 0) { f.err = "no valid rules"; }
        unregisterFile(f);
        return false;
    }

    // Мета-cron (гейт окна). Невалидный cron → скрипт не исполняется.
    f.metaValid = true;
    f.metaInit = false;
    f.metaNext = 0;
    memset(&f.metaExpr, 0, sizeof(f.metaExpr));
    if (f.metaCron.length() > 0) {
        const char* perr = NULL;
        cron_parse_expr(f.metaCron.c_str(), &f.metaExpr, &perr);
        if (perr) {
            f.metaValid = false;
            f.err = "cron error";
            unregisterFile(f);
            return false;
        }
    }

    // Подписки на события шины
    for (uint8_t i = 0; i < f.nRules; i++) {
        if (f.rules[i].type == MACRO_RULE_EVENT) { ensureEventSub(f.rules[i].spec); }
    }

    f.active = true;
    f.parseFails = 0;
    DEBUGMACROS("%s: %s -> %d rules (heap %u)\r\n", __FUNCTION__, f.name.c_str(), f.nRules, macroHeapFree());
    return true;
}

/** Валидирует сценарий без регистрации файла: синтаксис, meta_cron и rules. */
bool macroValidateScenario(EspLuaEngine& eng, const String& content, String& err) {
    err = "";
    lua_State* L = eng.getLuaState();
    if (L == NULL) { err = "No interpreter"; return false; }

    MacroFile tf;
    tf.name = "@validate";
    // На время исполнения top-level отключаем побочные bus-действия.
    macroValidateNoSideEffects = true;
    int status = macroLoadChunk(L, tf, content);
    macroValidateNoSideEffects = false;
    if (status != LUA_OK) {
        err = (tf.err.length() > 0) ? tf.err : String("Scenario parse error");
        lua_settop(L, 0);
        return false;
    }
    if (!lua_istable(L, -1)) {
        err = "legacy/invalid format: expected table {handlers,rules}";
        lua_settop(L, 0);
        return false;
    }
    int tbl = lua_absindex(L, -1);

    lua_getfield(L, tbl, "meta_cron");
    String metaCron;
    if (lua_isstring(L, -1)) { metaCron = String(lua_tostring(L, -1)); }
    lua_pop(L, 1);
    if (metaCron.length() > 0) {
        cron_expr metaExpr;
        memset(&metaExpr, 0, sizeof(metaExpr));
        const char* perr = NULL;
        cron_parse_expr(metaCron.c_str(), &metaExpr, &perr);
        if (perr) {
            err = String("meta_cron: ") + perr;
            lua_settop(L, 0);
            return false;
        }
    }

    lua_getfield(L, tbl, "rules");
    if (!lua_istable(L, -1)) {
        err = "no rules table";
        lua_settop(L, 0);
        return false;
    }
    int rt = lua_absindex(L, -1);
    int n = (int)lua_rawlen(L, rt);
    if (n <= 0) {
        err = "no rules";
        lua_settop(L, 0);
        return false;
    }
    if (n > MACRO_MAX_RULES) {
        err = String("too many rules (max ") + String((unsigned)MACRO_MAX_RULES) + ")";
        lua_settop(L, 0);
        return false;
    }

    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, rt, i);
        if (lua_istable(L, -1)) {
            MacroRule r;
            if (macroParseRule(L, tf, lua_absindex(L, -1), r) == false) {
                err = (tf.err.length() > 0) ? tf.err : String("rule ") + String(i) + ": invalid";
                lua_settop(L, 0);
                return false;
            }
        }
        lua_pop(L, 1);
    }
    lua_settop(L, 0);
    return true;
}

/** Читает только desc/meta_cron сценария (без разбора rules). */
bool macroReadScenarioMeta(EspLuaEngine& eng, const String& content,
                           String& desc, String& metaCron, String& err) {
    desc = "";
    metaCron = "";
    err = "";
    lua_State* L = eng.getLuaState();
    if (L == NULL) { err = "No interpreter"; return false; }

    MacroFile tf;
    tf.name = "@meta";
    // На время исполнения top-level отключаем побочные bus-действия.
    macroValidateNoSideEffects = true;
    int status = macroLoadChunk(L, tf, content);
    macroValidateNoSideEffects = false;
    if (status != LUA_OK) {
        err = (tf.err.length() > 0) ? tf.err : String("Scenario parse error");
        lua_settop(L, 0);
        return false;
    }
    if (!lua_istable(L, -1)) {
        err = "legacy/invalid format: expected table {handlers,rules}";
        lua_settop(L, 0);
        return false;
    }
    int tbl = lua_absindex(L, -1);

    lua_getfield(L, tbl, "desc");
    if (lua_isstring(L, -1)) { desc = String(lua_tostring(L, -1)); }
    lua_pop(L, 1);

    lua_getfield(L, tbl, "meta_cron");
    if (lua_isstring(L, -1)) { metaCron = String(lua_tostring(L, -1)); }
    lua_pop(L, 1);

    lua_settop(L, 0);
    return true;
}

/** Заполняет desc/meta_cron остановленного файла (без разбора правил). */
void CLASS_MODULE_MACROS::refreshFileMeta(MacroFile& f) {
    if (f.active) { return; }                 // активный уже заполнен registerFile
    if (_fs == NULL || _fs->exists(f.name) == false) { return; }
    if (initLua() == false) { return; }
    if (macroHeapFree() < MACRO_LUA_MIN_HEAP) { return; }

    String content = readFile(f.name);
    if (content.length() == 0) { return; }

    String desc, cron, err;
    if (macroReadScenarioMeta(*_lua, content, desc, cron, err)) {
        f.desc = desc;
        f.metaCron = cron;
    }
}

/** Пере(регистрирует) только помеченные файлы (needParse), не более N за тик. */
void CLASS_MODULE_MACROS::rebuildScripts() {
    uint8_t done = 0;
    for (uint8_t i = 0; i < _fileCount; i++) {
        MacroFile& f = _files[i];
        if (f.run == false) {
            if (f.active) { unregisterFile(f); }
            // Остановленный файл: при пометке обновляем только desc/meta_cron
            // (для веб-таблицы), правила не разбираем.
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
// Выполнение правил
// ============================================================

static void macroPushEventTable(lua_State* L, MacroRule& r, const BusValue* value) {
    lua_newtable(L);
    lua_pushstring(L, macroRuleTypeName(r.type));
    lua_setfield(L, -2, "type");
    lua_pushstring(L, r.spec.c_str());
    lua_setfield(L, -2, "spec");
    if (value != NULL) {
        macroPushBusValue(L, *value);
        lua_setfield(L, -2, "value");
    }
    BusValue av[MACRO_CALL_MAX_ARGS];
    int n = macroParseArgs(r.handlerArgs, av, MACRO_CALL_MAX_ARGS);
    lua_newtable(L);
    for (int i = 0; i < n; i++) {
        macroPushBusValue(L, av[i]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "args");
}

// Обёртка для ПРОТЕКТИРОВАННОГО построения таблицы события: вызывается под
// lua_pcall (аллокации tabla вне защиты → abort()). Правило передаётся upvalue.
static int macroPushEventClosure(lua_State* L) {
    MacroRule* r = (MacroRule*)lua_touserdata(L, lua_upvalueindex(1));
    if (r == NULL) { lua_pushnil(L); return 1; }
    macroPushEventTable(L, *r, nullptr);
    return 1;
}

/** Исполняет named-handler: перечитывает файл, компилирует и вызывает функция. */
String CLASS_MODULE_MACROS::runHandler(MacroFile& f, MacroRule& r) {
    if (_lua == NULL) { return "No interpreter"; }
    if (macroHeapFree() < MACRO_LUA_MIN_HEAP) { return "low memory"; }

    String content = readFile(f.name);
    if (content.length() == 0) { return "File is empty or unreadable"; }

    lua_State* L = _lua->getLuaState();
    int status = macroLoadChunk(L, f, content);
    if (status != LUA_OK) {
        return (f.err.length() > 0) ? f.err : String("Scenario parse error");
    }
    if (!lua_istable(L, -1)) { lua_settop(L, 0); return "handler: no table"; }
    int tbl = lua_absindex(L, -1);

    lua_getfield(L, tbl, "handlers");
    if (!lua_istable(L, -1)) { lua_settop(L, 0); return "handler: no handlers"; }

    lua_getfield(L, -1, r.handler.c_str());
    if (!lua_isfunction(L, -1)) { lua_settop(L, 0); return String("handler not found: ") + r.handler; }

    // Таблицу события строим в защищённом вызове (стек: [handler] -> [handler, event]).
    lua_pushlightuserdata(L, &r);
    lua_pushcclosure(L, macroPushEventClosure, 1);
    status = lua_pcall(L, 0, 1, 0);
    if (status != LUA_OK) {
        String err = macroLuaPopError(L);
        lua_settop(L, 0);
        return (err.length() > 0) ? err : String("event table error");
    }

    macroLuaSteps = 0;
    lua_sethook(L, macroLuaHook, LUA_MASKCOUNT, MACRO_LUA_HOOK_N);
    status = lua_pcall(L, 1, 0, 0);
    lua_sethook(L, NULL, 0, 0);

    String err;
    if (status != LUA_OK) {
        err = macroLuaPopError(L);
        if (err.length() == 0) { err = "handler error"; }
    }
    lua_settop(L, 0);
    return err;
}

/** Исполняет действие правила: set-запись либо декларативные bus-вызовы. */
static void macroRunActions(MacroFile& f, MacroRule& r) {
    if (r.setTarget.length() > 0) {
        BusResInfo info;
        int rc = BUS_ERR_NOT_FOUND;
        if (core_state.info(r.setTarget.c_str(), info) == BUS_OK) {
            // Обязательный резолв ENUM с актуальным info (поздняя регистрация ресурса):
            // строку приводим к индексу, числа отвергаем — до ветвления isFunc/state.
            BusValue val = r.setValue;
            if (info.kind == BusValue::ENUM) {
                String e;
                if (macroResolveEnum(r.setTarget.c_str(), val, val, e) != BUS_OK) {
                    f.err = e;
                    return;
                }
            }
            if (info.isFunc && !info.async) {
                // dual state+func: применяем значение через write-функцию
                BusValue res;
                rc = core_state.call(r.setTarget.c_str(), 1, &val, res);
                (void)res;   // результат write-функции не используется
            } else if (info.kind != BusValue::NONE) {
                // чистое состояние: коэрсим значение и обновляем реестр
                BusValue coerced = core_state.valueToKind(val, info.kind);
                switch (info.kind) {
                    case BusValue::BOOL: rc = core_state.setBool(r.setTarget.c_str(), coerced.b); break;
                    case BusValue::I32:  rc = core_state.setInt(r.setTarget.c_str(), coerced.i); break;
                    case BusValue::ENUM: rc = core_state.setInt(r.setTarget.c_str(), coerced.i); break;
                    case BusValue::F32:  rc = core_state.setF32(r.setTarget.c_str(), coerced.f); break;
                    case BusValue::STR:  rc = core_state.setStr(r.setTarget.c_str(), coerced.s); break;
                    case BusValue::TIME: rc = core_state.setTime(r.setTarget.c_str(), coerced.t); break;
                    default: break;
                }
            }
        }
        if (rc != BUS_OK) {
            f.err = String("set ") + r.setTarget + " " + ns_core_state::busErrStr(rc);
        }
        return;
    }

    for (uint8_t a = 0; a < r.nActions; a++) {
        String act = r.actions[a];
        int sep = act.indexOf('|');
        String name = (sep < 0) ? act : act.substring(0, sep);
        String args = (sep < 0) ? String() : act.substring(sep + 1);

        BusValue av[MACRO_CALL_MAX_ARGS];
        int argc = macroParseArgs(args, av, MACRO_CALL_MAX_ARGS);

        if (core_state.has(name.c_str()) == false) {
            f.err = String("call: not found ") + name;
            continue;
        }
        BusResInfo info;
        core_state.info(name.c_str(), info);

        // Обязательный резолв ENUM-аргумента до ветвления isFunc/state.
        if (info.kind == BusValue::ENUM && argc > 0) {
            BusValue rv;
            String e;
            if (macroResolveEnum(name.c_str(), av[0], rv, e) != BUS_OK) {
                f.err = e;
                continue;
            }
            av[0] = rv;
        }

        int rc = BUS_ERR_BAD_TYPE;
        if (info.isFunc && !info.async) {
            BusValue res;
            rc = core_state.call(name.c_str(), argc, (argc > 0) ? av : nullptr, res);
        } else if (info.isFunc && info.async) {
            rc = core_state.call_async(name.c_str(), macroNoopCb, nullptr, argc, (argc > 0) ? av : nullptr);
        } else {
            BusValue v = (argc > 0) ? av[0] : BusValue();
            BusValue coerced = core_state.valueToKind(v, info.kind);
            switch (info.kind) {
                case BusValue::BOOL: rc = core_state.setBool(name.c_str(), coerced.b); break;
                case BusValue::I32:  rc = core_state.setInt(name.c_str(), coerced.i); break;
                case BusValue::ENUM: rc = core_state.setInt(name.c_str(), coerced.i); break;
                case BusValue::F32:  rc = core_state.setF32(name.c_str(), coerced.f); break;
                case BusValue::STR:  rc = core_state.setStr(name.c_str(), coerced.s); break;
                case BusValue::TIME: rc = core_state.setTime(name.c_str(), coerced.t); break;
                default: break;
            }
        }
        if (rc != BUS_OK) {
            f.err = String("call ") + name + " " + ns_core_state::busErrStr(rc);
        }
    }
}

void CLASS_MODULE_MACROS::fireRule(MacroFile& f, MacroRule& r) {
    // Единый лог срабатывания правила (все типы: cron/cond/term/button/on/body).
    DEBUGMACROS("[MACRO] %s fired %s(%s)\r\n", f.name.c_str(), macroRuleTypeName(r.type),
                (r.type == MACRO_RULE_COND) ? macroRuleCondStr(r).c_str() : r.spec.c_str());
    if (r.handler.length() > 0) {
        String err = runHandler(f, r);
        if (err.length() > 0) {
            f.err = err;
            DEBUGMACROS("[MACRO] %s exec error: %s\r\n", f.name.c_str(), err.c_str());
        }
        return;
    }
    macroRunActions(f, r);
}

/** Вычисляет составное условие правила (AND по структурному массиву conds[]). */
bool CLASS_MODULE_MACROS::evalConds(MacroFile& f, MacroRule& r, String& errTxt) {
    (void)f;
    errTxt = "";
    bool all = true;

    for (uint8_t i = 0; i < r.nConds; i++) {
        MacroCond& c = r.conds[i];
        BusValue cur;
        if (macroReadRes(c.res, cur) == false) {
            // При ошибке чтения lastVals/haveLast НЕ обновляем:
            // changed должен сработать, когда ресурс снова станет доступен.
            errTxt = String("cond: not found ") + c.res;
            return false;
        }

        if (c.op == MACRO_COND_CHANGED) {
            String curStr = (cur.kind == BusValue::STR) ? String("s:") + cur.s
                          : (cur.kind == BusValue::BOOL) ? String(cur.b ? "b:1" : "b:0")
                          : (cur.kind == BusValue::F32) ? String("f:") + String(cur.f)
                          : (cur.kind == BusValue::TIME) ? String("t:") + String((long)cur.t)
                          : String("i:") + String((long)cur.i);
            bool changed = r.haveLast[i] && (curStr != r.lastVals[i]);
            r.lastVals[i] = curStr;
            r.haveLast[i] = true;
            if (!changed) { all = false; }
        } else {
            // Резолв ENUM-значения cond (обязательный: ресурс мог зарегистрироваться
            // после парсинга). Числовые значения для ENUM отвергаются.
            BusValue want = c.val;
            if (cur.kind == BusValue::ENUM) {
                String e;
                if (macroResolveEnum(c.res.c_str(), c.val, want, e) != BUS_OK) {
                    errTxt = e;
                    return false;
                }
            }
            if (macroCompare(cur, c.op, want) == false) { all = false; }
        }
    }
    return all;
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
    module_macros.fireToken(MACRO_RULE_EVENT, argv[0].s);
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
            if (f.active == false || f.rules == NULL) { continue; }
            for (uint8_t ri = 0; ri < f.nRules; ri++) {
                if (f.rules[ri].type == MACRO_RULE_EVENT && f.rules[ri].spec == _evtSubs[i].name) {
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
// Мета-cron и исполнительная машина
// ============================================================

static bool macroMetaGate(MacroFile& f, bool ntpSync) {
    if (f.metaCron.length() == 0) { return true; }
    if (f.metaValid == false) { return false; }
    if (ntpSync == false) { return false; }

    time_t t = (time_t)now();
    if (f.metaInit == false || t >= f.metaNext) {
        time_t from = (t > 0) ? (t - 1) : 0;
        f.metaNext = cron_next(&f.metaExpr, from);
        f.metaInit = true;
    }
    return (f.metaNext != (time_t)-1 && f.metaNext <= t);
}

void CLASS_MODULE_MACROS::drainEvents() {
    bool ntpSync = (NTP.getLastNTPSync() > 0);
    while (_evIn != _evOut) {
        MacroEvent ev = _evQueue[_evOut];
        _evOut = (_evOut + 1) % MACRO_EV_QUEUE;

        for (uint8_t i = 0; i < _fileCount; i++) {
            MacroFile& f = _files[i];
            if (f.run == false || f.active == false || f.rules == NULL) { continue; }
            if (macroMetaGate(f, ntpSync) == false) { continue; }
            for (uint8_t j = 0; j < f.nRules; j++) {
                MacroRule& r = f.rules[j];
                if (r.type != ev.type || r.spec != ev.spec) { continue; }

                // Для правил с первичным триггером cond с op=changed сравнивается
                // с моментом предыдущего срабатывания этого правила.
                String errTxt;
                bool ok = (r.nConds == 0) || evalConds(f, r, errTxt);
                if (errTxt.length() > 0) { f.err = errTxt; continue; }
                if (ok) {
                    fireRule(f, r);
                }
            }
        }
    }
}

void CLASS_MODULE_MACROS::tickStep() {
    rebuildScripts();

    if (_config.enabled == false) { return; }

    drainEvents();

    bool ntpSync = (NTP.getLastNTPSync() > 0);
    if (ntpSync && !_ntpWasSynced) {
        _ntpWasSynced = true;
        DEBUGMACROS("[MACRO] NTP synced, cron activated\r\n");
        // Файлы, добавленные до NTP, получают реальную дату создания (только в памяти).
        for (uint8_t i = 0; i < _fileCount; i++) {
            if (_files[i].created < MACRO_CREATED_MIN) {
                _files[i].created = (uint32_t)now();
            }
        }
    }

    // Скачок времени (NTP-синк, смена TZ, ручной time.set, откат источника): cron-правила
    // пропускают «прошлые» срабатывания — next пересчитывается без выполнения.
    time_t tNow = (time_t)now();
    int64_t tickDiff = (int64_t)tNow - (int64_t)_lastTickNow;
    bool bigJump = (_lastTickNow == 0)
        || (tickDiff > MACRO_CRON_JUMP_MAX_S) || (tickDiff < -MACRO_CRON_JUMP_MAX_S);
    if (bigJump) {
        // База пересчёта next — максимум текущего и предыдущего такта: после ОТКАТА
        // времени next уходит за уже пройденную (в прежней шкале) точку, поэтому при
        // «догоне» правило не срабатывает повторно.
        time_t base = (tNow > _lastTickNow) ? tNow : _lastTickNow;
        DEBUGMACROS("[MACRO] time jump %+lds, re-arm cron (no fire)\r\n",
                    (long)(tNow - _lastTickNow));
        for (uint8_t i = 0; i < _fileCount; i++) {
            MacroFile& f = _files[i];
            if (f.run == false || f.active == false || f.rules == NULL) { continue; }
            for (uint8_t j = 0; j < f.nRules; j++) {
                MacroRule& r = f.rules[j];
                if (r.type == MACRO_RULE_CRON) {
                    if (r.next == 0 || r.next == (time_t)-1) { continue; }
                    r.next = cron_next(&r.expr, base);
                } else if (r.type == MACRO_RULE_COND || r.type == MACRO_RULE_BODY) {
                    r.lastCond = false;   // сброс фронта после скачка
                }
            }
        }
    }
    _lastTickNow = tNow;

    for (uint8_t i = 0; i < _fileCount; i++) {
        MacroFile& f = _files[i];
        if (f.run == false || f.active == false || f.rules == NULL) { continue; }

        bool gate = macroMetaGate(f, ntpSync);

        for (uint8_t j = 0; j < f.nRules; j++) {
            MacroRule& r = f.rules[j];

            // term/button/on исполняются в drainEvents()
            if (r.type == MACRO_RULE_TERM || r.type == MACRO_RULE_BUTTON || r.type == MACRO_RULE_EVENT) {
                continue;
            }

            if (r.type == MACRO_RULE_BODY) {
                if (f.metaCron.length() > 0) {
                    if (gate && r.lastCond == false) {
                        r.lastCond = true;
                        fireRule(f, r);
                    } else if (gate == false) {
                        r.lastCond = false;
                    }
                } else if (r.lastCond == false) {
                    r.lastCond = true;
                    fireRule(f, r);
                }
                continue;
            }

            if (gate == false) { continue; }

            if (r.type == MACRO_RULE_CRON) {
                if (ntpSync == false) { continue; }
                if (r.next == 0) {
                    // Первичная инициализация: cond не вычисляем, иначе он «застрянет»
                    // на первом тике и потеряет момент времени.
                    r.next = cron_next(&r.expr, (time_t)now());
                    continue;
                }
                if (r.next == (time_t)-1) { continue; }
                if ((time_t)now() >= r.next) {
                    String errTxt;
                    bool ok = (r.nConds == 0) || evalConds(f, r, errTxt);
                    if (errTxt.length() > 0) { f.err = errTxt; }
                    if (ok) {
                        fireRule(f, r);
                    }
                    // Продвигаем next всегда — защита от цикла при устойчивой ошибке cond.
                    r.next = cron_next(&r.expr, (time_t)now());
                }
            }

            if (r.type == MACRO_RULE_COND) {
                String errTxt;
                bool truth = evalConds(f, r, errTxt);
                if (errTxt.length() > 0) {
                    f.err = errTxt;
                    DEBUGMACROS("[MACRO] %s cond error: %s\r\n", f.name.c_str(), errTxt.c_str());
                    continue;   // fire не вызывается, lastCond не меняем
                }
                if (truth && r.lastCond == false) {
                    r.lastCond = true;
                    fireRule(f, r);
                } else if (truth == false) {
                    r.lastCond = false;
                }
            }
        }
    }
}
