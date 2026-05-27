/* wasm-rt-udynlink.c — Bare-metal wasm2c runtime for udynlink
 *
 * Hook-based design: weak symbols for malloc, free, trap handler, and import
 * resolution default to the udynlink host callbacks.  The host firmware can
 * override any of them by providing a strong definition.
 *
 * Supports both static and dynamic linear memory models.  Static mode is
 * triggered by defining WASM_RT_STATIC_MEMORY and WASM_RT_INITIAL_PAGES in
 * the per-module shim.
 */

#include "wasm-rt.h"

/* -------------------------------------------------------------------------- */
/*  Essential libc-compatible string functions (weak, so host can override)     */
/* -------------------------------------------------------------------------- */

/* These are referenced by wasm2c-generated code (memory_fill, memory_copy,
 * func_types_eq, etc.).  We provide small builtin-based fallbacks because
 * the module links with -nostdlib.  Mark them used so --gc-sections does
 * not discard them when they are only reached from inline functions.        */

/* Provide bare-metal libc-compatible string functions.  These are
 * referenced by wasm2c-generated code (memory_fill, memory_copy,
 * func_types_eq, etc.) and by compiler lowers of __builtin_memcpy.
 *
 * Mark them used so --gc-sections does not discard them when they are
 * only reached from inline functions.  We name them with the standard
 * libc names because the compiler may lower __builtin_memcpy to a
 * call to memcpy when the copy is too large to inline, and we link
 * with -nostdlib so there is no libc conflict.                    */

__attribute__((weak, used))
void* memcpy(void* dest, const void* src, size_t n) {
    uint8_t* d = dest;
    const uint8_t* s = src;
    while (n--) *d++ = *s++;
    return dest;
}

__attribute__((weak, used))
void* memset(void* s, int c, size_t n) {
    uint8_t* p = s;
    while (n--) *p++ = (uint8_t)c;
    return s;
}

