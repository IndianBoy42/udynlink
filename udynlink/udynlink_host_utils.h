#ifndef __UDYNLINK_HOST_UTILS_H__
#define __UDYNLINK_HOST_UTILS_H__

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef UDYNLINK_HOST_SYM_CACHE_SIZE
#define UDYNLINK_HOST_SYM_CACHE_SIZE 16
#endif

typedef struct {
    const char *name;
    uintptr_t addr;
} udynlink_host_sym_cache_entry_t;

static inline uintptr_t udynlink_host_sym_cache_lookup(
    udynlink_host_sym_cache_entry_t *cache,
    size_t cache_size,
    const char *name,
    uintptr_t (*fallback)(const char *name))
{
    uint32_t h = 0;
    for (const char *p = name; *p; p++)
        h = h * 31 + (uint8_t)*p;
    size_t idx = h % cache_size;
    if (cache[idx].name && strcmp(cache[idx].name, name) == 0)
        return cache[idx].addr;
    uintptr_t addr = fallback(name);
    if (addr) {
        cache[idx].name = name;
        cache[idx].addr = addr;
    }
    return addr;
}

static inline void udynlink_host_sym_cache_invalidate(
    udynlink_host_sym_cache_entry_t *cache,
    size_t cache_size)
{
    memset(cache, 0, cache_size * sizeof(*cache));
}

#endif /* __UDYNLINK_HOST_UTILS_H__ */
