
#ifndef _MODULE_MACROS_COMMON_MODULE_h
#define _MODULE_MACROS_COMMON_MODULE_h

#include <Arduino.h>

// Вспомогательные функции модуля macros (чистые, без состояния).
namespace ns_module_macros {

// Базовое имя файла без пути
String macroFileBaseName(const String& pathOrName);

// Приведение cron к 6 полям: 5 полей получают ведущий "0 " (секунды).
String macroCronNormalize(const String& expr);

// Проверка cron-выражения (5 или 6 полей). При ошибке пишет текст в err.
bool macroCronValid(const String& expr, String& err);

} // namespace ns_module_macros

#endif // _MODULE_MACROS_COMMON_MODULE_h
