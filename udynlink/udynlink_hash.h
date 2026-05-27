#ifndef __UDYNLINK_HASH_H__
#define __UDYNLINK_HASH_H__

#include <stdint.h>
#include <stddef.h>

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

void *udynlink_resolve_hashed_symbol(const udynlink_hash_table_t *table, const char *name);

#ifdef __cplusplus
}
#endif

#endif
