#include "core_web/FSWebServerLib.h"

#include "core_ntp/NtpClientLib.h"
#include "core_json/core_json.h"
#include "core_terminal/core_terminal.h"
#include "core_terminal/ErriezSerialTerminal.h"

#include "module_macros.h"
#include "common_module.h"
#include "common/common.h"
#include "common/TimeLib.h"
#include "core_state/core_state.h"
#include "core_state/common_module.h"
#include "module_macros_version.h"
#include "core_sys/eertos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>

CLASS_MODULE_MACROS module_macros;

CLASS_MODULE_MACROS::CLASS_MODULE_MACROS() {
    _fileCount = 0;
    _ntpWasSynced = false;
    _evIn = 0;
    _evOut = 0;
    _fs = NULL;
    _files = NULL;
    _lua = NULL;
    _luaHeapBytes = 0;
    _nEvtSubs = 0;
    _resourcesReady = false;
}

// Периодическая 1-сек задача и терминальный обработчик объявлены здесь,
// чтобы их можно было использовать до определений в конце файла
void macroTickTask();
void macroCmd();

// Полный сброс одного слота файла: массив _files лежит в heap и не
// обнуляется автоматически, поэтому каждое использование слота начинается
// с этой инициализации.
static void resetMacroFile(MacroFile& f) {
    f.name = "";
    f.prio = 7;
    f.run = false;
    f.created = 0;
    f.metaCron = "";
    f.size = 0;
    f.active = false;
    f.err = "";
    f.desc = "";
    f.needParse = false;
    f.parseFails = 0;
    f.rules = NULL;
    f.nRules = 0;
    memset(&f.metaExpr, 0, sizeof(cron_expr));
    f.metaValid = false;
    f.metaInit = false;
    f.metaNext = 0;
}

// Обновляет кэш размера файла (чтобы /macros/list не открывал файлы на каждый запрос).
static void updateMacroFileSize(fs::LittleFSFS* fs, MacroFile& f) {
    f.size = 0;
    if (fs == NULL) { return; }
    File sf = fs->open(f.name, "r");
    if (sf) {
        f.size = (uint32_t)sf.size();
        sf.close();
    }
}

// ============================================================
// setFs()
// ============================================================
void CLASS_MODULE_MACROS::setFs(fs::LittleFSFS* fs) {
    _fs = fs;
}

// ============================================================
// begin()
// ============================================================
void CLASS_MODULE_MACROS::begin() {
    DEBUGMACROS("%s\r\n", __FUNCTION__);

    // Массив файлов выделяется в heap (static .bss для 25 слотов не влезает в DRAM).
    if (_files == NULL) {
        _files = new (std::nothrow) MacroFile[MACRO_MAX_FILES];
        if (_files == NULL) {
            DEBUGMACROS("%s: no memory for %d file slots\r\n", __FUNCTION__, MACRO_MAX_FILES);
            return;
        }
    }

    defaultConfig();
    if (loadConfig() == false) { saveConfig(); }

    ensureMacrosDir();
    reconcileList();

    // Кэшируем размеры файлов один раз при старте (для /macros/list).
    for (uint8_t i = 0; i < _fileCount; i++) { updateMacroFileSize(_fs, _files[i]); }

    TerminalRegisterModule(macroTerminalRegister);

    // Первичная сборка интерпретаторов запущенных файлов выполняется в setup()
    // (main-loop контекст, до старта веб-сервера). Помечаем все run-файлы и
    // разбираем их пакетами (не более MACRO_PARSE_PER_TICK за вызов), чтобы
    // не создавать лавину Lua-состояний и не фрагментировать хип.
    markAllDirty();
    while (anyNeedParse()) { rebuildScripts(); }

    // Периодическая задача исполнительной машины (раз в секунду)
    SetTimerTask(macroTickTask, 1000);

    core_state.signal("macros.enabled", BusValue::bo(_config.enabled));
    core_state.signal("macros.files", BusValue::i32(_fileCount));
}

void CLASS_MODULE_MACROS::begin(ModContext& ctx) {
    _fs = ctx.fs;
    begin();
}

// ============================================================
// register_resources()
// ============================================================
static int macroBusReload(void* user, int argc, const BusValue* argv, BusValue& result) {
    (void)user; (void)argc; (void)argv; (void)result;
    module_macros.reloadAll();
    return BUS_OK;
}

void CLASS_MODULE_MACROS::register_resources() {
    DEBUGMACROS("%s\r\n", __FUNCTION__);

    core_state.regState("enabled", BusValue::BOOL, "macros module enabled", false);
    core_state.regState("files",   BusValue::I32,  "number of scenarios", false);
    core_state.regFunc("reload", "->", "reload all scenarios", macroBusReload, nullptr);
}

