// Raw boot-argument lookup.
//
// PE_parse_boot_argn is typed; a value such as "auto" can be misread when requested as an integer.
// Parsing PE_boot_args() directly keeps string values (auto/0/N, names, paths) reliable.
#pragma once
#include <stddef.h>
#include <string.h>
#include <pexpert/pexpert.h>

namespace BootArgs {

inline bool find(const char *name, char *out, size_t cap) {
    if (!name || !*name) return false;
    const char *args = PE_boot_args();
    if (!args) return false;
    const size_t n = strlen(name);
    const char *p = args;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *key = (*p == '-') ? p + 1 : p;
        if (!strncmp(key, name, n)) {
            const char term = key[n];
            if (term == '=' || term == ' ' || term == 0) {
                const char *v = key + n;
                if (*v == '=') v++;
                else if (*v == ' ' || *v == 0) {
                    v++;
                    while (*v == ' ') v++;
                }
                if (out && cap) {
                    size_t len = 0;
                    while (v[len] && v[len] != ' ' && len + 1 < cap) len++;
                    memcpy(out, v, len);
                    out[len] = 0;
                }
                return true;
            }
        }
        while (*p && *p != ' ') p++;
    }
    return false;
}

inline bool present(const char *name) {
    return find(name, nullptr, 0);
}

} // namespace BootArgs