__attribute__((weak, used))
void* memmove(void* dest, const void* src, size_t n) {
    uint8_t* d = dest;
    const uint8_t* s = src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else if (d > s) {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dest;
}

__attribute__((weak, used))
int memcmp(const void* s1, const void* s2, size_t n) {
    const uint8_t* p1 = s1;
    const uint8_t* p2 = s2;
    while (n--) {
        if (*p1 != *p2) return (int)*p1 - (int)*p2;
        p1++; p2++;
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/*  Host integration hooks (weak → udynlink_external_*)                      */
/* -------------------------------------------------------------------------- */

#include "udynlink_externals.h"

__attribute__((weak)) void* wasm_rt_malloc(size_t size) {
    return udynlink_external_malloc(size);
}

__attribute__((weak)) void wasm_rt_mem_free(void* p) {
    udynlink_external_free(p);
}

__attribute__((weak)) void* wasm_rt_mem_realloc(void* p, size_t size) {
    /* Fallback: malloc + memcpy + free.  Host can override with a real
     * realloc to avoid the copy. */
    if (size == 0) {
        if (p) wasm_rt_mem_free(p);
        return NULL;
    }
    void* np = wasm_rt_malloc(size);
    if (!np) return NULL;
    if (p) {
        /* We don't know the old size; copy what we can.  For grow_memory
         * the new size is always >= old size, so this is safe. */
        wasm_rt_memcpy(np, p, size);
        wasm_rt_mem_free(p);
    }
    return np;
}

__attribute__((weak)) void wasm_rt_trap_handler(wasm_rt_trap_t code) {
    /* Default: no-op.  Host can override to log the trap via
     * udynlink_external_vprintf or a custom mechanism.
     * wasm_rt_trap() enters the fatal infinite loop after this returns. */
    (void)code;
}

__attribute__((weak)) void* wasm_rt_resolve_import(const char* module,
                                                    const char* name) {
    (void)module;
    return (void*)udynlink_external_resolve_symbol(name);
}

/* -------------------------------------------------------------------------- */
/*  Trap reporting                                                            */
/* -------------------------------------------------------------------------- */

static const char* const trap_names[] = {
    [WASM_RT_TRAP_NONE]               = "none",
    [WASM_RT_TRAP_OOB]                = "out of bounds",
    [WASM_RT_TRAP_INT_OVERFLOW]       = "integer overflow",
    [WASM_RT_TRAP_DIV_BY_ZERO]        = "divide by zero",
    [WASM_RT_TRAP_INVALID_CONVERSION] = "invalid conversion",
    [WASM_RT_TRAP_UNREACHABLE]        = "unreachable",
    [WASM_RT_TRAP_CALL_INDIRECT]      = "call indirect",
    [WASM_RT_TRAP_UNCAUGHT_EXCEPTION] = "uncaught exception",
    [WASM_RT_TRAP_UNALIGNED]          = "unaligned",
    [WASM_RT_TRAP_EXHAUSTION]         = "exhaustion",
};

const char* wasm_rt_strerror(wasm_rt_trap_t trap) {
    if (trap < sizeof(trap_names) / sizeof(trap_names[0])) {
        return trap_names[trap];
    }
    return "unknown trap";
}

WASM_RT_NO_RETURN void wasm_rt_trap(wasm_rt_trap_t trap) {
    wasm_rt_trap_handler(trap);
    while (1) {
        __asm__ volatile("bkpt #0" ::: "memory");
    }
}

/* -------------------------------------------------------------------------- */
/*  Runtime lifecycle                                                         */
/* -------------------------------------------------------------------------- */

static bool g_initialized = false;

__attribute__((used)) void wasm_rt_init(void) {
    g_initialized = true;
}

__attribute__((used)) bool wasm_rt_is_initialized(void) {
    return g_initialized;
}

__attribute__((used)) void wasm_rt_free(void) {
    g_initialized = false;
}

/* -------------------------------------------------------------------------- */
/*  Static linear memory buffer (one per compiled module)                      */
/* -------------------------------------------------------------------------- */

#ifdef WASM_RT_STATIC_MEMORY
#ifndef WASM_RT_INITIAL_PAGES
#define WASM_RT_INITIAL_PAGES 1
#endif
#ifndef WASM_RT_PAGE_SIZE
#define WASM_RT_PAGE_SIZE 65536
#endif

static uint8_t wasm_rt_linear_memory[WASM_RT_INITIAL_PAGES * WASM_RT_PAGE_SIZE]
    __attribute__((used, section(".bss")));
#endif

/* -------------------------------------------------------------------------- */
/*  Memory API                                                                */
/* -------------------------------------------------------------------------- */

__attribute__((used)) void wasm_rt_allocate_memory(wasm_rt_memory_t* mem,
                                                   uint64_t initial_pages,
                                                   uint64_t max_pages,
                                                   bool is64) {
#ifdef WASM_RT_STATIC_MEMORY
    (void)initial_pages;
    (void)max_pages;
    mem->data = wasm_rt_linear_memory;
    mem->pages = WASM_RT_INITIAL_PAGES;
    mem->max_pages = WASM_RT_INITIAL_PAGES;
    mem->size = WASM_RT_INITIAL_PAGES * WASM_RT_PAGE_SIZE;
    mem->is64 = is64;
#else
    size_t page_size = is64 ? (1ULL << 48) : WASM_RT_PAGE_SIZE;
    size_t alloc_size = (size_t)(initial_pages * page_size);
    mem->data = (uint8_t*)wasm_rt_malloc(alloc_size);
    mem->pages = initial_pages;
    mem->max_pages = max_pages;
    mem->size = alloc_size;
    mem->is64 = is64;
#endif
}

__attribute__((used)) uint64_t wasm_rt_grow_memory(wasm_rt_memory_t* mem,
                                                   uint64_t pages) {
#ifdef WASM_RT_STATIC_MEMORY
    (void)mem;
    (void)pages;
    /* Static memory cannot grow. */
    return 0xffffffffu;
#else
    if (mem->pages + pages > mem->max_pages) {
        return 0xffffffffu;
    }
    size_t page_size = mem->is64 ? (1ULL << 48) : WASM_RT_PAGE_SIZE;
    size_t old_size = (size_t)(mem->pages * page_size);
    size_t new_size = old_size + (size_t)(pages * page_size);
    uint8_t* new_data = (uint8_t*)wasm_rt_mem_realloc(mem->data, new_size);
    if (!new_data) {
        return 0xffffffffu;
    }
    mem->data = new_data;
    mem->pages += pages;
    mem->size = new_size;
    return (mem->pages - pages); /* old page count on success */
#endif
}

__attribute__((used)) void wasm_rt_free_memory(wasm_rt_memory_t* mem) {
#ifndef WASM_RT_STATIC_MEMORY
    if (mem->data) {
        wasm_rt_mem_free(mem->data);
        mem->data = NULL;
    }
#else
    (void)mem;
#endif
}

/* -------------------------------------------------------------------------- */
/*  Table API (dynamic allocation via wasm_rt_malloc)                         */
/* -------------------------------------------------------------------------- */

__attribute__((used)) void wasm_rt_allocate_funcref_table(wasm_rt_funcref_table_t* table,
                                                          uint32_t elements,
                                                          uint32_t max_elements) {
    table->data = (wasm_rt_funcref_t*)wasm_rt_malloc(max_elements * sizeof(wasm_rt_funcref_t));
    table->size = elements;
    table->max_size = max_elements;
}

__attribute__((used)) void wasm_rt_free_funcref_table(wasm_rt_funcref_table_t* table) {
    if (table->data) {
        wasm_rt_mem_free(table->data);
        table->data = NULL;
    }
}

__attribute__((used)) void wasm_rt_allocate_externref_table(wasm_rt_externref_table_t* table,
                                                             uint32_t elements,
                                                             uint32_t max_elements) {
    table->data = (wasm_rt_externref_t*)wasm_rt_malloc(max_elements * sizeof(wasm_rt_externref_t));
    table->size = elements;
    table->max_size = max_elements;
}

__attribute__((used)) void wasm_rt_free_externref_table(wasm_rt_externref_table_t* table) {
    if (table->data) {
        wasm_rt_mem_free(table->data);
        table->data = NULL;
    }
}

__attribute__((used)) uint32_t wasm_rt_grow_funcref_table(wasm_rt_funcref_table_t* table,
                                                           uint32_t delta,
                                                           wasm_rt_funcref_t init) {
    if (table->size + delta > table->max_size) {
        return 0xffffffffu;
    }
    for (uint32_t i = 0; i < delta; i++) {
        table->data[table->size + i] = init;
    }
    table->size += delta;
    return table->size - delta;
}

__attribute__((used)) uint32_t wasm_rt_grow_externref_table(wasm_rt_externref_table_t* table,
                                                               uint32_t delta,
                                                               wasm_rt_externref_t init) {
    if (table->size + delta > table->max_size) {
        return 0xffffffffu;
    }
    for (uint32_t i = 0; i < delta; i++) {
        table->data[table->size + i] = init;
    }
    table->size += delta;
    return table->size - delta;
}
