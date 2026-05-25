#include "udynlink_hash.h"
#include <string.h>

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
    uint32_t bloom_idx = (h / 32) & (table->bloom_size - 1);
    if ((table->bloom[bloom_idx] & mask) != mask)
        return NULL;

    uint32_t idx = table->buckets[h % table->nbuckets];
    if (idx < table->symoffset)
        return NULL;

    for (;;) {
        uint32_t hv = table->hash_values[idx - table->symoffset];
        if ((hv | 1u) == (h | 1u)) {
            const char *sym_name = table->strtab + table->strtab_offsets[idx];
            if (strcmp(sym_name, name) == 0)
                return (void *)(uintptr_t)table->sym_addrs[idx];
        }
        if (hv & 1u)
            break;
        idx++;
    }

    return NULL;
}
