/* wasm-rt-udynlink.c — Bare-metal wasm2c runtime for udynlink
 *
 * Hook-based design: weak symbols for malloc, free, trap handler, and import
 * resolution default to the udynlink host callbacks.  The host firmware can
 * override any of them by providing a strong definition.
 *
 * Supports three linear-memory models, selected by defines in the generated
 * wasm_rt_config.h: static (.bss buffer), dynamic (allocation hooks), and
 * external (host-registered buffer via wasm_rt_set_external_memory).
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

/* The runtime owns no tagged sections: its allocations (linear memory,
 * funcref/externref tables) all live in the host's default pool, hence
 * section = NULL. Alignment 8 covers the widest thing stored through these
 * buffers (f64/i64 in linear memory, pointers in tables) on every target
 * this runtime builds for; the wasm linear memory is page-granular anyway. */
#define WASM_RT_HOOK_ALIGN 8u

__attribute__((weak)) void* wasm_rt_malloc(size_t size) {
    return udynlink_external_malloc(size, NULL, WASM_RT_HOOK_ALIGN, 0);
}

__attribute__((weak)) void wasm_rt_mem_free(void* p) {
    udynlink_external_free(p, NULL, WASM_RT_HOOK_ALIGN, 0);
}

__attribute__((weak)) void* wasm_rt_mem_realloc(void* p, size_t old_size, size_t new_size) {
    /* Fallback: malloc + copy + free.  The caller supplies the old buffer
     * size (bare-metal allocators don't track it), so the copy is exact.
     * Host can override with a real realloc to avoid the copy. */
    if (new_size == 0) {
        if (p) wasm_rt_mem_free(p);
        return NULL;
    }
    void* np = wasm_rt_malloc(new_size);
    if (!np) return NULL;
    if (p) {
        wasm_rt_memcpy(np, p, old_size < new_size ? old_size : new_size);
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

__attribute__((weak)) void* wasm_rt_resolve_import(const udynlink_module_t *p_mod,
                                                    const char* module,
                                                    const char* name) {
    (void)module;
    return (void*)udynlink_external_resolve_symbol(p_mod, name);
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
    [WASM_RT_TRAP_OOM]                = "out of memory",
};

const char* wasm_rt_strerror(wasm_rt_trap_t trap) {
    if (trap < sizeof(trap_names) / sizeof(trap_names[0])) {
        return trap_names[trap];
    }
    return "unknown trap";
}

/* -------------------------------------------------------------------------- */
/*  Trap reporting & recovery                                                  */
/* -------------------------------------------------------------------------- */

static wasm_rt_trap_t g_last_trap = WASM_RT_TRAP_NONE;

wasm_rt_trap_t wasm_rt_last_trap(void) {
    return g_last_trap;
}

#ifdef WASM_RT_ENABLE_RECOVERY
/* One-shot recovery point, registered by the host (wasm_rt_set_recovery)
 * or by --wrappers-recover wrappers.  Consumed by the first trap. */
static jmp_buf* g_recovery = NULL;

void wasm_rt_set_recovery(jmp_buf* jb) {
    g_recovery = jb;
}
#endif

WASM_RT_NO_RETURN void wasm_rt_trap(wasm_rt_trap_t trap) {
    g_last_trap = trap;
#ifdef WASM_RT_ENABLE_RECOVERY
    if (g_recovery != NULL) {
        /* Host-registered recovery point: unwind to the host frame instead
         * of halting.  One-shot — the registration is consumed so a stale
         * frame is never longjmp'd into.  longjmp is resolved from the host
         * at load time like any other import. */
        jmp_buf* jb = g_recovery;
        g_recovery = NULL;
#if WASM_RT_USE_STACK_DEPTH_COUNT
        /* The longjmp abandons every wasm frame; without the reset the
         * depth counter would leak and falsely exhaust later calls. */
        wasm_rt_call_stack_depth = 0;
#endif
        longjmp(*jb, 1);
    }
#endif
#ifdef WASM_RT_TRAP_HANDLER
    /* Handler name baked at build time (mkwasm2c-module --trap-handler).
     * It is not defined inside the module: the host provides it and the
     * loader resolves it at load time like any other import. */
    WASM_RT_TRAP_HANDLER(trap);
#else
    wasm_rt_trap_handler(trap);
#endif
    while (1) {
#if defined(__arm__) || defined(__thumb__) || defined(__aarch64__)
        __asm__ volatile("bkpt #0" ::: "memory");
#else
        /* Host-native test builds (docs/host-testing.md): trap visibly
         * instead of assembling an ARM bkpt instruction. */
        __builtin_trap();
#endif
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

#if WASM_RT_USE_STACK_DEPTH_COUNT
/* Referenced by wasm2c-generated FUNC_PROLOGUE / FUNC_EPILOGUE. */
uint32_t wasm_rt_call_stack_depth = 0;
#endif

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

#ifdef WASM_RT_EXTERNAL_MEMORY
/* Host-registered linear memory buffer (external memory mode).  The whole
 * capacity is reserved up front, so growth within the registered capacity
 * needs no reallocation. */
static uint8_t* g_ext_mem_buf = NULL;
static size_t   g_ext_mem_capacity = 0;

void wasm_rt_set_external_memory(void* buf, size_t capacity_bytes) {
    g_ext_mem_buf = (uint8_t*)buf;
    g_ext_mem_capacity = capacity_bytes;
}
#endif

/* -------------------------------------------------------------------------- */
/*  Memory API                                                                */
/* -------------------------------------------------------------------------- */

__attribute__((used)) void wasm_rt_allocate_memory(wasm_rt_memory_t* mem,
                                                   uint64_t initial_pages,
                                                   uint64_t max_pages,
                                                   bool is64) {
    mem->is64 = is64;
#ifdef WASM_RT_STATIC_MEMORY
    (void)initial_pages;
    (void)max_pages;
    mem->data = wasm_rt_linear_memory;
    mem->pages = WASM_RT_INITIAL_PAGES;
    mem->max_pages = WASM_RT_INITIAL_PAGES;
    mem->size = WASM_RT_INITIAL_PAGES * WASM_RT_PAGE_SIZE;
#else
    size_t page_size = is64 ? (1ULL << 48) : WASM_RT_PAGE_SIZE;
    size_t need = (size_t)(initial_pages * page_size);
    mem->pages = initial_pages;
#ifdef WASM_RT_EXTERNAL_MEMORY
    if (g_ext_mem_buf == NULL || g_ext_mem_capacity < need) {
        /* Almost always means wasm_rt_set_external_memory() was never
         * called, or the buffer is too small. */
        wasm_rt_trap(WASM_RT_TRAP_OOM);
    }
    mem->data = g_ext_mem_buf;
    uint64_t capacity_pages = g_ext_mem_capacity / page_size;
    mem->max_pages = max_pages < capacity_pages ? max_pages : capacity_pages;
    mem->size = need;
#else /* dynamic */
    mem->data = (uint8_t*)wasm_rt_malloc(need);
    if (need != 0 && mem->data == NULL) {
        wasm_rt_trap(WASM_RT_TRAP_OOM);
    }
    mem->max_pages = max_pages;
    mem->size = need;
#endif
#endif
}

__attribute__((used)) uint64_t wasm_rt_grow_memory(wasm_rt_memory_t* mem,
                                                   uint64_t pages) {
    if (mem->pages + pages > mem->max_pages) {
        return 0xffffffffu;
    }
    uint64_t old_pages = mem->pages;
#ifdef WASM_RT_EXTERNAL_MEMORY
    /* The host reserved the full capacity up front; growing within it is a
     * pure bookkeeping update.  For static memory the check above always
     * fails because max_pages == initial pages, so grow fails, as designed. */
    size_t page_size = mem->is64 ? (1ULL << 48) : WASM_RT_PAGE_SIZE;
    mem->pages += pages;
    mem->size = (size_t)(mem->pages * page_size);
    return old_pages;
#else /* dynamic */
    size_t page_size = mem->is64 ? (1ULL << 48) : WASM_RT_PAGE_SIZE;
    size_t old_size = (size_t)mem->size;
    size_t new_size = old_size + (size_t)(pages * page_size);
    uint8_t* new_data = (uint8_t*)wasm_rt_mem_realloc(mem->data, old_size, new_size);
    if (new_data == NULL) {
        return 0xffffffffu; /* legal wasm outcome: grow may fail */
    }
    mem->data = new_data;
    mem->pages += pages;
    mem->size = new_size;
    return old_pages;
#endif
}

__attribute__((used)) void wasm_rt_free_memory(wasm_rt_memory_t* mem) {
#if !defined(WASM_RT_STATIC_MEMORY) && !defined(WASM_RT_EXTERNAL_MEMORY)
    if (mem->data) {
        wasm_rt_mem_free(mem->data);
        mem->data = NULL;
    }
#else
    (void)mem; /* memory not owned by the runtime */
#endif
}

/* -------------------------------------------------------------------------- */
/*  Table API (dynamic allocation via wasm_rt_malloc)                         */
/* -------------------------------------------------------------------------- */

__attribute__((used)) void wasm_rt_allocate_funcref_table(wasm_rt_funcref_table_t* table,
                                                          uint32_t elements,
                                                          uint32_t max_elements) {
    if (max_elements == 0) {
        table->data = NULL;
        table->size = 0;
        table->max_size = 0;
        return;
    }
    table->data = (wasm_rt_funcref_t*)wasm_rt_malloc(max_elements * sizeof(wasm_rt_funcref_t));
    if (table->data == NULL) {
        wasm_rt_trap(WASM_RT_TRAP_OOM);
    }
    /* Wasm semantics: unfilled table slots are null funcrefs; a garbage
     * slot would turn call_indirect into a wild call. */
    wasm_rt_memset(table->data, 0, max_elements * sizeof(wasm_rt_funcref_t));
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
    if (max_elements == 0) {
        table->data = NULL;
        table->size = 0;
        table->max_size = 0;
        return;
    }
    table->data = (wasm_rt_externref_t*)wasm_rt_malloc(max_elements * sizeof(wasm_rt_externref_t));
    if (table->data == NULL) {
        wasm_rt_trap(WASM_RT_TRAP_OOM);
    }
    wasm_rt_memset(table->data, 0, max_elements * sizeof(wasm_rt_externref_t));
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