// ============================================================
// web_Init()
// ============================================================
void CLASS_MODULE_MACROS::web_Init() {
    DEBUGMACROS("%s\r\n", __FUNCTION__);

    // AJAX — список файлов-сценариев (JSON)
    ESPHTTPServer.on("/macros/list", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!ESPHTTPServer.checkAuth(request)) { return request->requestAuthentication(); }
        this->handleList(request);
    });

    // AJAX — действия по сценариям (только GET, состояние передаётся в query)
    ESPHTTPServer.on("/macros/create", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!ESPHTTPServer.checkAuth(request)) { return request->requestAuthentication(); }
        this->handleCreate(request);
    });

    // AJAX — удалить файл(ы)
    ESPHTTPServer.on("/macros/delete", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!ESPHTTPServer.checkAuth(request)) { return request->requestAuthentication(); }
        this->handleDelete(request);
    });

    // AJAX — переименовать файл
    ESPHTTPServer.on("/macros/rename", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!ESPHTTPServer.checkAuth(request)) { return request->requestAuthentication(); }
        this->handleRename(request);
    });

    // AJAX — запустить/остановить файл
    ESPHTTPServer.on("/macros/state", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!ESPHTTPServer.checkAuth(request)) { return request->requestAuthentication(); }
        this->handleState(request);
    });

    // AJAX — изменить приоритет (информационно)
    ESPHTTPServer.on("/macros/prio", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!ESPHTTPServer.checkAuth(request)) { return request->requestAuthentication(); }
        this->handlePrio(request);
    });

    // AJAX — перечитать файл(ы)
    ESPHTTPServer.on("/macros/reload", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!ESPHTTPServer.checkAuth(request)) { return request->requestAuthentication(); }
        this->handleReload(request);
    });

    // AJAX — внешнее событие (button/term) — резерв для будущих модулей
    ESPHTTPServer.on("/macros/fire", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!ESPHTTPServer.checkAuth(request)) { return request->requestAuthentication(); }
        this->handleFire(request);
    });

    // AJAX — каталог ресурсов шины (для дерева на странице)
    ESPHTTPServer.on("/macros/resources", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!ESPHTTPServer.checkAuth(request)) { return request->requestAuthentication(); }
        this->handleResources(request);
    });

    // AJAX — валидация cron/синтаксиса Lua
    ESPHTTPServer.on("/macros/validate", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!ESPHTTPServer.checkAuth(request)) { return request->requestAuthentication(); }
        this->handleValidate(request);
    });

    // AJAX — оценка heap модуля (валидация бюджета макросов)
    ESPHTTPServer.on("/macros/heap", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!ESPHTTPServer.checkAuth(request)) { return request->requestAuthentication(); }
        this->handleHeap(request);
    });

    // AJAX — сохранение содержимого сценария (встроенный редактор)
    ESPHTTPServer.on("/macros/save", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!ESPHTTPServer.checkAuth(request)) { return request->requestAuthentication(); }
        this->handleWrite(request);
    });

    // AJAX — чтение содержимого сценария
    ESPHTTPServer.on("/macros/get", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!ESPHTTPServer.checkAuth(request)) { return request->requestAuthentication(); }
        this->handleGet(request);
    });

    // Версия модуля
    ESPHTTPServer.on("/macros/ver", HTTP_GET, [this](AsyncWebServerRequest *request) {
        this->html_ver_get(request);
    });
}

// ============================================================
// Веб-обработчики
// ============================================================
void CLASS_MODULE_MACROS::handleList(AsyncWebServerRequest *request) {
    // Защита от OOM в AsyncTCP: без запаса heap не строим ответ.
    if (ESP.getMaxAllocHeap() < 6000) {
        request->send(503, "text/plain", "ERR: low memory");
        return;
    }

    AsyncResponseStream *resp = request->beginResponseStream("application/json");
    resp->print("{\"enabled\":");
    resp->print(_config.enabled ? "true" : "false");
    resp->print(",\"ntp\":");
    resp->print((NTP.getLastNTPSync() > 0) ? "1" : "0");
    resp->print(",\"files\":[");

    // Сортировка по приоритету (0 - высший), затем по имени
    uint8_t order[MACRO_MAX_FILES];
    for (uint8_t i = 0; i < _fileCount; i++) { order[i] = i; }
    for (uint8_t i = 0; i < _fileCount; i++) {
        for (uint8_t j = i + 1; j < _fileCount; j++) {
            bool less = (_files[order[j]].prio < _files[order[i]].prio);
            if (!less && _files[order[j]].prio == _files[order[i]].prio) {
                less = (_files[order[j]].name < _files[order[i]].name);
            }
            if (less) {
                uint8_t tmp = order[i];
                order[i] = order[j];
                order[j] = tmp;
            }
        }
    }

    for (uint8_t n = 0; n < _fileCount; n++) {
        MacroFile& f = _files[order[n]];
        if (n > 0) { resp->print(","); }

        resp->print("{\"name\":\"");
        resp->print(escapeJson(ns_module_macros::macroFileBaseName(f.name)));
        resp->print("\",\"prio\":");
        resp->print(f.prio);
        resp->print(",\"run\":");
        resp->print(f.run ? "true" : "false");
        resp->print(",\"size\":");
        resp->print(f.size);
        resp->print(",\"created\":");
        resp->print(f.created);
        resp->print(",\"active\":");
        resp->print(f.active ? "true" : "false");
        resp->print(",\"cron\":\"");
        resp->print(escapeJson(f.metaCron));
        resp->print("\",\"desc\":\"");
        resp->print(escapeJson(f.desc));
        resp->print("\",\"err\":\"");
        resp->print(escapeJson(f.err));
        resp->print("\"}");
    }

    resp->print("]}");
    request->send(resp);
}

