/* Standalone dependency system for udynlink — implementation.
 *
 * See udynlink_deps.h for API documentation.
 *
 * Copyright (c) 2026 udynlink contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#include "udynlink_deps.h"
#include "udynlink_externals.h"
#include <string.h>

udynlink_module_t *udynlink_external_dep_load(const char *name)
    __attribute__((weak));
udynlink_module_t *udynlink_external_dep_load(const char *name) {
    (void)name;
    return NULL;
}

/* ─── Dependency manager ──────────────────────────────────────────── */

void udynlink_dep_mgr_init(udynlink_dep_mgr_t *mgr,
                           udynlink_dep_entry_t *buf, size_t cap) {
    memset(mgr, 0, sizeof(*mgr));
    mgr->entries = buf;
    mgr->capacity = cap;
}

/* ─── Dependency query helpers ──────────────────────────────────────── */

int udynlink_dep_is_dependency(const char *name) {
    if (name == NULL) return 0;
    return strncmp(name, UDYNLINK_DEP_PREFIX, UDYNLINK_DEP_PREFIX_LEN) == 0;
}

const char *udynlink_dep_get_name(const char *name) {
    if (!udynlink_dep_is_dependency(name)) return NULL;
    return name + UDYNLINK_DEP_PREFIX_LEN;
}

udynlink_module_t *udynlink_dep_find(udynlink_dep_mgr_t *mgr,
                                     const char *name) {
    if (mgr == NULL || name == NULL) return NULL;
    for (size_t i = 0; i < mgr->count; i++) {
        if (mgr->entries[i].p_mod == NULL) continue;
        const char *mod_name = udynlink_get_module_name(mgr->entries[i].p_mod);
        if (mod_name != NULL && strcmp(mod_name, name) == 0) {
            return mgr->entries[i].p_mod;
        }
    }
    return NULL;
}

/* ─── Circular dependency detection ────────────────────────────────── */

