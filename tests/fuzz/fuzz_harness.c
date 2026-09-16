/* Shared loader exercise routine — see fuzz_harness.h.
 *
 * Iterates all three load modes on the given image and additionally drives the
 * non-contiguous-image entry point (udynlink_load_module_image with an
 * udynlink_module_image_t built via udynlink_image_from_memory). Treats every
 * udynlink_error_t return as success; the only failures this code cares
 * about are an ASan/UBSan report, a signal, or the deterministic
 * misaligned-RAM oracle below aborting (a loader bug, never an input
 * property).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "udynlink.h"
#include "fuzz_harness.h"

/* When udynlink_relocate_module is given a NULL destination it asks our
 * udynlink_external_malloc to allocate the move target; this exercises the
 * rebase_module_pointers path with a foreign NULL dest without actually
 * moving anything. The result code is irrelevant — a clean rejection is as
 * good as a successful relocate from the harness's "did it crash?" view.
 *
 * Also walks the whole symbol table via the index-based enumeration API so the
 * sanitized bounds checks in udynlink_get_symbol() / get_sym_at_raw run on
 * every (possibly malformed) loadable image. */
static void exercise_loaded(udynlink_module_t *p_mod) {
    udynlink_sym_t sym;
    udynlink_sym_t *psym;

    (void)udynlink_get_module_name(p_mod);
    psym = udynlink_lookup_symbol(p_mod, "test", &sym);
    (void)psym;
    (void)udynlink_get_symbol_value(p_mod, "test");

    size_t nsym = udynlink_get_symbol_count(p_mod);
    for (size_t i = 0; i < nsym; i++) {
        (void)udynlink_get_symbol(p_mod, i, &sym);
    }

    (void)udynlink_relocate_module(p_mod, NULL, 0);
}
/* Sectioned images (hdr flag SECTIONS) allocate their tagged sections from
 * the fuzz allocator's bump arena; rewinding at entry (declaration in
 * fuzz_harness.h) keeps one input's allocations from starving every later
 * input. Untagged images never touch it. */

int udynlink_fuzz_exercise(const uint8_t *buf, size_t size) {
    udynlink_test_reset_section_heap();
    udynlink_module_t mod;
    /* The loader's documented contract is that the caller supplies at least
     * `udynlink_get_image_size_bounded(buf, size)` bytes — the header plus the
     * relocation table, symbol table, section table, code and data whose sizes
     * the image declares. The loader has no API to receive the source buffer
     * length, so it cannot self-defend against a header (or section table)
     * that claims more payload than the buffer actually holds; COPY_ALL's
     * `memcpy(p_temp8 + code_offset, image->p_code, p_header->code_size)` and
     * the per-tagged-section copies would then read past the buffer.
     *
     * Inputs that violate this contract (e.g. a mutated stub claiming
     * code_size=128 with only 8 actual code bytes, or a sectioned image whose
     * tagged section declares 256 KiB of payload in a 508-byte file) are
     * filtered here, at the caller boundary, before any loader entry point —
     * not in the loader. The bounded variant is what makes the sectioned case
     * decidable: the plain `udynlink_get_image_size()` sees only the header
     * and the main payloads, so it cannot bound tagged payloads at all. */
    if (size < sizeof(udynlink_module_header_t)) {
        return 0;
    }
    size_t image_size = udynlink_get_image_size_bounded(buf, size);
    if (image_size == 0 || image_size > size) {
        return 0;
    }
    udynlink_module_image_t image;
    udynlink_error_t err;
    udynlink_load_mode_t mode;

    for (mode = UDYNLINK_LOAD_MODE_COPY_ALL;
         mode <= UDYNLINK_LOAD_MODE_XIP;
         mode = (udynlink_load_mode_t)(mode + 1)) {

        (void)udynlink_validate_header(
            (const udynlink_module_header_t *)buf);
        (void)udynlink_get_image_metadata_size(
            (const udynlink_module_header_t *)buf);

        memset(&mod, 0, sizeof(mod));
        err = udynlink_load_module(&mod, buf, NULL, 0, mode);
        if (err == UDYNLINK_OK) {
            exercise_loaded(&mod);
            udynlink_unload_module(&mod);
        } else if (mod.p_header != NULL) {
            /* Defensive: the loader mark_module_free's on error, but if a
             * future change ever leaves a partial allocation we still free it
             * rather than leak under ASan. */
            udynlink_unload_module(&mod);
        }
    }

    /* Non-contiguous-image entry point: rebuild the descriptor from the same
     * buffer and load it again, in COPY_ALL only (the other modes share the
     * same load path body and would merely triple the runtime). */
    udynlink_image_from_memory(buf, &image);
    /* Pre-load enumeration: walk the image's symbol table before any RAM is
     * allocated, exercising udynlink_image_get_symbol_count /
     * udynlink_image_get_symbol and their symt_size bounds on the (possibly
     * malformed) buffer. */
    {
        udynlink_sym_t sym;
        size_t nsym = udynlink_image_get_symbol_count(&image);
        for (size_t i = 0; i < nsym; i++) {
            (void)udynlink_image_get_symbol(&image, i, &sym);
        }
    }
    memset(&mod, 0, sizeof(mod));
    err = udynlink_load_module_image(&mod, &image, NULL, 0,
                                      UDYNLINK_LOAD_MODE_COPY_ALL);
    /* Capture RAM ownership before the unload below zeroes the handle. */
    int baseline_had_ram = (err == UDYNLINK_OK && mod.p_ram != NULL);
    if (err == UDYNLINK_OK) {
        exercise_loaded(&mod);
        udynlink_unload_module(&mod);
    } else if (mod.p_header != NULL) {
        udynlink_unload_module(&mod);
    }

    /* Alignment-contract oracle. When the auto-alloc load above succeeded
     * with a real RAM block, the same image loaded into a deliberately
     * misaligned caller-supplied buffer of exactly the required size has no
     * remaining failure path but the alignment check, so it must be rejected
     * with UDYNLINK_ERR_LOAD_RAM_UNALIGNED — deterministically, for any
     * input that reaches this point. A different outcome is a loader bug,
     * not a property of the fuzz input. */
    if (baseline_had_ram) {
        size_t ram_size = udynlink_compute_ram_size(image.p_header,
                                                    UDYNLINK_LOAD_MODE_COPY_ALL);
        uint8_t *p_misaligned = (uint8_t *)malloc(ram_size + 1);
        if (p_misaligned != NULL) { /* allocation failure is a harness limit, not a bug */
            udynlink_module_t mod_misaligned;
            memset(&mod_misaligned, 0, sizeof(mod_misaligned));
            udynlink_error_t err_misaligned = udynlink_load_module_image(
                &mod_misaligned, &image, p_misaligned + 1, ram_size,
                UDYNLINK_LOAD_MODE_COPY_ALL);
            if (err_misaligned != UDYNLINK_ERR_LOAD_RAM_UNALIGNED) {
                /* Not assert(): the san gate compiles with -DNDEBUG, which
                 * would silently drop the check. */
                fprintf(stderr, "udynlink_fuzz_exercise: misaligned load_addr "
                                "not rejected with UDYNLINK_ERR_LOAD_RAM_UNALIGNED "
                                "(got %d)\n", (int)err_misaligned);
                abort();
            }
            free(p_misaligned);
        }
    }

    return 0;
}
