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

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Защита runtime модуля от гонок: веб-обработчики AsyncWebServer выполняются в
// задаче AsyncTCP, а тик «барабана» — в loop-задаче. Один рекурсивный мьютекс
// сериализует доступ к _files/_lua/очереди событий между ними.
struct MacroLockGuard {
    SemaphoreHandle_t m;
    explicit MacroLockGuard(SemaphoreHandle_t s) : m(s) {
        if (m != nullptr) { xSemaphoreTakeRecursive(m, portMAX_DELAY); }
    }
    ~MacroLockGuard() {
        if (m != nullptr) { xSemaphoreGiveRecursive(m); }
    }
};

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
    bool fireToken(uint8_t type, const String& spec,
                   const BusValue& value = BusValue()); // событие term/button/on в очередь
    uint8_t getFileCount();
    void printList();                        // вывод списка сценариев в терминал
    void ensureEventSub(const String& name); // подписка на событие шины (для on())

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
    void handleHeap(AsyncWebServerRequest *request);
    void handleWrite(AsyncWebServerRequest *request);
    void handleGet(AsyncWebServerRequest *request);
    // Управление модулями (перенесено из core_state): список и смена режима.
    void handleModules(AsyncWebServerRequest *request);
    void handleModuleMode(AsyncWebServerRequest *request);

    // Конфиг
    void defaultConfig();
    bool loadConfig();
    bool saveConfig();
    bool saveMeta();                         // сохранить массив _files в config_macros.json

    // Логика работы со списком файлов
    int  findFile(const String& name);       // индекс в _files или -1
    uint8_t runningCount();                  // число запущенных файлов (run==true)
    bool nameOk(const String& name);         // проверка имени (без пути)
    void markAllDirty();                     // пометить все run-файлы на переразбор
    void bumpMeta();                         // отметить изменение списка (пересборка в tick)
    void reconcileList();                    // скан /macros + слияние с метой + сохранение
    void ensureMacrosDir();                  // создать /macros
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

    // Движок «барабана» (вызывается только в main-loop: tick/терминал)
    bool initLua();                          // создать единый интерпретатор и команды
    bool registerFile(MacroFile& f);         // скомпилировать чанк и создать окружение/state
    void unregisterFile(MacroFile& f);       // снять обработчики/подписки, освободить refs
    void rebuildScripts();                   // (пере)компиляция только needParse
    bool anyNeedParse();                     // есть ли файлы, требующие компиляции
    void refreshFileMeta(MacroFile& f);      // читать desc без запуска живого файла
    void pruneEventSubs();                   // снять неиспользуемые подписки
    void drainEvents();                      // обработка очереди внешних событий
    void tickStep();                         // шаг «барабана» за одну секунду
    size_t estimateHeapBytes();              // оценка heap, занятого модулем

protected:
    fs::LittleFSFS* _fs;

    SemaphoreHandle_t _lock;  // рекурсивный мьютекс runtime модуля (loop vs AsyncTCP)

    strMacrosConfig _config;
    MacroFile* _files;       // массив файлов-сценариев (heap, MACRO_MAX_FILES)
    uint8_t _fileCount;
    bool _ntpWasSynced;      // для инициализации cron после первой синхронизации NTP

    EspLuaEngine* _lua;      // единый Lua-интерпретатор «барабана» (heap)
    size_t _luaHeapBytes;    // измеренный расход heap на _lua (для валидации)

    struct { String name; uint32_t handle; } _evtSubs[MACRO_MAX_EVENT_SUBS];
    uint8_t _nEvtSubs;

    String _resourcesJson;    // последний успешный каталог ресурсов (кэш + fallback)
    bool   _resourcesReady;   // в _resourcesJson лежит валидная строка
    uint32_t _resourcesMs;    // millis() последней сборки каталога (кэш по TTL)

    MacroEvent _evQueue[MACRO_EV_QUEUE];     // очередь внешних событий (term/button/on)
    uint8_t _evIn;
    uint8_t _evOut;

    time_t _lastTickNow = 0;   // now() прошлого тика (детект скачка времени)
    uint8_t _bodyCursor = 0;   // курсор round-robin исполнения тел сценариев

    friend void macroTickTask();   // 1-сек задача EERTOS
    friend void macroCmd();        // обработчик терминальной команды "macro"
};

extern CLASS_MODULE_MACROS module_macros;

// Терминальные команды модуля (регистрируются через TerminalRegisterModule)
void macroTerminalRegister();

#endif // _MODULE_MACROS_h
