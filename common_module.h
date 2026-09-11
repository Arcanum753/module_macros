
#ifndef _MODULE_MACROS_COMMON_MODULE_h
#define _MODULE_MACROS_COMMON_MODULE_h

#include <Arduino.h>

// Вспомогательные функции модуля macros (чистые, без состояния).
namespace ns_module_macros {

// Базовое имя файла без пути
String macroFileBaseName(const String& pathOrName);

} // namespace ns_module_macros

#endif // _MODULE_MACROS_COMMON_MODULE_h
