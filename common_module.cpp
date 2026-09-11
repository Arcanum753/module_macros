
#include "common_module.h"

// ============================================================
// Вспомогательные функции модуля macros
// ============================================================

namespace ns_module_macros {

String macroFileBaseName(const String& pathOrName) {
    int slash = pathOrName.lastIndexOf('/');
    if (slash >= 0) { return pathOrName.substring(slash + 1); }
    return pathOrName;
}

} // namespace ns_module_macros
