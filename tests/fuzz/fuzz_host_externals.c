/* Host-side externals for the udynlink loader fuzz/sanitizer harnesses.
 *
 * The loader links against five host callbacks (udynlink_externals.h). Three of
 * them already have __attribute__((weak)) defaults in udynlink.c; the strong
 * definitions here override the weak ones at link time so the harnesses link
 * without forcing every host to provide their own.
 *
 * udynlink_external_malloc/free delegate to the real libc allocator for the
 * main block (section = NULL) so that ASan tracks the loader's RAM
 * allocations with redzones — exactly the mechanism that lets the fuzzer
 * catch header-derived lengths walking past the image buffer. Non-NULL
 * sections (sectioned images) are served from a small owned block instead:
 * returning NULL there would still cover the failure path, but serving real
 * memory keeps the full load path — placement, payload copy, per-section
 * free — under sanitizers.
 */
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>

#include "udynlink.h"

/* One reusable block backs every tagged-section request. Loads are strictly
 * sequential here, so a single scratch arena can't starve; if an image ever
 * declares more tagged bytes than fit, the load fails the way an exhausted
 * region would — also a useful fuzz outcome. */
static uint8_t g_section_heap[1u << 20];
static size_t  g_section_heap_used;

void *udynlink_external_malloc(size_t size, const char *section,
                               size_t align, uint32_t flags) {
    (void)flags;
    if (section != NULL) {
        if (align < 4u)
            align = 4u;
        uintptr_t base = (uintptr_t)g_section_heap;
        uintptr_t p = (base + g_section_heap_used + (align - 1u))
                      & ~(uintptr_t)(align - 1u);
        size_t off = (size_t)(p - base);
        if (size > sizeof(g_section_heap) - off)
            return NULL;
        g_section_heap_used = off + size;
        return (void *)p;
    }
    (void)align;
    return malloc(size);
}

void udynlink_external_free(void *p, const char *section,
                            size_t align, uint32_t flags) {
    (void)align;
    (void)flags;
    if (section != NULL) {
        /* Bump arena: per-section blocks are reclaimed by rewinding at the
         * end of each exercise() run, not individually. */
        return;
    }
    free(p);
}

/* Resets the tagged-section arena. udynlink_fuzz_exercise calls this at
 * entry so one input's allocations can never starve the next input. */
void udynlink_test_reset_section_heap(void) {
    g_section_heap_used = 0;
}

void udynlink_external_vprintf(const char *s, va_list va) {
    (void)s;
    (void)va;
}
int udynlink_external_is_pointer_in_ram(const void *p) {
    (void)p;
    return 0;
}


/* Non-zero sentinel: EXTERN relocations take their write branch and
 * extern-bearing modules load successfully. Module code is ARM Thumb and is
 * never executed on the host, so the address need not be valid. */
uintptr_t udynlink_external_resolve_symbol(const udynlink_module_t *p_mod,
                                           const char *name) {
    (void)p_mod;
    (void)name;
    return (uintptr_t)0x1000;
}
