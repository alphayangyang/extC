#include "prelude.h"

/* 由 tools/embed.c 生成（build/prelude_data.c） */
extern const unsigned char extc_prelude[];
extern const unsigned long extc_prelude_len;

const char *preludeSource(size_t *len) {
    *len = (size_t)extc_prelude_len;
    return (const char *)extc_prelude;
}
