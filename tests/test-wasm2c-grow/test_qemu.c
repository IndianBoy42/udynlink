#include "udynlink.h"
#include "mod_wasm2c_grow_module_data.h"
#include "test_utils.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Static pool backing the module's linear memory: proves the dynamic
 * memory model works without a real heap.  The module builds with a 1 KiB
 * wasm page (memory 1..4), so the bump allocator never needs to reclaim:
 * worst case 1+2+4 KiB of live buffers fits easily. */
#define POOL_SIZE (32 * 1024)
static uint8_t g_pool[POOL_SIZE] __attribute__((aligned(8)));
static size_t g_pool_used;

void *wasm_pool_malloc(size_t size) {
    if (size > POOL_SIZE - g_pool_used)
        return NULL;
    void *p = &g_pool[g_pool_used];
    g_pool_used += (size + 7u) & ~(size_t)7u;
    return p;
}

void wasm_pool_free(void *p) {
    (void)p; /* bump allocator: reclamation happens at unload */
}

uintptr_t test_resolve_symbol(const char *name) {
    if (!strcmp(name, "wasm_pool_malloc"))
        return (uintptr_t)&wasm_pool_malloc;
    if (!strcmp(name, "wasm_pool_free"))
        return (uintptr_t)&wasm_pool_free;
    return 0;
}

int test_qemu(void) {
    const char *extern_syms[] = {"wasm_pool_malloc", "wasm_pool_free", NULL};
    const char *exported_syms[] = {"grow", "size", "poke", "peek", NULL};
    udynlink_module_t mod;
    udynlink_sym_t sym_grow, sym_size, sym_poke, sym_peek;
    int res = 0;

    memset(&mod, 0, sizeof(mod));

    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i ++) {
        if (test_load_module(&mod, mod_wasm2c_grow_module_data, NULL, 0, (udynlink_load_mode_t)i))
            return 0;
        if (!check_extern_symbols(&mod, extern_syms) ||
            !check_exported_symbols(&mod, exported_syms))
            goto exit;
        if (udynlink_lookup_symbol(&mod, "grow", &sym_grow) == NULL ||
            udynlink_lookup_symbol(&mod, "size", &sym_size) == NULL ||
            udynlink_lookup_symbol(&mod, "poke", &sym_poke) == NULL ||
            udynlink_lookup_symbol(&mod, "peek", &sym_peek) == NULL)
            goto exit;
        {
            uint32_t (*grow)(uint32_t) = (uint32_t (*)(uint32_t))sym_grow.val;
            uint32_t (*size)(void) = (uint32_t (*)(void))sym_size.val;
            void (*poke)(uint32_t, uint32_t) = (void (*)(uint32_t, uint32_t))sym_poke.val;
            uint32_t (*peek)(uint32_t) = (uint32_t (*)(uint32_t))sym_peek.val;

            /* Write inside the initial (1 KiB) page, then grow: the realloc
             * path must preserve the contents (old-size contract, D4). */
            UDYNLINK_PREPARE_CALL(&mod);
            poke(100, 0x12345678u);
            UDYNLINK_PREPARE_CALL(&mod);
            uint32_t old_pages = grow(1);
            UDYNLINK_PREPARE_CALL(&mod);
            uint32_t pages = size();
            printf("grow: %u -> %u pages, size %u\n", old_pages, pages, pages);
            if (old_pages != 1 || pages != 2)
                goto exit;

            UDYNLINK_PREPARE_CALL(&mod);
            if (peek(100) != 0x12345678u)
                goto exit;
            printf("mem[100] = 12345678\n");
            printf("content preserved after realloc\n");

            /* Write and read beyond the initial page boundary. */
            UDYNLINK_PREPARE_CALL(&mod);
            poke(1500, 54321u);
            UDYNLINK_PREPARE_CALL(&mod);
            if (peek(1500) != 54321u)
                goto exit;

            /* Growing past max_pages fails with -1; size is unchanged. */
            UDYNLINK_PREPARE_CALL(&mod);
            if (grow(10) != 0xFFFFFFFFu)
                goto exit;
            printf("grow beyond max rejected\n");

            UDYNLINK_PREPARE_CALL(&mod);
            old_pages = grow(2);
            UDYNLINK_PREPARE_CALL(&mod);
            pages = size();
            if (old_pages != 2 || pages != 4)
                goto exit;
            UDYNLINK_PREPARE_CALL(&mod);
            if (grow(1) != 0xFFFFFFFFu)
                goto exit;
            printf("post-max grow rejected\n");

            /* Data written before all the growing still reads back. */
            UDYNLINK_PREPARE_CALL(&mod);
            if (peek(100) != 0x12345678u)
                goto exit;

            g_pool_used = 0; /* pool is reused by the next load mode */
            udynlink_unload_module(&mod);
            continue;
        }
    }
    res = 1;
exit:
    udynlink_unload_module(&mod);
    return res;
}
