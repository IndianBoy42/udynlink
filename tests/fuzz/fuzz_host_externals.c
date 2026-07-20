/* Host-side externals for the udynlink loader fuzz/sanitizer harnesses.
 *
 * The loader links against five host callbacks (udynlink_externals.h). Three of
 * them already have __attribute__((weak)) defaults in udynlink.c; the strong
 * definitions here override the weak ones at link time so the harnesses link
 * without forcing every host to provide their own.
 *
 * udynlink_external_malloc/free delegate to the real libc allocator so that
 * ASan tracks the loader's RAM allocations with redzones — exactly the
 * mechanism that lets the fuzzer catch header-derived lengths walking past
 * the image buffer.
 *
 * udynlink_external_resolve_symbol returns a non-zero sentinel so EXTERN
 * relocations take their write branch (udynlink.c:355-356) and
 * extern-bearing modules load successfully. Module code is ARM Thumb and is
 * never executed on the host, so the returned address need not be valid.
 */
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>

#include "udynlink.h"

void *udynlink_external_malloc(size_t size) { return malloc(size); }

void udynlink_external_free(void *p) { free(p); }

void udynlink_external_vprintf(const char *s, va_list va) {
    (void)s;
    (void)va;
}

int udynlink_external_is_pointer_in_ram(const void *p) {
    (void)p;
    return 0;
}

uintptr_t udynlink_external_resolve_symbol(const udynlink_module_t *p_mod,
                                           const char *name) {
    (void)p_mod;
    (void)name;
    return (uintptr_t)0x1000;
}