void CLASS_MODULE_MACROS::handleCreate(AsyncWebServerRequest *request) {
    DEBUGMACROS("%s\r\n", __FUNCTION__);

    String tmpl = MACRO_DEFAULT_NAME;
    if (request->hasArg("name")) {
        String n = request->arg("name");
        n.trim();
        if (n.length() > 0) {
            if (!n.endsWith(".lua")) { n += ".lua"; }
            if (!nameOk(n)) { request->send(200, "text/plain", "ERR: bad name"); return; }
            tmpl = n;
        }
    }

    String fullPath;
    if (createNewFile(tmpl, fullPath)) {
        request->send(200, "text/plain", "OK");
    } else {
        request->send(200, "text/plain", "ERR: cannot create");
    }
}

void CLASS_MODULE_MACROS::handleDelete(AsyncWebServerRequest *request) {
    DEBUGMACROS("%s\r\n", __FUNCTION__);

    if (!request->hasArg("name")) { request->send(200, "text/plain", "ERR: no name"); return; }

    String list = request->arg("name");
    bool ok = true;
    int start = 0;
    while (start <= (int)list.length()) {
        int comma = list.indexOf(',', start);
        String base = (comma < 0) ? list.substring(start) : list.substring(start, comma);
        base.trim();
        if (base.length() > 0) {
            if (deleteFileEntry(base) == false) { ok = false; }
        }
        if (comma < 0) { break; }
        start = comma + 1;
    }
    request->send(200, "text/plain", ok ? "OK" : "ERR: partial delete");
}

void CLASS_MODULE_MACROS::handleRename(AsyncWebServerRequest *request) {
    DEBUGMACROS("%s\r\n", __FUNCTION__);

    if (!request->hasArg("old") || !request->hasArg("new")) {
        request->send(200, "text/plain", "ERR: no args");
        return;
    }
    String oldBase = request->arg("old");
    String newBase = request->arg("new");
    oldBase.trim();
    newBase.trim();
    if (!newBase.endsWith(".lua")) { newBase += ".lua"; }

    if (renameFileEntry(oldBase, newBase)) {
        request->send(200, "text/plain", "OK");
    } else {
        request->send(200, "text/plain", "ERR: rename failed");
    }
}

void CLASS_MODULE_MACROS::handleState(AsyncWebServerRequest *request) {
    DEBUGMACROS("%s\r\n", __FUNCTION__);

    if (!request->hasArg("name") || !request->hasArg("on")) {
        request->send(200, "text/plain", "ERR: no args");
        return;
    }
    String base = request->arg("name");
    base.trim();
    bool on = (request->arg("on") == "1");

    if (setFileRun(base, on)) {
        request->send(200, "text/plain", "OK");
    } else {
        request->send(200, "text/plain", "ERR: not found");
    }
}

void CLASS_MODULE_MACROS::handlePrio(AsyncWebServerRequest *request) {
    DEBUGMACROS("%s\r\n", __FUNCTION__);

    if (!request->hasArg("name") || !request->hasArg("dir")) {
        request->send(200, "text/plain", "ERR: no args");
        return;
    }
    String base = request->arg("name");
    base.trim();
    int8_t delta = (int8_t)request->arg("dir").toInt();

    if (setFilePrio(base, delta)) {
        request->send(200, "text/plain", "OK");
    } else {
        request->send(200, "text/plain", "ERR: not found");
    }
}

void CLASS_MODULE_MACROS::handleReload(AsyncWebServerRequest *request) {
    DEBUGMACROS("%s\r\n", __FUNCTION__);

    if (request->hasArg("name")) {
        String base = request->arg("name");
        base.trim();
        int idx = findFile(MACROS_DIR_RE + base);
        if (idx >= 0) {
            // Пересборка только этого файла в ближайшем тике
            _files[idx].parseFails = 0;
            _files[idx].needParse = true;
            request->send(200, "text/plain", "OK");
            return;
        }
        request->send(200, "text/plain", "ERR: not found");
        return;
    }
    reloadAll();
    request->send(200, "text/plain", "OK");
}

