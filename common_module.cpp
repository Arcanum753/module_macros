
#include "common_module.h"

#include <string.h>

#include "ccronexpr.h"

// ============================================================
// Вспомогательные функции модуля macros
// ============================================================

namespace ns_module_macros {

String macroFileBaseName(const String& pathOrName) {
    int slash = pathOrName.lastIndexOf('/');
    if (slash >= 0) { return pathOrName.substring(slash + 1); }
    return pathOrName;
}

String macroCronNormalize(const String& expr) {
    String s = expr;
    s.trim();
    if (s.length() == 0) { return s; }

    // Считаем поля по пробелам.
    int fields = 0;
    bool inField = false;
    for (unsigned int i = 0; i < s.length(); i++) {
        bool sep = (s[i] == ' ' || s[i] == '\t');
        if (sep) {
            inField = false;
        } else if (!inField) {
            inField = true;
            fields++;
        }
    }

    // 5 полей — без секунд: добавляем ведущий "0 ".
    if (fields == 5) { return String("0 ") + s; }
    return s;
}

bool macroCronValid(const String& expr, String& err) {
    err = "";
    String norm = macroCronNormalize(expr);
    if (norm.length() == 0) { err = "empty cron"; return false; }
    const char* perr = NULL;
    cron_expr cx;
    memset(&cx, 0, sizeof(cx));
    cron_parse_expr(norm.c_str(), &cx, &perr);
    if (perr != NULL) { err = String(perr); return false; }
    return true;
}

} // namespace ns_module_macros
