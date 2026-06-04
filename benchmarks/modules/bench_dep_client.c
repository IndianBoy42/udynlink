#include <stdint.h>
#include <stddef.h>

extern int32_t dep_hash_lookup(const uint8_t *key, int key_len);
extern int32_t dep_hash_insert(const uint8_t *key, int key_len, int32_t value);
extern void     dep_hash_reset(void);

int32_t bench_dep_client(void) {
    dep_hash_reset();
    dep_hash_insert((const uint8_t *)"abc", 3, 1);
    dep_hash_insert((const uint8_t *)"def", 3, 2);
    dep_hash_insert((const uint8_t *)"ghi", 3, 3);
    return dep_hash_lookup((const uint8_t *)"abc", 3);
}
