/* Shared loader exercise routine used by both the libFuzzer harness
 * (fuzz_load_module.c) and the sanitizer regression gate (san_load_module.c).
 *
 * Drives every public loader entry point on a single module image across all
 * three load modes, plus the non-contiguous-image entry point. The goal is NOT
 * to assert UDYNLINK_OK — many fuzz inputs are legitimately malformed and must
 * be rejected gracefully. The only observable failure the surrounding harness
 * cares about is an ASan/UBSan report or a signal.
 *
 * Contract: `buf` must point to at least `size` bytes and `size` must be >=
 * sizeof(udynlink_module_header_t) (the documented minimum module image
 * length). Inputs shorter than a full header are caller-contract violations
 * for the loader's pre-load entry points, so the harnesses filter them before
 * calling this routine — see the size guard in each harness entry function.
 */
#ifndef UDYNLINK_FUZZ_HARNESS_H
#define UDYNLINK_FUZZ_HARNESS_H

#include <stddef.h>
#include <stdint.h>

#include "udynlink.h"

/* Rewinds the tagged-section arena in the host externals (fuzz_host_externals.c)
 * so one input's sectioned allocations can't starve the next input. */
void udynlink_test_reset_section_heap(void);

/* Run the full load/lookup/relocate/unload sequence on a module image.
 * Returns 0. The only deliberate failure is the deterministic
 * alignment-contract oracle (see fuzz_harness.c), which aborts; any other
 * crash is a real loader bug. */
int udynlink_fuzz_exercise(const uint8_t *buf, size_t size);

#endif /* UDYNLINK_FUZZ_HARNESS_H */
