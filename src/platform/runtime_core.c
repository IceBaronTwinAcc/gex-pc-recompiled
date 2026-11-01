#include "platform/runtime.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

char *gex_string_duplicate(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy) memcpy(copy, text, size);
    return copy;
}

int gex_string_compare_case_n(const char *left, const char *right, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        int a = tolower((unsigned char)left[i]), b = tolower((unsigned char)right[i]);
        if (a != b) return a - b;
        if (!a) return 0;
    }
    return 0;
}

int gex_string_compare_case(const char *left, const char *right) {
    return gex_string_compare_case_n(left, right, SIZE_MAX);
}

void gex_call_once(gex_once *once, void (*initialize)(void)) {
    unsigned expected = 0;
    if (atomic_compare_exchange_strong(&once->state, &expected, 1)) {
        initialize();
        atomic_store(&once->state, 2);
    } else {
        while (atomic_load(&once->state) != 2) gex_runtime_yield();
    }
}
