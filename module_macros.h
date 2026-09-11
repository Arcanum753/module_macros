#ifndef _MODULE_MACROS_h
#define _MODULE_MACROS_h

#include "main.h"

#include "mod_context.h"

#include "module_macros_engine.h"

#ifdef DEBUG_MACROS
#define DEBUGMACROS(...) Serial.printf(__VA_ARGS__)
#else
#define DEBUGMACROS(...)
#endif

#include <LittleFS.h>

#define CONFIG_FILE_MACROS    "/config_macros.json"
#define MACROS_DIR            "/macros"
#define MACROS_DIR_RE         "/macros/"
#define MACRO_DEFAULT_NAME    "new_macros.lua"

class AsyncWebServerRequest;

class CLASS_MODULE_MACROS {
public:
    CLASS_MODULE_MACROS();
    void setFs(fs::LittleFSFS* fs);
    void begin();
    void begin(ModContext& ctx);
    void register_resources();
    void web_Init();

    // Публичное API
    bool reloadAll();                        // перечитать все файлы сценариев
    bool fireToken(uint8_t type, const String& spec); // событие term/button в очередь
    uint8_t getFileCount();
    void printList();                        // вывод списка сценариев в терминал

private:
    // Версионные методы
    String getVersionStr();
    String getGeneratedTime();
    String getCommitDateStr();
    void html_ver_get(AsyncWebServerRequest *request);

    // Веб-обработчики
    void handleList(AsyncWebServerRequest *request);
    void handleCreate(AsyncWebServerRequest *request);
    void handleDelete(AsyncWebServerRequest *request);
    void handleRename(AsyncWebServerRequest *request);
    void handleState(AsyncWebServerRequest *request);
    void handlePrio(AsyncWebServerRequest *request);
    void handleReload(AsyncWebServerRequest *request);
    void handleFire(AsyncWebServerRequest *request);
    void handleResources(AsyncWebServerRequest *request);
    void handleValidate(AsyncWebServerRequest *request);
    void handleSetCron(AsyncWebServerRequest *request);
    void handleWrite(AsyncWebServerRequest *request);
    void handleGet(AsyncWebServerRequest *request);

    // Конфиг
    void defaultConfig();
    bool loadConfig();
    bool saveConfig();
    bool saveMeta();                         // сохранить массив _files в config_macros.json

    // Логика работы со списком файлов
    int  findFile(const String& name);       // индекс в _files или -1
    bool nameOk(const String& name);         // проверка имени (без пути)
    void bumpMeta();                         // отметить изменение списка (пересборка в tick)
    void reconcileList();                    // скан /macros + слияние с метой + сохранение
    void ensureMacrosDir();                  // создать /macros и пример example.lua
    String readFile(const String& path);     // содержимое файла в String
    bool writeFile(const String& path, const String& data);
    bool fsRenameFile(const String& oldPath, const String& newPath); // копия + удаление
    String uniqueNewName(const String& tmpl);// свободное имя (new_macros.lua, new_macros1.lua, ...)

    // Операции со списком (общие для HTTP и терминала)
    bool addFileEntry(const String& base, uint8_t prio, bool run); // новый файл в списке
    bool createNewFile(const String& base, String& fullPath);      // создать файл и запись
    bool deleteFileEntry(const String& base);                      // удалить файл и запись
    bool setFileRun(const String& base, bool on);                  // запустить/остановить
    bool setFilePrio(const String& base, int8_t delta);            // приоритет ± (clamp 0..7)
    bool renameFileEntry(const String& oldBase, const String& newBase);

    // Движок сценариев (вызывается только в main-loop: tick/терминал)
    bool parseScript(MacroFile& f);          // разобрать файл в таблицу сущностей
    void destroyScript(MacroFile& f);        // освободить интерпретатор
    void rebuildScripts();                   // синхронизация _files -> интерпретаторы
    String runBody(MacroFile& f, int bodyRef);            // выполнить функцию-тело, вернуть ошибку
    void execBody(MacroFile& f, MacroEntity& e);          // выполнить тело сущности и записать ошибку
    bool evalCondEntity(MacroFile& f, MacroEntity& e, String& errTxt); // вычислить условие cond
    void drainEvents();                      // обработка очереди внешних событий
    void tickStep();                         // шаг исполнения (cron/cond) за одну секунду

protected:
    fs::LittleFSFS* _fs;

    strMacrosConfig _config;
    MacroFile _files[MACRO_MAX_FILES];
    uint8_t _fileCount;
    uint8_t _metaRev;        // счётчик изменений меты (для страницы — не используется напрямую)
    uint8_t _scriptRev;      // счётчик: при изменении tick пересобирает интерпретаторы
    uint8_t _lastScriptRev;  // последний обработанный tick-ом _scriptRev
    bool _ntpWasSynced;      // для инициализации cron после первой синхронизации NTP

    MacroEvent _evQueue[MACRO_EV_QUEUE];     // очередь внешних событий (term/button)
    uint8_t _evIn;
    uint8_t _evOut;

    friend void macroTickTask();   // 1-сек задача EERTOS
    friend void macroCmd();        // обработчик терминальной команды "macro"
};

extern CLASS_MODULE_MACROS module_macros;

// Терминальные команды модуля (регистрируются через TerminalRegisterModule)
void macroTerminalRegister();

#endif // _MODULE_MACROS_h