void CLASS_MODULE_MACROS::handleFire(AsyncWebServerRequest *request) {
    DEBUGMACROS("%s\r\n", __FUNCTION__);

    if (!request->hasArg("token")) {
        request->send(200, "text/plain", "ERR: no token");
        return;
    }
    uint8_t type = MACRO_RULE_TERM;
    if (request->hasArg("type")) {
        String t = request->arg("type");
        if (t == "button") { type = MACRO_RULE_BUTTON; }
        else if (t == "term") { type = MACRO_RULE_TERM; }
    }
    String token = request->arg("token");
    if (fireToken(type, token)) {
        request->send(200, "text/plain", "OK");
    } else {
        request->send(200, "text/plain", "ERR: queue full");
    }
}

void CLASS_MODULE_MACROS::handleWrite(AsyncWebServerRequest *request) {
    DEBUGMACROS("%s\r\n", __FUNCTION__);

    if (!request->hasArg("name") || !request->hasArg("body")) {
        request->send(200, "text/plain", "ERR: no args");
        return;
    }
    String base = request->arg("name");
    base.trim();
    if (nameOk(base) == false) {
        request->send(200, "text/plain", "ERR: bad name");
        return;
    }

    // Правка и сохранение доступны только для остановленных сценариев.
    int idx = findFile(MACROS_DIR_RE + base);
    if (idx >= 0 && _files[idx].run) {
        request->send(200, "text/plain", "ERR: stop scenario first");
        return;
    }

    String body = request->arg("body");
    if (writeFile(MACROS_DIR_RE + base, body) == false) {
        request->send(200, "text/plain", "ERR: write failed");
        return;
    }
    if (idx >= 0) {
        _files[idx].parseFails = 0;
        _files[idx].needParse = true;
        updateMacroFileSize(_fs, _files[idx]);
    }
    request->send(200, "text/plain", "OK");
}

void CLASS_MODULE_MACROS::handleGet(AsyncWebServerRequest *request) {
    if (!request->hasArg("name")) {
        request->send(200, "text/plain", "");
        return;
    }
    String base = request->arg("name");
    base.trim();
    if (nameOk(base) == false) {
        request->send(200, "text/plain", "");
        return;
    }
    request->send(200, "text/plain", readFile(MACROS_DIR_RE + base));
}

void CLASS_MODULE_MACROS::handleResources(AsyncWebServerRequest *request) {
    // Каталог ресурсов статичен после старта: собираем один раз (пока heap высок)
    // и стримим из памяти, без JsonDocument и аллокаций на каждый запрос.
    if (ESP.getMaxAllocHeap() < 8000) {
        request->send(503, "text/plain", "ERR: low memory");
        return;
    }
    if (_resourcesReady == false) {
        JsonDocument doc;
        core_state.catalogToJson(doc);
        if (doc.overflowed()) {
            request->send(500, "text/plain", "ERR: catalog too large");
            return;
        }
        _resourcesJson = "";
        serializeJson(doc, _resourcesJson);
        _resourcesReady = true;
    }
    AsyncResponseStream *resp = request->beginResponseStream("application/json");
    resp->print(_resourcesJson);
    request->send(resp);
}

void CLASS_MODULE_MACROS::handleValidate(AsyncWebServerRequest *request) {
    DEBUGMACROS("%s\r\n", __FUNCTION__);

    if (request->hasArg("cron")) {
        String errTxt;
        if (ns_module_macros::macroCronValid(request->arg("cron"), errTxt) == false) {
            request->send(200, "text/plain", String("ERROR: cron: ") + errTxt);
            return;
        }
    }

    if (request->hasArg("body")) {
        String body = request->arg("body");
        EspLuaEngine eng;
        if (eng.getLuaState() == NULL) {
            request->send(200, "text/plain", "ERROR: no memory");
            return;
        }
        String cn = "@validate";
        int status = luaL_loadbuffer(eng.getLuaState(), body.c_str(), body.length(), cn.c_str());
        if (status != LUA_OK) {
            size_t len = 0;
            const char* s = lua_tolstring(eng.getLuaState(), -1, &len);
            String e = (s != NULL) ? String(s) : String("syntax error");
            request->send(200, "text/plain", String("ERROR: lua: ") + e);
            return;
        }
    }

    request->send(200, "text/plain", "OK");
}

