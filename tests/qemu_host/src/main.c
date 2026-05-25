//
// This file is part of the GNU ARM Eclipse distribution.
// Copyright (c) 2014 Liviu Ionescu.
//

// ----------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include "udynlink.h"

///////////////////////////////////////////////////////////////////////////////

void *udynlink_external_malloc(size_t size) {
    return malloc(size);
}

void udynlink_external_free(void *p) {
    free(p);
}

void udynlink_external_vprintf(const char *s, va_list va) {
    vprintf(s, va);
}

uint32_t test_resolve_symbol(const char *name) __attribute__((weak));
uint32_t test_resolve_symbol(const char *name) {
    (void*)name;
    return 0;
}

extern int _write(int file, char *ptr, int len);

uint32_t udynlink_external_resolve_symbol(const char *name) {
    if (!strcmp(name, "printf"))
        return (uint32_t)&printf;
    else if (!strcmp(name, "_write"))
        return (uint32_t)&_write;
    else
        return test_resolve_symbol(name);
}

///////////////////////////////////////////////////////////////////////////////
// Test me!

extern int test_qemu(void);

int main() {
#ifdef UDYNLINK_TEST_DEBUG_LEVEL
    udynlink_set_debug_level(UDYNLINK_TEST_DEBUG_LEVEL);
#else
    udynlink_set_debug_level(UDYNLINK_DEBUG_NONE);
#endif
    int ok = test_qemu();
    printf (ok ? "*** TEST OK ***\n" : "*** TEST FAILED! ***\n");
    exit(ok ? 0 : 1);
}
