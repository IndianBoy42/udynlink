#ifndef __UDYNLINK_HASH_H__
#define __UDYNLINK_HASH_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t nbuckets;
    uint32_t symoffset;
    uint32_t bloom_size;
    uint32_t bloom_shift;
    const uint32_t *bloom;
    const uint32_t *buckets;
    const uint32_t *hash_values;
    const uint32_t *sym_addrs;
    const char *strtab;
    const uint32_t *strtab_offsets;
} udynlink_hash_table_t;

void *udynlink_resolve_hashed_symbol(const udynlink_hash_table_t *table, const char *name);

#ifdef __cplusplus
}
#endif

#endif