// Оценка heap, занятого модулем: массив файлов + правила (со строками) +
// единый lua_State + кэш каталога ресурсов. Служит для проверки бюджета.
size_t CLASS_MODULE_MACROS::estimateHeapBytes() {
    size_t total = 0;

    if (_files != NULL) {
        total += sizeof(MacroFile) * MACRO_MAX_FILES; // выделено под весь лимит файлов
        for (uint8_t i = 0; i < _fileCount; i++) {
            MacroFile& f = _files[i];
            total += f.name.length() + 1;
            total += f.metaCron.length() + 1;
            total += f.desc.length() + 1;
            total += f.err.length() + 1;
            if (f.rules == NULL) { continue; }
            total += sizeof(MacroRule) * f.nRules;
            for (uint8_t j = 0; j < f.nRules; j++) {
                MacroRule& r = f.rules[j];
                total += r.spec.length() + 1;
                total += r.setTarget.length() + 1;
                if (r.setValue.kind == BusValue::STR) { total += r.setValue.s.length() + 1; }
                for (uint8_t k = 0; k < r.nConds; k++) {
                    total += r.conds[k].res.length() + 1;
                    if (r.conds[k].val.kind == BusValue::STR) { total += r.conds[k].val.s.length() + 1; }
                    total += r.lastVals[k].length() + 1;
                }
                total += r.handler.length() + 1;
                total += r.handlerArgs.length() + 1;
                for (uint8_t a = 0; a < r.nActions; a++) {
                    total += r.actions[a].length() + 1;
                }
            }
        }
    }

    total += _luaHeapBytes;                  // единый интерпретатор
    total += _resourcesJson.length() + 1;    // кэш каталога ресурсов
    for (uint8_t i = 0; i < _nEvtSubs; i++) { total += _evtSubs[i].name.length() + 1; }
    return total;
}

void CLASS_MODULE_MACROS::handleHeap(AsyncWebServerRequest *request) {
    size_t used = estimateHeapBytes();
    uint32_t pct = (uint32_t)((used * 100) / MACRO_HEAP_BUDGET);
    bool critical = (pct >= MACRO_HEAP_CRIT_PCT);
    uint8_t active = 0;
    uint16_t rules = 0;
    for (uint8_t i = 0; i < _fileCount; i++) {
        if (_files[i].active) { active++; rules += _files[i].nRules; }
    }

    AsyncResponseStream *resp = request->beginResponseStream("application/json");
    resp->print("{\"budget\":");   resp->print((uint32_t)MACRO_HEAP_BUDGET);
    resp->print(",\"used\":");     resp->print((uint32_t)used);
    resp->print(",\"free\":");     resp->print((uint32_t)ESP.getFreeHeap());
    resp->print(",\"maxalloc\":"); resp->print((uint32_t)ESP.getMaxAllocHeap());
    resp->print(",\"percent\":");  resp->print(pct);
    resp->print(",\"limit\":");    resp->print((uint32_t)MACRO_HEAP_CRIT_PCT);
    resp->print(",\"critical\":"); resp->print(critical ? "true" : "false");
    resp->print(",\"files\":");    resp->print(_fileCount);
    resp->print(",\"active\":");   resp->print(active);
    resp->print(",\"rules\":");    resp->print(rules);
    resp->print(",\"lua\":");      resp->print((uint32_t)_luaHeapBytes);
    resp->print(",\"catalog\":");  resp->print((uint32_t)_resourcesJson.length());
    resp->print("}");
    request->send(resp);
}

// ============================================================
// Конфиг
// ============================================================

void CLASS_MODULE_MACROS::defaultConfig() {
    _config.enabled = true;
    _fileCount = 0;
}

bool CLASS_MODULE_MACROS::loadConfig() {
    DEBUGMACROS("%s\r\n", __FUNCTION__);
    JsonDocument doc;
    if (core_json.jsonFileLoadDoc(CONFIG_FILE_MACROS, doc) == false) { return false; }

    _config.enabled = doc["enabled"].as<bool>();

    _fileCount = 0;
    if (doc["files"].is<JsonArray>()) {
        JsonArray arr = doc["files"].as<JsonArray>();
        for (JsonObject obj : arr) {
            if (_fileCount >= MACRO_MAX_FILES) { break; }
            MacroFile& f = _files[_fileCount];
            resetMacroFile(f);
            f.name     = obj["name"].as<String>();
            if (f.name.length() == 0) { continue; }
            f.prio     = obj["prio"].as<uint8_t>();
            f.run      = obj["run"].as<bool>();
            f.created  = obj["created"].as<uint32_t>();
            _fileCount++;
        }
    }

    DEBUGMACROS("enabled: %d, files: %d\r\n", _config.enabled, _fileCount);
    return true;
}

bool CLASS_MODULE_MACROS::saveConfig() {
    DEBUGMACROS("%s\r\n", __FUNCTION__);
    JsonDocument doc;
    core_json.jsonFileLoadDoc(CONFIG_FILE_MACROS, doc);
    doc["enabled"] = _config.enabled;

    JsonArray arr = doc["files"].to<JsonArray>();
    arr.clear();
    for (uint8_t i = 0; i < _fileCount; i++) {
        JsonObject obj = arr.add<JsonObject>();
        obj["name"]    = _files[i].name;
        obj["prio"]    = _files[i].prio;
        obj["run"]     = _files[i].run;
        obj["created"] = _files[i].created;
    }
    return core_json.jsonFileSaveDoc(CONFIG_FILE_MACROS, doc);
}

bool CLASS_MODULE_MACROS::saveMeta() {
    return saveConfig();
}

// ============================================================
// Версионные методы
// ============================================================