static int is_loading(udynlink_dep_mgr_t *mgr, const char *name) {
    for (size_t i = 0; i < mgr->loading_depth; i++) {
        if (strcmp(mgr->loading_stack[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

static void push_loading(udynlink_dep_mgr_t *mgr, const char *name) {
    if (mgr->loading_depth < UDYNLINK_DEP_MAX_DEPTH) {
        mgr->loading_stack[mgr->loading_depth++] = name;
    }
}

static void pop_loading(udynlink_dep_mgr_t *mgr) {
    if (mgr->loading_depth > 0) {
        mgr->loading_depth--;
    }
}

/* ─── Module registration ──────────────────────────────────────────── */

static int register_module(udynlink_dep_mgr_t *mgr, udynlink_module_t *p_mod) {
    if (mgr->count >= mgr->capacity) return -1;
    mgr->entries[mgr->count].p_mod = p_mod;
    mgr->entries[mgr->count].gateway = NULL;
    mgr->count++;
    return 0;
}

static void unregister_module(udynlink_dep_mgr_t *mgr, udynlink_module_t *p_mod) {
    for (size_t i = 0; i < mgr->count; i++) {
        if (mgr->entries[i].p_mod == p_mod) {
            mgr->entries[i] = mgr->entries[--mgr->count];
            return;
        }
    }
}

static udynlink_dep_entry_t *find_entry(udynlink_dep_mgr_t *mgr,
                                         const udynlink_module_t *p_mod) {
    for (size_t i = 0; i < mgr->count; i++) {
        if (mgr->entries[i].p_mod == p_mod) {
            return &mgr->entries[i];
        }
    }
    return NULL;
}

/* ─── Resolution helpers ────────────────────────────────────────────── */

uintptr_t udynlink_dep_resolve_dependency(udynlink_dep_mgr_t *mgr,
                                           const char *name) {
    const char *dep_name = udynlink_dep_get_name(name);
    if (dep_name == NULL) return 0;

    if (is_loading(mgr, dep_name)) {
        return UDYNLINK_SYM_DEFERRED;
    }

    udynlink_module_t *p_mod = udynlink_dep_find(mgr, dep_name);
    if (p_mod != NULL) {
        return (uintptr_t)p_mod;
    }

    p_mod = udynlink_external_dep_load(dep_name);
    if (p_mod != NULL) {
        return (uintptr_t)p_mod;
    }

    return 0;
}

void udynlink_dep_register(udynlink_dep_mgr_t *mgr,
                           udynlink_module_t *p_mod) {
    register_module(mgr, p_mod);
}

void udynlink_dep_generate_thunks(udynlink_dep_mgr_t *mgr,
                                  udynlink_module_t *p_mod) {
    (void)mgr;
    if (p_mod == NULL || p_mod->p_header == NULL) return;

    /* A module without a gateway slot declares no thunk exports. */
    udynlink_sym_t gw;
    if (udynlink_lookup_symbol(p_mod, "udynlink_thunk_gateway", &gw) == NULL) {
        return;
    }
    if (gw.type != UDYNLINK_SYM_TYPE_EXPORTED ||
        gw.location != UDYNLINK_SYM_LOCATION_DATA) {
        return;
    }

    uint8_t *gateway = (uint8_t *)gw.val;
    udynlink_thunk_write_gateway(gateway, (uint32_t)p_mod->ram_base);

    size_t count = udynlink_get_symbol_count(p_mod);
    for (size_t i = 0; i < count; i++) {
        udynlink_sym_t s;
        if (udynlink_get_symbol(p_mod, i, &s) == NULL) continue;
        if (s.type != UDYNLINK_SYM_TYPE_EXPORTED ||
            s.location != UDYNLINK_SYM_LOCATION_DATA) continue;
        if (s.name == NULL) continue;
        /* memcmp, not strncmp: arm-none-eabi-gcc 15 miscompiles
         * strncmp(s, const, strlen(const)) into a 2-arg strcmp, which fails
         * for names longer than the prefix. */
        if (memcmp(s.name, UDYNLINK_THUNK_EXPORT_PREFIX,
                   UDYNLINK_THUNK_EXPORT_PREFIX_LEN) != 0) continue;

        const char *fn = s.name + UDYNLINK_THUNK_EXPORT_PREFIX_LEN;
        udynlink_sym_t fsym;
        if (udynlink_lookup_symbol(p_mod, fn, &fsym) == NULL) continue;
        if (fsym.location != UDYNLINK_SYM_LOCATION_CODE) continue;

        /* Slots and gateway are co-located in one small .bss section, so
         * the stub->gateway b.n branch is always in range. */
        udynlink_thunk_write_stub((uint8_t *)s.val, (uint32_t)fsym.val,
                                  gateway);
    }
}

uintptr_t udynlink_dep_resolve_func(udynlink_dep_mgr_t *mgr,
                                    udynlink_thunk_pool_t *pool,
                                    const char *name) {
    if (mgr == NULL || pool == NULL || name == NULL) return 0;

    for (size_t i = 0; i < mgr->count; i++) {
        udynlink_module_t *p_mod = mgr->entries[i].p_mod;
        if (p_mod == NULL) continue;

        udynlink_sym_t sym;
        if (udynlink_lookup_symbol(p_mod, name, &sym) == NULL) continue;

        if (sym.location == UDYNLINK_SYM_LOCATION_DATA) continue;

        /* Serve pre-generated in-module thunks first: if this module
         * declared a thunk export for `name`, its stub slot was already
         * filled by udynlink_dep_generate_thunks() at load time.  A zeroed
         * slot means generation skipped it (function not exported), so it
         * falls through to the dynamic pool. */
        char mname[UDYNLINK_THUNK_EXPORT_PREFIX_LEN + UDYNLINK_DEP_MAX_NAME + 1];
        size_t name_len = strlen(name);
        if (name_len > 0 && name_len <= UDYNLINK_DEP_MAX_NAME) {
            memcpy(mname, UDYNLINK_THUNK_EXPORT_PREFIX,
                   UDYNLINK_THUNK_EXPORT_PREFIX_LEN);
            memcpy(mname + UDYNLINK_THUNK_EXPORT_PREFIX_LEN, name,
                   name_len + 1);
            udynlink_sym_t tsym;
            if (udynlink_lookup_symbol(p_mod, mname, &tsym) != NULL &&
                tsym.type == UDYNLINK_SYM_TYPE_EXPORTED &&
                tsym.location == UDYNLINK_SYM_LOCATION_DATA &&
                *(const uint16_t *)tsym.val != 0) {
                return tsym.val | 1u;
            }
        }

        uint32_t ram_base = (uint32_t)p_mod->ram_base;
        uint32_t func_addr = (uint32_t)sym.val;

        udynlink_dep_entry_t *entry = &mgr->entries[i];

        uintptr_t existing = udynlink_external_find_stub(pool, func_addr);
        if (existing != 0) return existing;

        if (entry->gateway == NULL) {
            entry->gateway = udynlink_thunk_alloc_gateway(pool, ram_base);
            if (entry->gateway == NULL) return 0;
        }

        return udynlink_thunk_alloc_stub(pool, func_addr, entry->gateway);
    }

    return 0;
}

uintptr_t udynlink_dep_resolve_data(udynlink_dep_mgr_t *mgr,
                                    const char *name) {
    if (mgr == NULL || name == NULL) return 0;

    for (size_t i = 0; i < mgr->count; i++) {
        udynlink_module_t *p_mod = mgr->entries[i].p_mod;
        if (p_mod == NULL) continue;

        udynlink_sym_t sym;
        if (udynlink_lookup_symbol(p_mod, name, &sym) == NULL) continue;

        if (sym.location == UDYNLINK_SYM_LOCATION_DATA) {
            return sym.val;
        }
    }

    return 0;
}

/* ─── Module load / unload with dependency tracking ────────────────── */

udynlink_error_t udynlink_dep_load(udynlink_dep_mgr_t *mgr,
                                   udynlink_module_t *p_mod,
                                   const void *base_addr,
                                   void *load_addr, size_t load_size,
                                   udynlink_load_mode_t load_mode,
                                   udynlink_thunk_pool_t *pool) {
    if (mgr == NULL || p_mod == NULL || base_addr == NULL) {
        return UDYNLINK_ERR_INVALID_MODULE;
    }

    const char *mod_name = udynlink_get_module_name_from_image(base_addr);
    if (mod_name != NULL) {
        if (is_loading(mgr, mod_name)) {
            return UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL;
        }
        push_loading(mgr, mod_name);
    }

    udynlink_error_t err = udynlink_load_module(p_mod, base_addr,
        load_addr, load_size, load_mode);
    if (err != UDYNLINK_OK) {
        if (mod_name != NULL) pop_loading(mgr);
        return err;
    }

    register_module(mgr, p_mod);

    /* Eagerly generate in-module thunks for the module's declared thunk
     * exports so importers can resolve them without the dynamic pool.
     * No-op for modules without a udynlink_thunk_gateway slot. */
    udynlink_dep_generate_thunks(mgr, p_mod);

    if (mod_name != NULL) pop_loading(mgr);

    (void)pool;
    return UDYNLINK_OK;
}

udynlink_error_t udynlink_dep_unload(udynlink_dep_mgr_t *mgr,
                                     udynlink_module_t *p_mod) {
    if (mgr == NULL || p_mod == NULL) {
        return UDYNLINK_ERR_INVALID_MODULE;
    }

    unregister_module(mgr, p_mod);
    return udynlink_unload_module(p_mod);
}
