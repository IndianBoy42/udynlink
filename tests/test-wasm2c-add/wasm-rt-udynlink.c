#include "wasm-rt.h"

static bool g_initialized = true;

void wasm_rt_init(void) { g_initialized = true; }
bool wasm_rt_is_initialized(void) { return g_initialized; }
void wasm_rt_free(void) { g_initialized = false; }

WASM_RT_NO_RETURN void wasm_rt_trap(wasm_rt_trap_t trap) {
    (void)trap;
    while (1) {}
}

const char* wasm_rt_strerror(wasm_rt_trap_t trap) {
    (void)trap;
    return "trap";
}

/* Stubs for memory/table operations - not used by this PoC */
void wasm_rt_allocate_memory(wasm_rt_memory_t* mem, uint64_t initial_pages,
                             uint64_t max_pages, bool is64) {
    (void)mem; (void)initial_pages; (void)max_pages; (void)is64;
}
uint64_t wasm_rt_grow_memory(wasm_rt_memory_t* mem, uint64_t pages) {
    (void)mem; (void)pages;
    return 0xffffffffu;
}
void wasm_rt_free_memory(wasm_rt_memory_t* mem) { (void)mem; }

void wasm_rt_allocate_funcref_table(wasm_rt_funcref_table_t* table,
                                    uint32_t elements, uint32_t max_elements) {
    (void)table; (void)elements; (void)max_elements;
}
void wasm_rt_free_funcref_table(wasm_rt_funcref_table_t* table) { (void)table; }
void wasm_rt_allocate_externref_table(wasm_rt_externref_table_t* table,
                                       uint32_t elements, uint32_t max_elements) {
    (void)table; (void)elements; (void)max_elements;
}
void wasm_rt_free_externref_table(wasm_rt_externref_table_t* table) { (void)table; }
uint32_t wasm_rt_grow_funcref_table(wasm_rt_funcref_table_t* table,
                                     uint32_t delta, wasm_rt_funcref_t init) {
    (void)table; (void)delta; (void)init;
    return 0xffffffffu;
}
uint32_t wasm_rt_grow_externref_table(wasm_rt_externref_table_t* table,
                                       uint32_t delta, wasm_rt_externref_t init) {
    (void)table; (void)delta; (void)init;
    return 0xffffffffu;
}