String CLASS_MODULE_MACROS::getVersionStr() { return String(MODULE_MACROS_VERSION); }
String CLASS_MODULE_MACROS::getGeneratedTime() { return String(MODULE_MACROS_GENERATED_TIME); }
String CLASS_MODULE_MACROS::getCommitDateStr() { return String(MODULE_MACROS_COMMIT_DATE_STR); }

void CLASS_MODULE_MACROS::html_ver_get(AsyncWebServerRequest *request) {
    DEBUGMACROS("%s\r\n", __FUNCTION__);
    String values = "";
    values += "macrosversion|" + getVersionStr()    + "|div\n";
    values += "macrosgentime|" + getGeneratedTime() + "|div\n";
    values += "macrosgendate|" + getCommitDateStr() + "|div\n";
    request->send(200, "text/plain", values);
}

// ============================================================
// Логика работы со списком файлов
// ============================================================

int CLASS_MODULE_MACROS::findFile(const String& name) {
    for (uint8_t i = 0; i < _fileCount; i++) {
        if (_files[i].name == name) { return i; }
    }
    return -1;
}

bool CLASS_MODULE_MACROS::nameOk(const String& name) {
    if (name.length() < 6 || name.length() > 48) { return false; }
    if (!name.endsWith(".lua")) { return false; }
    for (uint8_t i = 0; i < name.length(); i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok) { return false; }
    }
    return true;
}

void CLASS_MODULE_MACROS::markAllDirty() {
    for (uint8_t i = 0; i < _fileCount; i++) {
        if (_files[i].run) { _files[i].needParse = true; _files[i].parseFails = 0; }
    }
}

void CLASS_MODULE_MACROS::bumpMeta() {
    // Полная перерегистрация: помечаем все запущенные файлы (create/delete/rename/reloadAll)
    markAllDirty();
}

String CLASS_MODULE_MACROS::readFile(const String& path) {
    File f = _fs->open(path, "r");
    if (!f) { return ""; }
    String content;
    while (f.available()) {
        content += (char)f.read();
        if (content.length() > 8192) { break; }
    }
    f.close();
    return content;
}

bool CLASS_MODULE_MACROS::writeFile(const String& path, const String& data) {
    File f = _fs->open(path, "w");
    if (!f) { return false; }
    bool ok = (f.write((const uint8_t*)data.c_str(), data.length()) == data.length());
    f.close();
    return ok;
}

bool CLASS_MODULE_MACROS::fsRenameFile(const String& oldPath, const String& newPath) {
    String data = readFile(oldPath);
    if (data.length() == 0) { return false; }
    if (writeFile(newPath, data) == false) { return false; }
    _fs->remove(oldPath);
    return true;
}

String CLASS_MODULE_MACROS::uniqueNewName(const String& tmpl) {
    String base = tmpl.substring(0, tmpl.length() - 4); // без ".lua"
    String cand = tmpl;
    uint8_t idx = 0;
    while (_fs->exists(MACROS_DIR_RE + cand)) {
        idx++;
        cand = base + String(idx) + ".lua";
        if (idx > 99) { break; }
    }
    return cand;
}

void CLASS_MODULE_MACROS::ensureMacrosDir() {
    // Создаём каталог сценариев (примеры .lua поставляются в составе FS-образа:
    // web/macros/*.lua -> /macros/*.lua, см. python/4_fs_builder.py)
    if (_fs->exists(MACROS_DIR) == false) {
        _fs->mkdir(MACROS_DIR);
        DEBUGMACROS("%s: dir %s created\r\n", __FUNCTION__, MACROS_DIR);
    }
}

void CLASS_MODULE_MACROS::reconcileList() {
    // 1) Удаляем записи, файлы которых пропали
    for (int i = (int)_fileCount - 1; i >= 0; i--) {
        if (_fs->exists(_files[i].name) == false) {
            unregisterFile(_files[i]);
            for (int j = i; j < (int)_fileCount - 1; j++) { _files[j] = _files[j + 1]; }
            // Слот за пределами _fileCount может хранить дубликат указателя rules —
            // не освобождаем его (владелец — перемещённая запись), обнуляем при переиспользовании.
            _fileCount--;
        }
    }

    // 2) Сканируем каталог и добавляем новые .lua
    File root = _fs->open(MACROS_DIR);
    if (root) {
        File entry = root.openNextFile();
        while (entry) {
            if (entry.isDirectory() == false) {
                String base = ns_module_macros::macroFileBaseName(String(entry.name()));
                if (base.endsWith(".lua")) {
                    if (findFile(MACROS_DIR_RE + base) < 0) {
                        if (_fileCount >= MACRO_MAX_FILES) {
                            DEBUGMACROS("%s: limit %d reached, skipping %s\r\n",
                                        __FUNCTION__, MACRO_MAX_FILES, base.c_str());
                        } else {
                            addFileEntry(base, 7, false);
                        }
                    }
                }
            }
            entry = root.openNextFile();
        }
    }

    saveConfig();
    DEBUGMACROS("%s: total %d files\r\n", __FUNCTION__, _fileCount);
}

