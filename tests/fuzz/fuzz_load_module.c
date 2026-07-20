/* libFuzzer harness for the udynlink loader (udynlink/udynlink.c).
 *
 * Build with -fsanitize=fuzzer,address,undefined (clang). Run:
 *   ./udynlink_fuzz_load tests/fuzz/corpus -max_total_time=60 -print_final_stats=1
 *
 * Reproduce a crash file with: ./udynlink_fuzz_load <crash-file>
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fuzz_harness.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* The loader's documented contract is that every module image begins with
     * a full udynlink_module_header_t (32 bytes). Inputs shorter than that are
     * caller-contract violations for udynlink_load_module; the harness filters
     * them so the fuzzer spends its budget on the real fuzz surface —
     * header-derived lengths (num_rels, symt_size, code_size, data_size)
     * walking past the image buffer — rather than tripping on every short
     * input via validate_header's reads. */
    if (size < sizeof(udynlink_module_header_t)) {
        return 0;
    }

    /* Copy data into a freshly malloc'd buffer of exactly `size` bytes:
     * libFuzzer's `data` pointer may sit inside a larger arena with no ASan
     * redzone at `data + size`. A dedicated malloc(size) places a redzone
     * exactly at the buffer end, so any header-derived length that walks past
     * `size` is caught as a heap-buffer-overflow. */
    uint8_t *buf = (uint8_t *)malloc(size);
    if (buf == NULL) {
        return 0;
    }
    memcpy(buf, data, size);

    (void)udynlink_fuzz_exercise(buf, size);

    free(buf);
    return 0;
}
