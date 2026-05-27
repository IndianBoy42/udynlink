#ifndef __UDYNLINK_HASH_H__
#define __UDYNLINK_HASH_H__

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t nbuckets;
    size_t symoffset;
    size_t bloom_size;
    size_t bloom_shift;
    const uint32_t *bloom;
    const uint32_t *buckets;
    const uint32_t *hash_values;
    const uintptr_t *sym_addrs;
    const char *strtab;
    const size_t *strtab_offsets;
} udynlink_hash_table_t;

static uint32_t gnu_hash(const char *name) {
    uint32_t h = 5381;
    unsigned char c;
    while ((c = (unsigned char)*name++) != '\0')
        h = h * 33 + c;
    return h;
} 

void *udynlink_resolve_hashed_symbol(const udynlink_hash_table_t *table, const char *name) {
    uint32_t h = gnu_hash(name);

    uint32_t mask = (1u << (h % 32)) | (1u << ((h >> table->bloom_shift) % 32));
    size_t bloom_idx = (h / 32) & (table->bloom_size - 1);
    if ((table->bloom[bloom_idx] & mask) != mask)
        return NULL;

    size_t idx = table->buckets[h % table->nbuckets];
    if (idx < table->symoffset)
        return NULL;

    for (;;) {
        uint32_t hv = table->hash_values[idx - table->symoffset];
        if ((hv | 1u) == (h | 1u)) {
            const char *sym_name = table->strtab + table->strtab_offsets[idx];
            if (strcmp(sym_name, name) == 0)
                return (void *)table->sym_addrs[idx];
        }
        if (hv & 1u)
            break;
        idx++;
    }

    return NULL;
}

#ifdef __cplusplus
}
#endif

#endif