// ============================================================
// Операции со списком (общие для HTTP и терминала)
// ============================================================

bool CLASS_MODULE_MACROS::addFileEntry(const String& base, uint8_t prio, bool run) {
    if (nameOk(base) == false) { return false; }
    String path = MACROS_DIR_RE + base;
    if (findFile(path) >= 0) { return true; }
    if (_fileCount >= MACRO_MAX_FILES) { return false; }

    MacroFile& f = _files[_fileCount];
    // Слот массива лежит в heap и мог использоваться ранее — сбрасываем все
    // поля, чтобы новый файл не унаследовал мету/подписки предыдущего.
    resetMacroFile(f);
    f.name    = path;
    f.prio    = (prio > 7) ? 7 : prio;
    f.run     = run;
    f.created = (uint32_t)now();
    f.needParse = run;
    updateMacroFileSize(_fs, f);
    _fileCount++;
    return true;
}

bool CLASS_MODULE_MACROS::createNewFile(const String& base, String& fullPath) {
    if (nameOk(base) == false) { return false; }

    String cand = uniqueNewName(base);
    fullPath = MACROS_DIR_RE + cand;

    String empty;
    empty  = "-- Новый сценарий (Lua). Каждое правило: when + одно действие set/call/calls/run.\r\n";
    empty += "-- meta_cron = \"...\" задаёт окно (гейт) для всего файла; без него — активен всегда.\r\n";
    empty += "return {\r\n";
    empty += "    desc = \"new scenario\",\r\n";
    empty += "    -- meta_cron = \"*/10 * * * * *\",\r\n";
    empty += "    rules = {\r\n";
    empty += "        -- { when = { cron = \"0 * * * * *\" }, set = \"e7.effect\", value = 1 },\r\n";
    empty += "    },\r\n";
    empty += "}\r\n";
    if (writeFile(fullPath, empty) == false) { return false; }

    if (addFileEntry(cand, 7, false) == false) {
        _fs->remove(fullPath);
        return false;
    }
    saveConfig();
    bumpMeta();
    return true;
}

bool CLASS_MODULE_MACROS::deleteFileEntry(const String& base) {
    String path = MACROS_DIR_RE + base;
    int idx = findFile(path);
    if (idx < 0) { return false; }

    if (_fs->exists(path)) { _fs->remove(path); }

    unregisterFile(_files[idx]);
    for (int j = idx; j < (int)_fileCount - 1; j++) { _files[j] = _files[j + 1]; }
    _fileCount--;
    saveConfig();
    bumpMeta();
    return true;
}

bool CLASS_MODULE_MACROS::setFileRun(const String& base, bool on) {
    String path = MACROS_DIR_RE + base;
    int idx = findFile(path);
    if (idx < 0) { return false; }

    if (_files[idx].run != on) {
        _files[idx].run = on;
        if (on == false) {
            _files[idx].err = "";
            unregisterFile(_files[idx]);
        } else {
            // Регистрация правил только этого файла в ближайшем тике
            _files[idx].parseFails = 0;
            _files[idx].needParse = true;
        }
        saveConfig();
    }
    return true;
}

bool CLASS_MODULE_MACROS::setFilePrio(const String& base, int8_t delta) {
    String path = MACROS_DIR_RE + base;
    int idx = findFile(path);
    if (idx < 0) { return false; }

    int8_t p = (int8_t)_files[idx].prio + delta;
    if (p < 0) { p = 0; }
    if (p > 7) { p = 7; }
    _files[idx].prio = (uint8_t)p;
    saveConfig();
    return true;
}

bool CLASS_MODULE_MACROS::renameFileEntry(const String& oldBase, const String& newBase) {
    if (nameOk(newBase) == false) { return false; }
    String oldPath = MACROS_DIR_RE + oldBase;
    String newPath = MACROS_DIR_RE + newBase;
    int idx = findFile(oldPath);
    if (idx < 0) { return false; }
    if (newBase == oldBase) { return true; }
    if (_fs->exists(newPath)) { return false; }

    if (fsRenameFile(oldPath, newPath) == false) { return false; }

    _files[idx].name = newPath;
    _files[idx].err = "";
    saveConfig();
    bumpMeta();
    return true;
}

// ============================================================
// Публичное API
// ============================================================

bool CLASS_MODULE_MACROS::reloadAll() {
    bumpMeta();
    return true;
}

bool CLASS_MODULE_MACROS::fireToken(uint8_t type, const String& spec) {
    if (spec.length() == 0) { return false; }
    if (type != MACRO_RULE_TERM && type != MACRO_RULE_BUTTON && type != MACRO_RULE_EVENT) { return false; }
    uint8_t next = (uint8_t)((_evIn + 1) % MACRO_EV_QUEUE);
    if (next == _evOut) { return false; } // очередь заполнена
    _evQueue[_evIn].type = type;
    _evQueue[_evIn].spec = spec;
    _evIn = next;
    return true;
}

uint8_t CLASS_MODULE_MACROS::getFileCount() {
    return _fileCount;
}

void CLASS_MODULE_MACROS::printList() {
    Serial.printf("[MACRO] enabled: %d, files: %d\r\n", _config.enabled, _fileCount);
    for (uint8_t i = 0; i < _fileCount; i++) {
        MacroFile& f = _files[i];
        Serial.printf("  [%d] prio=%d run=%d active=%d %s (%d rules)%s%s\r\n",
                      i,
                      f.prio,
                      f.run ? 1 : 0,
                      f.active ? 1 : 0,
                      f.name.c_str(),
                      f.nRules,
                      f.err.length() > 0 ? " err=" : "",
                      f.err.c_str());
        if (f.rules == NULL) { continue; }
        for (uint8_t j = 0; j < f.nRules; j++) {
            MacroRule& r = f.rules[j];
            String cond = macroRuleCondStr(r);
            String action;
            if (r.setTarget.length() > 0) {
                action = String("set=") + r.setTarget;
            } else if (r.handler.length() > 0) {
                action = String("run=") + r.handler;
            } else {
                action = String("calls=") + String(r.nActions);
            }
            Serial.printf("      #%d %s spec='%s' cond='%s' %s\r\n",
                          j, macroRuleTypeStr(r.type), r.spec.c_str(),
                          cond.c_str(), action.c_str());
        }
    }
}

// ============================================================
// Периодическая задача EERTOS (раз в секунду)
// ============================================================

void macroTickTask() {
    module_macros.tickStep();
    SetTimerTask(macroTickTask, 1000);
}

// ============================================================
// Терминальные команды
// ============================================================

void macroCmd() {
    String arg = term.getNext();

    if (arg == "list") {
        module_macros.printList();
        return;
    }
    if (arg == "reload") {
        String name = term.getNext();
        if (name.length() == 0) {
            module_macros.reloadAll();
            Serial.println("[MACRO] reload requested");
            return;
        }
        // Перезапуск одного файла (пересборка в ближайшем тике)
        int idx = module_macros.findFile(String(MACROS_DIR_RE) + name);
        if (idx >= 0) {
            module_macros._files[idx].parseFails = 0;
            module_macros._files[idx].needParse = true;
            Serial.println("[MACRO] reload requested for " + name);
        } else {
            Serial.println("[MACRO] not found");
        }
        return;
    }
    if (arg == "run") {
        String name = term.getNext();
        if (name.length() == 0) { Serial.println("Usage: macro run <file.lua>"); return; }
        if (module_macros.setFileRun(name, true)) { Serial.println("[MACRO] run " + name); }
        else { Serial.println("[MACRO] not found"); }
        return;
    }
    if (arg == "stop") {
        String name = term.getNext();
        if (name.length() == 0) { Serial.println("Usage: macro stop <file.lua>"); return; }
        if (module_macros.setFileRun(name, false)) { Serial.println("[MACRO] stop " + name); }
        else { Serial.println("[MACRO] not found"); }
        return;
    }
    if (arg == "prio") {
        String name = term.getNext();
        String dir = term.getNext();
        if (name.length() == 0 || dir.length() == 0) {
            Serial.println("Usage: macro prio <file.lua> <+1|-1>");
            return;
        }
        int8_t d = (int8_t)dir.toInt();
        if (module_macros.setFilePrio(name, d)) { Serial.println("[MACRO] prio updated"); }
        else { Serial.println("[MACRO] not found"); }
        return;
    }
    if (arg == "msg" || arg == "btn") {
        uint8_t type = (arg == "msg") ? MACRO_RULE_TERM : MACRO_RULE_BUTTON;
        const char* what = (type == MACRO_RULE_TERM) ? "term" : "button";

        // Собираем ВСЕ слова аргументов в одну строку (спецификатор с параметрами)
        String spec;
        while (true) {
            String w = term.getNext();
            if (w.length() == 0) { break; }
            if (spec.length() > 0) { spec += " "; }
            spec += w;
        }
        if (spec.length() == 0) {
            Serial.println("Usage: macro msg <term-specifier> | macro btn <button-specifier>");
            return;
        }
        if (module_macros.fireToken(type, spec)) {
            Serial.println("[MACRO] event " + String(what) + " \"" + spec + "\" queued");
        } else {
            Serial.println("[MACRO] queue full");
        }
        return;
    }

    Serial.println("Commands:");
    Serial.println("  macro list                        - show scenarios");
    Serial.println("  macro reload [file.lua]           - re-read scenario(s)");
    Serial.println("  macro run <file.lua>              - start scenario");
    Serial.println("  macro stop <file.lua>             - stop scenario");
    Serial.println("  macro prio <file.lua> <delta>     - change priority");
    Serial.println("  macro msg <word> [params...]      - fire term-rules by full match");
    Serial.println("  macro btn <name> [params...]      - fire button-rules by full match");
}

void macroTerminalRegister() {
    term.addCommand("macro", macroCmd);
}
