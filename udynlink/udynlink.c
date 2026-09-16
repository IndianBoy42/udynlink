#include "udynlink.h"
#include "udynlink_externals.h"
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

__attribute__((weak))
uintptr_t udynlink_external_resolve_symbol(const udynlink_module_t *p_mod, const char *name) {
    (void)p_mod;
    (void)name;
    return 0;
}

__attribute__((weak))
int udynlink_external_is_pointer_in_ram(const void *p) {
    (void)p;
    return 0;
}

__attribute__((weak))
void udynlink_external_vprintf(const char *s, va_list va) {
    (void)s;
    (void)va;
}

udynlink_error_t udynlink_check_arch_tag(uint16_t mod_arch, uint16_t host_arch) {
    if ((mod_arch & UDYNLINK_ARCH_FAMILY_MASK) != (host_arch & UDYNLINK_ARCH_FAMILY_MASK)) {
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, "Module architecture family mismatch (mod=0x%04X, host=0x%04X)\n", mod_arch, host_arch);
        return UDYNLINK_ERR_LOAD_ARCH_MISMATCH;
    }
    uint16_t mod_float = (mod_arch >> UDYNLINK_ARCH_FLOAT_ABI_SHIFT) & 0x03;
    uint16_t host_float = (host_arch >> UDYNLINK_ARCH_FLOAT_ABI_SHIFT) & 0x03;
    if (mod_float == UDYNLINK_ARCH_FLOAT_ABI_HARD && host_float != UDYNLINK_ARCH_FLOAT_ABI_HARD) {
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, "Module requires hard-float, host has soft-float\n");
        return UDYNLINK_ERR_LOAD_ARCH_MISMATCH;
    }
    if (mod_float == UDYNLINK_ARCH_FLOAT_ABI_SOFTFP && host_float == UDYNLINK_ARCH_FLOAT_ABI_SOFT) {
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, "Module requires softfp, host has soft-float\n");
        return UDYNLINK_ERR_LOAD_ARCH_MISMATCH;
    }
    return UDYNLINK_OK;
}

////////////////////////////////////////////////////////////////////////////////
// Local macros and data

#define UDYNLINK_MODULE_SIGN                  (((uint32_t)'M' << 24) | ((uint32_t)'L' << 16) | ((uint32_t)'D' << 8) | (uint32_t)'U')

/* Sanity cap on header-derived RAM requests. Real modules are kilobyte-scale;
 * a 16 MiB cap leaves >1000x headroom and rejects malformed/malicious images
 * that would otherwise drive udynlink_external_malloc into a multi-GB
 * allocation (or DoS the host). Header fields feeding ram_size are
 * attacker-controlled in the fuzz/malicious-module threat model. */
#define UDYNLINK_MAX_RAM_SIZE                (16u * 1024u * 1024u)

/* Sanity cap on the header-derived *image* size (header + relocs + symtab +
 * section table + payloads), computed the same way udynlink_get_image_size
 * does.  Real modules are kilobyte-scale; a 4 MiB cap leaves >1000x headroom
 * and rejects malformed/malicious images whose header fields (num_rels,
 * symt_size, code_size, data_size) are inflated to drive a multi-MiB memcpy.
 * Like the RAM cap above, these fields are attacker-controlled in the
 * fuzz/malicious-module threat model. */
#define UDYNLINK_MAX_IMAGE_SIZE              (4u * 1024u * 1024u)

/* Sanity cap on the section-derived allocation total (main block + every
 * tagged section, since each becomes a host allocation) and on the
 * section-table entry count (contract §1.2). The count lives in the
 * header's flag bits 7:1, so the field itself can only express 0..127 and
 * the loader accepts 1..63; anything else (or a count without the
 * section-table flag) is a malformed table. */
#define UDYNLINK_MAX_SECTIONS                (63u)

/* Whether a header carries a section table (and thus the sectioned load
 * path).  Untagged headers take today's exact loader path. */
static int is_sectioned(const udynlink_module_header_t *p_header) {
    return (p_header->flags & UDYNLINK_HDR_FLAG_SECTIONS) != 0;
}

#define _UDYNLINK_EXPAND(x)                   #x"\n"
static const char * const error_codes[] = {
    UDYNLINK_ERROR_CODES
};
#undef _UDYNLINK_EXPAND

// Symbol table masks and data
#define UDYNLINK_SYM_OFFSET_MASK              0x07FFFFFF
#define UDYNLINK_SYM_INFO_SHIFT               27
#define UDYNLINK_SYM_INFO_CODE_MASK           0x08
#define UDYNLINK_SYM_INFO_TYPE_MASK           0x07
#define UDYNLINK_SYM_NAME_OFFSET              0

// Module structure masks
#define UDYNLINK_LOAD_MODE_MASK               (uint8_t)0x03
#define UDYNLINK_LOAD_FOREIGN_RAM_MASK        (uint8_t)0x04
#define UDYNLINK_LOAD_GET_MODE(p_mod)         (udynlink_load_mode_t)(p_mod->info & UDYNLINK_LOAD_MODE_MASK)
#define UDYNLINK_LOAD_SET_MODE(p_mod, m)      p_mod->info = (p_mod->info & (uint8_t)~UDYNLINK_LOAD_MODE_MASK) | ((uint8_t)m)
#define UDYNLINK_LOAD_IS_FOREIGN_RAM(p_mod)   ((p_mod->info & UDYNLINK_LOAD_FOREIGN_RAM_MASK) != 0)
#define UDYNLINK_LOAD_SET_FOREIGN_RAM(p_mod)  p_mod->info |= UDYNLINK_LOAD_FOREIGN_RAM_MASK
#define UDYNLINK_LOAD_CLR_FOREIGN_RAM(p_mod)  p_mod->info &= (uint8_t)~UDYNLINK_LOAD_FOREIGN_RAM_MASK

////////////////////////////////////////////////////////////////////////////////
// Helpers - debug

// Helper for the 'udynlink_debug' method
// Needed just because we don't want to have two dependencies (udynlink_external printf and
// udynlink_external_vprintf) instead of a single one (udynlink_external_vprintf).
static void internal_printf(const char *msg, ...) {
    va_list va;

    va_start(va, msg);
    udynlink_external_vprintf(msg, va);
    va_end(va);
}

// The debug output function
static void udynlink_debug(const char *func, int line, udynlink_debug_level_t level, const char *msg, ...) {
    va_list va;
    static const char * const names[] = {"n/a", "error", "warning", "info"};

    if ((int)level > UDYNLINK_DEBUG_LEVEL) {
        return;
    }
    internal_printf("[udynlink %s in function %s, line %d] ", names[(int)level], func, line);
    va_start(va, msg);
    udynlink_external_vprintf(msg, va);
    va_end(va);
}

////////////////////////////////////////////////////////////////////////////////
// Helpers - offsets and addresses

// Returns the size of the module header in bytes.
// v1.0 and v3.0+ headers are 32 bytes; v2.0 headers are 36 bytes.
static size_t get_header_size(const udynlink_module_header_t *p_header) {
    if (p_header->udynlink_version >= UDYNLINK_MAKE_VERSION(3, 0))
        return sizeof(udynlink_module_header_t);
    if (p_header->udynlink_version >= UDYNLINK_MAKE_VERSION(2, 0))
        return 36;
    return 32;
}

/* End of the header-derived metadata: header + relocations + symbol table,
 * aligned to 4. Dereference-free: this is the offset every header-coverage
 * contract (udynlink_get_image_size callers, the fuzz harness gate) is
 * computed against. */
static size_t get_symtab_end_offset(const udynlink_module_header_t *p_header) {
    size_t res = get_header_size(p_header) + p_header->num_rels * 2 * sizeof(uint32_t) + p_header->symt_size;
    /* Align to 4 with a size_t mask: ~3U is 32-bit and would zero the high
     * half of `res` on 64-bit hosts, silently truncating an oversized (e.g.
     * 4 GiB symt_size) header to a small 'valid' offset and bypassing the
     * UDYNLINK_MAX_IMAGE_SIZE cap downstream. */
    res = (res + 3) & ~(size_t)3;
    return res;
}

/* Raw section count from the header's flag bits (7:1).  Validity (1..63,
 * and only alongside the section-table flag) is enforced by get_sectab();
 * planning arithmetic clamps invalid values to 0 so every metadata offset
 * stays header-derived and dereference-free. */
static uint32_t get_num_sections_raw(const udynlink_module_header_t *p_header) {
    uint32_t num = UDYNLINK_HDR_NUM_SECTIONS(p_header->flags);
    if (!is_sectioned(p_header) || num == 0 || num > UDYNLINK_MAX_SECTIONS) {
        return 0;
    }
    return num;
}

/* Byte offset of the code payload in the image.  For sectioned images the
 * section table (header-bounded: the count lives in the header flags) sits
 * between the symbol table and the code.  Dereference-free: planning APIs
 * can call this on any header without reading beyond it. */
static size_t get_code_offset_from_header(const udynlink_module_header_t *p_header) {
    size_t res = get_symtab_end_offset(p_header);
    uint32_t num_sections = get_num_sections_raw(p_header);
    res += num_sections * 6 * sizeof(uint32_t);
    return (res + 3) & ~(size_t)3;
}


// Gets the pointer to the symbol table according to the given module header
static const uint32_t *get_sym_table_pointer(const udynlink_module_header_t *p_header) {
    return (uint32_t*)p_header + get_header_size(p_header) / sizeof(uint32_t) + p_header->num_rels * 2;
}


////////////////////////////////////////////////////////////////////////////////
// Helpers - section table (loader ABI 3.1)

/* Byte offset of the section table inside the image metadata. */
static size_t get_sectab_offset(const udynlink_module_header_t *p_header) {
    return get_symtab_end_offset(p_header);
}

/* One section-table entry, decoded from the (attacker-controlled) image. */
typedef struct {
    uint32_t name_off;  /* offset into the symbol-table string pool; 0 = unnamed */
    uint32_t va;        /* link-time VA of the section start */
    uint32_t size;      /* bytes, multiple of 4 */
    uint32_t align;     /* bytes, power of two, >= 4 */
    uint32_t cls;       /* UDYNLINK_SEC_CLASS_* */
    uint32_t flags;     /* hint flags; bit 31 = MAIN */
} sect_entry_t;

/* Bounds-checked view of a header's section table.  num == 0 for an
 * untagged (legacy) header, whose three implicit main sections are
 * synthesized by the reporting APIs only — resolution for untagged modules
 * keeps today's code/data-base arithmetic verbatim. */
typedef struct {
    const uint8_t *p_entries;  /* first 24-byte entry */
    const char *p_pool;        /* symbol-table string pool base (name offsets) */
    size_t num;                /* validated entry count */
} sect_view_t;

/* Decode entry `idx` of a validated view. */
static void sect_entry_at(const sect_view_t *tab, size_t idx, sect_entry_t *e) {
    const uint32_t *w = (const uint32_t *)(tab->p_entries + idx * 6 * sizeof(uint32_t));
    e->name_off = w[0];
    e->va = w[1];
    e->size = w[2];
    e->align = w[3];
    e->cls = w[4];
    e->flags = w[5];
}

/* Section name (points into the image's string pool), or NULL when unnamed. */
static const char *sect_name(const sect_view_t *tab, const sect_entry_t *e) {
    return e->name_off ? tab->p_pool + e->name_off : NULL;
}

/* Host-visible allocator flags: MAIN is loader-internal and bits 23:16 are
 * reserved in v1 — neither is ever passed to the host. */
static uint32_t sect_host_flags(const sect_entry_t *e) {
    return e->flags & ~(uint32_t)(UDYNLINK_SEC_FLAG_MAIN | 0x00FF0000u);
}

/* Number of bytes the non-main section base array occupies after the LOT. */
static size_t get_nonmain_bases_size(const sect_view_t *tab) {
    return tab->num > 3 ? (tab->num - 3) * sizeof(uintptr_t) : 0;
}

/* Parse and validate the section table of a (possibly malformed) header.
 * The section count lives in the header's flag bits, so every metadata
 * offset is header-derived; the entry fields are attacker-controlled in
 * the fuzz threat model and get the same discipline as the header fields:
 * per-field sanity checks and structural invariants (sorted by va,
 * non-overlapping, main sections at indices 0/1/2 matching the header's
 * code/data/bss sizes).  When `check_names` is set, section names are
 * verified to be NUL-terminated inside the symbol table string pool
 * (needed before any name is handed to the allocator); lookup paths skip
 * it to stay O(entries).  Untagged headers yield an empty view.
 *
 * Returns UDYNLINK_OK, or UDYNLINK_ERR_LOAD_BAD_SECTION_TABLE for any
 * malformed or inconsistent table (including an invalid flag/count
 * combination and the flag set without the ABI version that defines it). */
static udynlink_error_t get_sectab(const udynlink_module_header_t *p_header, sect_view_t *tab, int check_names) {
    tab->p_entries = NULL;
    tab->p_pool = (const char *)get_sym_table_pointer(p_header);
    tab->num = 0;
    if ((p_header->flags & UDYNLINK_HDR_FLAG_SECTIONS) == 0) {
        /* Untagged: the count bits must be zero (they were a reserved
         * field before loader ABI 3.1). */
        if ((p_header->flags & UDYNLINK_HDR_SECTIONS_MASK) != 0) {
            return UDYNLINK_ERR_LOAD_BAD_SECTION_TABLE;
        }
        return UDYNLINK_OK;
    }
    if ((p_header->flags & 0xFF00u) != 0) {
        return UDYNLINK_ERR_LOAD_BAD_SECTION_TABLE;
    }
    if (p_header->udynlink_version < UDYNLINK_MAKE_VERSION(3, 1)) {
        return UDYNLINK_ERR_LOAD_BAD_SECTION_TABLE;
    }
    uint32_t num = get_num_sections_raw(p_header);
    if (num == 0) {
        /* Zero count with the flag set, or a count above the 63 the field
         * can validly express. */
        return UDYNLINK_ERR_LOAD_BAD_SECTION_TABLE;
    }
    /* Bounds the memchr scans below for hostile symt_size values. */
    if (p_header->symt_size > (uint32_t)UDYNLINK_MAX_IMAGE_SIZE) {
        return UDYNLINK_ERR_LOAD_BAD_SECTION_TABLE;
    }
    const uint8_t *entries = (const uint8_t *)p_header + get_sectab_offset(p_header);
    sect_entry_t prev = {0, 0, 0, 0, 0, 0};
    for (uint32_t i = 0; i < num; i++) {
        sect_entry_t e;
        const uint32_t *w = (const uint32_t *)(entries + (size_t)i * 6 * sizeof(uint32_t));
        e.name_off = w[0]; e.va = w[1]; e.size = w[2]; e.align = w[3]; e.cls = w[4]; e.flags = w[5];
        if (e.align < 4 || (e.align & (e.align - 1)) != 0) {
            return UDYNLINK_ERR_LOAD_BAD_SECTION_TABLE;
        }
        if ((e.size & 3u) != 0) {
            return UDYNLINK_ERR_LOAD_BAD_SECTION_TABLE;
        }
        if (e.cls > UDYNLINK_SEC_CLASS_BSS) {
            return UDYNLINK_ERR_LOAD_BAD_SECTION_TABLE;
        }
        if ((e.flags & 0x00FF0000u) != 0) {
            return UDYNLINK_ERR_LOAD_BAD_SECTION_TABLE;
        }
        if (i < 3) {
            /* The three main sections are always the lowest VAs and always
             * MAIN: .text/.data/.bss in class order, matching the header. */
            if ((e.flags & UDYNLINK_SEC_FLAG_MAIN) == 0 || e.cls != i) {
                return UDYNLINK_ERR_LOAD_BAD_SECTION_TABLE;
            }
        } else {
            if ((e.flags & UDYNLINK_SEC_FLAG_MAIN) != 0) {
                return UDYNLINK_ERR_LOAD_BAD_SECTION_TABLE;
            }
            if (e.name_off == 0) {
                /* A tagged section without a name cannot be routed to the
                 * allocator: section == NULL means the main block. */
                return UDYNLINK_ERR_LOAD_BAD_SECTION_TABLE;
            }
        }
        if (check_names && e.name_off != 0) {
            if (e.name_off >= p_header->symt_size ||
                memchr(tab->p_pool + e.name_off, 0, p_header->symt_size - e.name_off) == NULL) {
                return UDYNLINK_ERR_LOAD_BAD_SECTION_TABLE;
            }
        }
        if (i == 0) {
            if (e.va != 0) {
                return UDYNLINK_ERR_LOAD_BAD_SECTION_TABLE;
            }
        } else if ((uint64_t)prev.va + prev.size > (uint64_t)e.va) {
            /* Sorted by va ascending, non-overlapping. */
            return UDYNLINK_ERR_LOAD_BAD_SECTION_TABLE;
        }
        prev = e;
    }
    /* Main-section consistency with the header (the index convention the
     * VA map and the payload layout both rely on). */
    sect_entry_t m[3];
    sect_view_t view = { entries, tab->p_pool, num };
    for (uint32_t i = 0; i < 3; i++) {
        sect_entry_at(&view, i, &m[i]);
    }
    if (m[0].size != p_header->code_size ||
        m[1].va != p_header->code_size ||
        m[1].size != p_header->data_size ||
        m[2].va != (uint32_t)((uint64_t)p_header->code_size + p_header->data_size) ||
        m[2].size != p_header->bss_size) {
        return UDYNLINK_ERR_LOAD_BAD_SECTION_TABLE;
    }
    tab->p_entries = entries;
    tab->num = num;
    return UDYNLINK_OK;
}

/* Index of the section containing link-time VA `va`, or -1.  Entries are
 * sorted by va and non-overlapping (enforced by get_sectab), so the scan
 * can stop at the first section starting past `va`. */
static int sect_find_by_va(const sect_view_t *tab, uint32_t va) {
    for (size_t i = 0; i < tab->num; i++) {
        sect_entry_t e;
        sect_entry_at(tab, i, &e);
        if (va < e.va) {
            break;
        }
        if ((uint64_t)(va - e.va) < e.size) {
            return (int)i;
        }
    }
    return -1;
}

/* Payload bytes of the entries from index `from` whose class is selected by
 * `cls_mask` (bit i = class i).  Sums in 64 bits so a table of inflated
 * sizes cannot overflow the size comparisons downstream. */
static uint64_t sum_section_sizes(const sect_view_t *tab, size_t from, unsigned cls_mask) {
    uint64_t total = 0;
    for (size_t k = from; k < tab->num; k++) {
        sect_entry_t e;
        sect_entry_at(tab, k, &e);
        if (cls_mask & (1u << e.cls)) {
            total += e.size;
        }
    }
    return total;
}

/* In-block byte offset where main-section placement starts: after the LOT
 * and the non-main base array, plus the metadata (including the section
 * table) in COPY_ALL. */
static size_t get_main_sections_start(const udynlink_module_header_t *p_header, const sect_view_t *tab, udynlink_load_mode_t mode) {
    size_t off = p_header->num_lot * sizeof(uint32_t) + get_nonmain_bases_size(tab);
    if (mode == UDYNLINK_LOAD_MODE_COPY_ALL) {
        off += get_code_offset_from_header(p_header);
    }
    return off;
}

/* In-block offset of main section `idx` (0=.text, 1=.data, 2=.bss) of a
 * sectioned module: each main section base is aligned up to its declared
 * alignment, and the padding is part of ram_size.  XIP keeps .text in the
 * image — callers never ask for idx 0 under XIP. */
static size_t get_main_section_offset(const udynlink_module_header_t *p_header, const sect_view_t *tab, udynlink_load_mode_t mode, size_t idx) {
    if (tab->num <= idx) {
        return 0; // defensive: callers pass validated views
    }
    size_t off = get_main_sections_start(p_header, tab, mode);
    for (size_t i = (mode == UDYNLINK_LOAD_MODE_XIP) ? 1 : 0; i <= idx; i++) {
        sect_entry_t e;
        sect_entry_at(tab, i, &e);
        off = (off + e.align - 1) & ~(size_t)(e.align - 1);
        if (i == idx) {
            return off;
        }
        off += e.size;
    }
    return off;
}

/* Alignment the module's main RAM block must satisfy: the maximum over the
 * main sections (4 for untagged modules — nothing else is deliverable for
 * them). */
static size_t get_main_align(const udynlink_module_header_t *p_header, const sect_view_t *tab) {
    size_t align = 4; // the LOT and every block access are word-granular
    if (tab->num > 0) {
        for (size_t i = 0; i < 3; i++) {
            sect_entry_t e;
            sect_entry_at(tab, i, &e);
            if (e.align > align) {
                align = e.align;
            }
        }
    }
    return align;
}

// Gets the address of the code
static uint8_t *get_code_pointer(const udynlink_module_t *p_mod) {
    const udynlink_module_header_t *p_header = p_mod->p_header;

    if (is_sectioned(p_header)) {
        sect_view_t tab;
        (void)get_sectab(p_header, &tab, 0); // loaded modules carry a validated table
        if (UDYNLINK_LOAD_GET_MODE(p_mod) == UDYNLINK_LOAD_MODE_XIP) {
            // the main .text executes in the image
            return (uint8_t *)p_header + get_code_offset_from_header(p_header);
        }
        return (uint8_t *)p_mod->p_ram + get_main_section_offset(p_header, &tab, UDYNLINK_LOAD_GET_MODE(p_mod), 0);
    }
    if (UDYNLINK_LOAD_GET_MODE(p_mod) == UDYNLINK_LOAD_MODE_COPY_TEXT_DATA) { // the code is after the LOT in RAM.
        return (uint8_t*)p_mod->p_ram + p_header->num_lot * sizeof(uint32_t);// the code is after the LOT in RAM.
    } else { // the code is after the module header, the relocations and the symbol table
        return (uint8_t*)p_header + get_code_offset_from_header(p_header);
    }
}

// Gets the address of the data section (in RAM)
static uint8_t *get_data_pointer(const udynlink_module_t *p_mod) {
    const udynlink_module_header_t *p_header = p_mod->p_header;

    if (is_sectioned(p_header)) {
        sect_view_t tab;
        (void)get_sectab(p_header, &tab, 0);
        return (uint8_t *)p_mod->p_ram + get_main_section_offset(p_header, &tab, UDYNLINK_LOAD_GET_MODE(p_mod), 1);
    }
    switch (UDYNLINK_LOAD_GET_MODE(p_mod)) {
        case UDYNLINK_LOAD_MODE_XIP: // the data is after the LOT
            return (uint8_t*)p_mod->p_ram + p_header->num_lot * sizeof(uint32_t);
        case UDYNLINK_LOAD_MODE_COPY_TEXT_DATA: // the data is after the code in RAM (which is in turn after the LOT)
            return (uint8_t*)p_mod->p_ram + p_header->num_lot * sizeof(uint32_t) + p_header->code_size;
        case UDYNLINK_LOAD_MODE_COPY_ALL: // use directly the data section from the module header (after the code section)
            return (uint8_t*)p_header + get_code_offset_from_header(p_header) + p_header->code_size;
        default:
            UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, "Invalid load mode %d\n", (int)UDYNLINK_LOAD_GET_MODE(p_mod));
            return NULL;
    }
}


// Return a pointer to the relocation data (after the header)
static const uint32_t *get_relocs_pointer(const udynlink_module_t *p_mod) {
    const udynlink_module_header_t *p_header = p_mod->p_header;

    return (const uint32_t*)p_header + get_header_size(p_header) / sizeof(uint32_t);
}
static uint16_t compute_num_named_syms_raw(const uint32_t *p_symt, size_t symt_size_bytes) {
    size_t symt_words = symt_size_bytes / sizeof(uint32_t);
    /* The entry-count word and every name_off read must lie inside the
     * symbol table. symt_size is a header field, attacker-controlled. */
    if (symt_words < 1) return 0;
    uint32_t num_entries = *p_symt;
    uint16_t last_named = 0;
    uint8_t found_local = 0;

    for (size_t i = 1; i < num_entries; i++) {
        /* Each entry occupies two words: [val, name_off]. Reading
         * p_symt[i*2+1] needs (i*2+1) < symt_words. */
        if (i * 2 + 1 >= symt_words) break;
        uint32_t name_off = p_symt[i * 2 + 1];
        uint8_t sym_type = (name_off >> UDYNLINK_SYM_INFO_SHIFT) & UDYNLINK_SYM_INFO_TYPE_MASK;
        if (sym_type == UDYNLINK_SYM_TYPE_INTERNAL) {
            found_local = 1;
        } else {
            if (found_local) return 0;
            last_named = (uint16_t)i;
        }
    }

    /* name_off is attacker-controlled. Both the byte offset itself AND the
     * C-string read it induces must stay inside the symtab: a malformed
     * image that omits the NUL terminator within symt_size would let strcmp
     * walk out of the loader's own RAM copy of the symtab. Clamp the offset
     * and bound strncmp by the remaining symtab bytes; on missing-NUL the
     * compare returns non-zero, the sorted-check fails, and we fall back to
     * linear search via return 0. */
    for (size_t i = 2; i <= last_named; i++) {
        uint32_t prev_off = p_symt[(i - 1) * 2 + 1] & UDYNLINK_SYM_OFFSET_MASK;
        uint32_t cur_off  = p_symt[i * 2 + 1] & UDYNLINK_SYM_OFFSET_MASK;
        if (prev_off >= symt_size_bytes || cur_off >= symt_size_bytes) return 0;
        const char *prev = (const char*)p_symt + prev_off;
        const char *cur  = (const char*)p_symt + cur_off;
        size_t prev_remaining = symt_size_bytes - prev_off;
        size_t cur_remaining  = symt_size_bytes - cur_off;
        size_t cmp_bound = prev_remaining < cur_remaining ? prev_remaining : cur_remaining;
        if (strncmp(prev, cur, cmp_bound) > 0) return 0;
    }

    return last_named;
}

static uint16_t compute_num_named_syms(const udynlink_module_header_t *p_header) {
    return compute_num_named_syms_raw(get_sym_table_pointer(p_header), p_header->symt_size);
}

////////////////////////////////////////////////////////////////////////////////
// Helpers - various

// Marks the given module as "free" by zeroing its data structure
static void mark_module_free(udynlink_module_t *p_mod) {
    memset(p_mod, 0, sizeof(udynlink_module_t));
}

// Return the entry with the specified index in the given symbol table
// Returns "p_sym" if OK, NULL if index is out of range or an error occurred
static udynlink_sym_t *get_sym_at_raw(const uint32_t *p_symt, size_t index, udynlink_sym_t *p_sym, size_t symt_size_bytes) {
    uint32_t name_off;
    uint32_t info;
    size_t symt_words = symt_size_bytes / sizeof(uint32_t);

    /* entry-count word and the [val, name_off] pair for @index must all lie
     * inside the symtab. symt_size is attacker-controlled. */
    if (symt_words < 1) return NULL;
    if (index >= *p_symt) { // first word in the symbol table is the number of entries
        return NULL;
    }
    /* Needs p_symt[index*2+1] and p_symt[index*2+2]; require the higher one. */
    if (index * 2 + 2 >= symt_words) return NULL;
    // Read the offset to the name of the symbol and the symbol value
    name_off = p_symt[index * 2 + 1];
    p_sym->val = p_symt[index * 2 + 2];
    // Get symbol type and location
    info = name_off >> UDYNLINK_SYM_INFO_SHIFT;
    p_sym->type = info & UDYNLINK_SYM_INFO_TYPE_MASK;
    p_sym->location = (info & UDYNLINK_SYM_INFO_CODE_MASK) ? UDYNLINK_SYM_LOCATION_CODE : UDYNLINK_SYM_LOCATION_DATA;
    // Sanity check
    if ((index == UDYNLINK_SYM_NAME_OFFSET) && (p_sym->type != UDYNLINK_SYM_TYPE_MODULE_NAME)) {
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, "Module name symbol doesn't have the correct type!\n");
        return NULL;
    }
    // Get name pointer (if available)
    if (p_sym->type != UDYNLINK_SYM_TYPE_INTERNAL) { // local symbols don't have names
        /* name_off is attacker-controlled; keep the name base inside the
         * symtab so the caller's strcmp can't walk off the buffer. */
        if ((name_off & UDYNLINK_SYM_OFFSET_MASK) >= symt_size_bytes) return NULL;
        p_sym->name = (const char*)p_symt + (name_off & UDYNLINK_SYM_OFFSET_MASK);
    } else {
        p_sym->name = "(N/A)";
    }
    return p_sym;
}
static udynlink_sym_t *get_sym_at(const udynlink_module_header_t *p_header, size_t index, udynlink_sym_t *p_sym) {
    return get_sym_at_raw(get_sym_table_pointer(p_header), index, p_sym, p_header->symt_size);
}

/* Bound the symbol-table entry count claimed by *p_symt against what the
 * (attacker-controlled) symt_size can actually hold.  get_sym_at_raw admits an
 * index i only when i < *p_symt AND i*2+2 < symt_words, i.e. i < (symt_words-1)/2.
 * Returning the clamped count keeps a malformed image that lies about its
 * entry count from making a host iterate billions of NULL-returning calls. */
static size_t bounded_sym_count(const uint32_t *p_symt, size_t symt_size_bytes) {
    size_t symt_words = symt_size_bytes / sizeof(uint32_t);
    if (symt_words < 1) {
        return 0;
    }
    size_t claimed = *p_symt;
    size_t max_valid = (symt_words >= 3) ? (symt_words - 1) / 2 : 0;
    return claimed < max_valid ? claimed : max_valid;
}

/* Runtime base of section `idx` of a loaded sectioned module.  Main
 * sections are derived from the RAM block by the layout walk (XIP keeps
 * .text in the image); non-main sections live in the base array that
 * follows the LOT. */
static uintptr_t get_section_base_idx(const udynlink_module_t *p_mod, const sect_view_t *tab, size_t idx) {
    if (idx < 3) {
        if (idx == 0 && UDYNLINK_LOAD_GET_MODE(p_mod) == UDYNLINK_LOAD_MODE_XIP) {
            return (uintptr_t)p_mod->p_header + get_code_offset_from_header(p_mod->p_header);
        }
        return (uintptr_t)p_mod->p_ram + get_main_section_offset(p_mod->p_header, tab, UDYNLINK_LOAD_GET_MODE(p_mod), idx);
    }
    if (p_mod->p_ram == NULL) {
        return 0;
    }
    uintptr_t base = 0;
    /* memcpy so the array access stays alignment-safe on every target: the
     * array starts at p_ram + num_lot*4, which need not be pointer-aligned. */
    memcpy(&base, (const uint8_t *)p_mod->p_ram + p_mod->p_header->num_lot * sizeof(uint32_t) + (idx - 3) * sizeof(uintptr_t), sizeof(base));
    return base;
}

/* Store one non-main section base into the array after the LOT. */
static void set_section_base_idx(udynlink_module_t *p_mod, size_t ordinal, uintptr_t base) {
    memcpy((uint8_t *)p_mod->p_ram + p_mod->p_header->num_lot * sizeof(uint32_t) + ordinal * sizeof(uintptr_t), &base, sizeof(base));
}
/* Free every non-main section the loader allocated, with the same (section,
 * align, flags) the allocation used.  Called on unload and on load error
 * paths — always BEFORE the main block goes away, since the base array
 * lives there.  The array is zeroed right after the main allocation, so a
 * partially-loaded module frees exactly the sections that were recorded. */
static void free_nonmain_sections(const udynlink_module_t *p_mod, const sect_view_t *tab) {
    if (p_mod->p_ram == NULL || tab->num <= 3) {
        return;
    }
    for (size_t k = 3; k < tab->num; k++) {
        uintptr_t base = get_section_base_idx(p_mod, tab, k);
        if (base == 0) {
            break; // sequential allocation: the first gap ends the recorded run
        }
        sect_entry_t e;
        sect_entry_at(tab, k, &e);
        udynlink_external_free((void *)base, sect_name(tab, &e), e.align, sect_host_flags(&e));
    }
}

/* Runtime address for link-time VA `va` through the section map, or 0 with
 * *p_found == 0 when no section contains it. */
static uintptr_t resolve_runtime_va(const udynlink_module_t *p_mod, const sect_view_t *tab, uint32_t va, int *p_found) {
    int idx = sect_find_by_va(tab, va);
    if (idx < 0) {
        *p_found = 0;
        return 0;
    }
    sect_entry_t e;
    sect_entry_at(tab, (size_t)idx, &e);
    *p_found = 1;
    return get_section_base_idx(p_mod, tab, (size_t)idx) + (va - e.va);
}

/* Word a relocation with the given lot_offset writes, or NULL when the
 * offset is out of range.  Untagged: a LOT slot or a .data word (today's
 * num_lot + data_size/4 bound, verbatim).  Sectioned: the corresponding
 * link-time VA (code_size + 4*(lot_offset - num_lot), the encoder's
 * convention) must lie inside a CODE or DATA section — BSS is never a
 * relocation target — and the write lands at that VA's runtime address,
 * which may be a tagged section's host-provided block. */
static uint32_t *get_reloc_target(const udynlink_module_t *p_mod, const sect_view_t *tab, uint32_t lot_offset) {
    const udynlink_module_header_t *p_header = p_mod->p_header;
    if (lot_offset < p_header->num_lot) {
        if (p_mod->p_ram == NULL) {
            return NULL;
        }
        return (uint32_t *)p_mod->p_ram + lot_offset;
    }
    uint32_t word_idx = lot_offset - p_header->num_lot;
    uint32_t va = p_header->code_size + 4u * word_idx;
    if (tab->num == 0) {
        if (word_idx >= p_header->data_size / sizeof(uint32_t)) {
            return NULL;
        }
        return (uint32_t *)get_data_pointer(p_mod) + word_idx;
    }
    int idx = sect_find_by_va(tab, va);
    if (idx < 0) {
        return NULL;
    }
    sect_entry_t e;
    sect_entry_at(tab, (size_t)idx, &e);
    if (e.cls != UDYNLINK_SEC_CLASS_CODE && e.cls != UDYNLINK_SEC_CLASS_DATA) {
        return NULL; // BSS is never a relocation target
    }
    return (uint32_t *)(get_section_base_idx(p_mod, tab, (size_t)idx) + (va - e.va));
}


// Offset the given symbol relative to the required base address (.code or .data), based on the symbol location
// The function returns p_sym after it applies the offset to p_sym->val.
static udynlink_sym_t *offset_sym(const udynlink_module_t *p_mod, udynlink_sym_t *p_sym) {
    uintptr_t prev_val = p_sym->val;
    // Weak symbols are initially offset like internal/exported symbols so the
    // module's own definition is the default.  If the host provides an override
    // the loader patches the LOT/data entry afterwards.
    if ((p_sym->type == UDYNLINK_SYM_TYPE_INTERNAL) || (p_sym->type == UDYNLINK_SYM_TYPE_EXPORTED) || (p_sym->type == UDYNLINK_SYM_TYPE_WEAK)) {
        if (is_sectioned(p_mod->p_header)) {
            // Sectioned images carry flat link-space VAs, resolved through
            // the section map.  Load-path callers validate resolvability
            // before writing, so an unmapped value only reaches this path
            // through post-load queries and reports as 0.
            sect_view_t tab;
            if (get_sectab(p_mod->p_header, &tab, 0) == UDYNLINK_OK) {
                int found = 0;
                p_sym->val = resolve_runtime_va(p_mod, &tab, (uint32_t)p_sym->val, &found);
            } else {
                p_sym->val = 0;
            }
        } else if (p_sym->location == UDYNLINK_SYM_LOCATION_CODE) {
            p_sym->val += (uintptr_t)get_code_pointer(p_mod);
        } else {
            p_sym->val += (uintptr_t)get_data_pointer(p_mod);
        }
    }
    UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Symbol %s relocated relative to %s, orig value is %08X, new value is %08X\n", p_sym->name, p_sym->location == UDYNLINK_SYM_LOCATION_CODE ? "code" : "data", (uint32_t)prev_val, (uint32_t)p_sym->val);
    return p_sym;
}

////////////////////////////////////////////////////////////////////////////////
/* Sectioned variant: LOT + non-main base array + metadata (COPY_ALL) + the
 * main sections, each aligned up to its declared alignment — the padding is
 * part of the block.  Tagged sections are allocated separately by the host
 * callbacks and are not part of this size. */
static size_t get_ram_size_sections(const udynlink_module_header_t *p_header, const sect_view_t *tab, udynlink_load_mode_t load_mode) {
    size_t tot_size = p_header->num_lot * sizeof(uint32_t) + get_nonmain_bases_size(tab);
    if (load_mode == UDYNLINK_LOAD_MODE_COPY_ALL) {
        tot_size += get_code_offset_from_header(p_header);
    }
    for (size_t i = (load_mode == UDYNLINK_LOAD_MODE_XIP) ? 1 : 0; i < 3; i++) {
        sect_entry_t e;
        sect_entry_at(tab, i, &e);
        tot_size = (tot_size + e.align - 1) & ~(size_t)(e.align - 1);
        tot_size += e.size;
    }
    return tot_size;
}

static size_t get_ram_size_for_header(const udynlink_module_header_t *p_header, udynlink_load_mode_t load_mode) {
    if (is_sectioned(p_header)) {
        sect_view_t tab;
        if (get_sectab(p_header, &tab, 0) == UDYNLINK_OK && tab.num > 0) {
            return get_ram_size_sections(p_header, &tab, load_mode);
        }
        /* Malformed table: get_sectab rejects the image at load; fall
         * through to the header-only arithmetic. */
    }
    size_t tot_size = p_header->num_lot * sizeof(uint32_t) + p_header->data_size + p_header->bss_size;
    if (load_mode == UDYNLINK_LOAD_MODE_COPY_TEXT_DATA) {
        tot_size += p_header->code_size;
    } else if (load_mode == UDYNLINK_LOAD_MODE_COPY_ALL) {
        tot_size += get_code_offset_from_header(p_header) + p_header->code_size;
    }
    return tot_size;
}

////////////////////////////////////////////////////////////////////////////////
// Helpers - symbol resolution

// Single-tier symbol resolution: host callback only.
// Returns the resolved address, 0 if unresolved, or UDYNLINK_SYM_DEFERRED.
static uintptr_t resolve_symbol(const udynlink_module_t *p_mod, const char *name) {
    uintptr_t sym_addr = udynlink_external_resolve_symbol(p_mod, name);
    if (sym_addr == UDYNLINK_SYM_DEFERRED) {
        return UDYNLINK_SYM_DEFERRED;
    }
    return sym_addr;
}

////////////////////////////////////////////////////////////////////////////////
// Helpers - relocation application

udynlink_error_t udynlink_load_apply_relocations(udynlink_module_t *p_mod,
    const udynlink_module_header_t *p_header,
    const uint32_t *p_relocations,
    const uint32_t *p_symtab) {
    uint32_t *p_lot = (uint32_t *)p_mod->p_ram;
    udynlink_sym_t sym;
    udynlink_error_t res = UDYNLINK_OK;
    sect_view_t tab;
    int sectioned = 0;

    if (get_sectab(p_header, &tab, 0) != UDYNLINK_OK) {
        if (is_sectioned(p_header)) {
            /* Sectioned relocations resolve through the section map; a
             * table that cannot be parsed cannot be relocated against. */
            return UDYNLINK_ERR_LOAD_BAD_SECTION_TABLE;
        }
    } else {
        sectioned = tab.num > 0;
    }

    /* Reads of p_relocations[i*2..] are safe — num_rels is bounded by the
     * image builder (udynlink_image_from_memory) before we are called.
     * Write targets are validated per relocation by get_reloc_target: the
     * LOT bound and (sectioned) the CODE/DATA membership of the target VA.
     * BSS is never a relocation target (zeroed at load). */

    UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "LOT base: %p, .data starts at %p, .code starts at %p\n", p_lot, get_data_pointer(p_mod), get_code_pointer(p_mod));


    for (size_t i = 0; i < p_header->num_rels; i++) {
        uint32_t lot_offset = p_relocations[i * 2];
        uint32_t symt_offset = p_relocations[i * 2 + 1];

        if (symt_offset & (1u << 31)) {
            // https://stackoverflow.com/questions/75558729/position-independent-code-gcc-versus-armcc
            // R_ARM_ABS32 data relocation
            // *offset += &data - value
            if (lot_offset < p_header->num_lot) {  // would underflow the .data index below
                res = UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE;
                goto exit;
            }
            uint32_t *p = get_reloc_target(p_mod, &tab, lot_offset);
            if (p == NULL) {
                res = UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE;
                goto exit;
            }
            if (sectioned) {
                // The in-place word holds a link-time VA; the low bits of
                // `value` (the section symbol's st_value) are ignored — the
                // containing section comes from the section map.
                int found = 0;
                uintptr_t target = resolve_runtime_va(p_mod, &tab, *p, &found);
                if (!found) {
                    res = UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE;
                    goto exit;
                }
                *p = (uint32_t)target;
            } else {
                *p += (uint32_t)(uintptr_t)get_data_pointer(p_mod) - (symt_offset & 0x7FFFFFFF);
            }
            continue;
        }

        if (symt_offset & (1u << 30)) {
            if (lot_offset < p_header->num_lot) {  // would underflow the .data index below
                res = UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE;
                goto exit;
            }
            uint32_t *p = get_reloc_target(p_mod, &tab, lot_offset);
            if (p == NULL) {
                res = UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE;
                goto exit;
            }
            if (sectioned) {
                // Same VA-map operation as the ABS32 form: the word holds a
                // link-time VA and the low bits of `value` are ignored.
                int found = 0;
                uintptr_t target = resolve_runtime_va(p_mod, &tab, *p, &found);
                if (!found) {
                    res = UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE;
                    goto exit;
                }
                *p = (uint32_t)target;
            } else {
                *p = ((uint32_t)(uintptr_t)get_code_pointer(p_mod) + *p);
            }
            continue;
        }

        if (get_sym_at_raw(p_symtab, symt_offset, &sym, p_header->symt_size) == NULL) { // symbol table offset is out of range, shouldn't happen
            res = UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE;
            goto exit;
        }

        // Relocations in LOT and .data are encoded in the same way, they can be differentiated based on the value of lot_offset.
        // If lot_offset is larger than or equal to the number of LOT entries, this relocation applies to data, not to LOT.
        uint32_t *p_rel_location = get_reloc_target(p_mod, &tab, lot_offset);
        if (p_rel_location == NULL) {
            res = UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE;
            goto exit;
        }

        switch (sym.type) {
            case UDYNLINK_SYM_TYPE_INTERNAL:
            case UDYNLINK_SYM_TYPE_EXPORTED:
                UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Applying relocation for symbol at index %u, name=%s, type=%d, data_reloc=%d at lot_offset=%u, value=%08X\n", symt_offset, sym.name, sym.type, sym.location, lot_offset, sym.val);
                if (sectioned) {
                    // Flat link-space VA: the symbol must resolve through the
                    // section map, or the module is malformed.
                    int found = 0;
                    uintptr_t target = resolve_runtime_va(p_mod, &tab, (uint32_t)sym.val, &found);
                    if (!found) {
                        res = UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE;
                        goto exit;
                    }
                    *p_rel_location = (uint32_t)target;
                } else {
                    *p_rel_location = offset_sym(p_mod, &sym)->val;
                }
                break;


            case UDYNLINK_SYM_TYPE_WEAK:
                // Write the module's own address first (default fallback), then
                // try host override.  Unlike EXTERN, failure to resolve a weak
                // symbol is not fatal — the module definition remains.
                UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Applying weak relocation for symbol at index %u, name=%s at lot_offset=%u\n", symt_offset, sym.name, lot_offset);
                if (sectioned) {
                    int found = 0;
                    uintptr_t target = resolve_runtime_va(p_mod, &tab, (uint32_t)sym.val, &found);
                    if (!found) {
                        res = UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE;
                        goto exit;
                    }
                    *p_rel_location = (uint32_t)target;
                } else {
                    *p_rel_location = offset_sym(p_mod, &sym)->val;
                }
                {
                    uintptr_t sym_addr = resolve_symbol(p_mod, sym.name);
                    if (sym_addr == UDYNLINK_SYM_DEFERRED) {
                        // Keep module's own default, defer override
                        break;
                    }
                    if (sym_addr > 0) {
                        *p_rel_location = (uint32_t)sym_addr;
                    }
                }
                break;

            case UDYNLINK_SYM_TYPE_EXTERN:
                UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Applying extern relocation for symbol at index %u, name=%s at lot_offset=%u\n", symt_offset, sym.name, lot_offset);
                {
                    uintptr_t sym_addr = resolve_symbol(p_mod, sym.name);
                    if (sym_addr == UDYNLINK_SYM_DEFERRED) {
                        *p_rel_location = 0;
                        break;
                    }
                    if (sym_addr > 0) {
                        *p_rel_location = (uint32_t)sym_addr;
                    } else {
                        UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, "Unable to resolve relocation for extern symbol '%s'\n", sym.name);
                        res = UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL;
                        goto exit;
                    }
                }
                break;

            case UDYNLINK_SYM_TYPE_MODULE_NAME: // no relocations should be emitted against the name of the module
                res = UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE;
                goto exit;
        }
    }

exit:
    return res;
}

// Rebase every module-internal absolute pointer in an already-loaded (and
// recently moved) module by the given code/data deltas.  Mirrors the
// relocation walk in udynlink_load_apply_relocations but, instead of the
// load-time formula, adds the move delta exactly once per slot so that
// repeated relocates compose.  EXTERN slots (host-absolute) and
// host-overridden weak slots are left untouched; module-internal slots
// (INTERNAL/EXPORTED/WEAK-default) and the additive R_ARM_ABS32 /
// code-base data-section pointers are shifted by the matching delta.
static udynlink_error_t rebase_module_pointers(udynlink_module_t *p_mod,
                                               uintptr_t code_delta,
                                               uintptr_t data_delta,
                                               uintptr_t old_code,
                                               uintptr_t old_data) {
    const udynlink_module_header_t *p_header = p_mod->p_header;
    const uint32_t *p_rels = get_relocs_pointer(p_mod);
    uint32_t *p_lot = (uint32_t *)p_mod->p_ram;
    uint32_t *p_data = (uint32_t *)get_data_pointer(p_mod);
    uint16_t num_lot = p_header->num_lot;

    for (size_t i = 0; i < p_header->num_rels; i++) {
        uint32_t lot_offset = p_rels[i * 2];
        uint32_t symt_offset = p_rels[i * 2 + 1];

        // R_ARM_ABS32 data-section pointer: an absolute .data address after
        // load; a single additive delta rebases it.
        if (symt_offset & (1u << 31)) {
            uint32_t *p = p_data + (lot_offset - num_lot);
            *p += (uint32_t)data_delta;
            continue;
        }
        // Code-base data-section pointer: an absolute .code address after load.
        if (symt_offset & (1u << 30)) {
            uint32_t *p = p_data + (lot_offset - num_lot);
            *p += (uint32_t)code_delta;
            continue;
        }

        udynlink_sym_t sym;
        if (get_sym_at_raw(get_sym_table_pointer(p_header), symt_offset, &sym, p_header->symt_size) == NULL)
            continue; // defensive: rebase must never fail mid-move

        uint32_t *p_rel = (lot_offset < num_lot) ? p_lot + lot_offset
                                                 : p_data + (lot_offset - num_lot);

        switch (sym.type) {
            case UDYNLINK_SYM_TYPE_INTERNAL:
            case UDYNLINK_SYM_TYPE_EXPORTED:
                *p_rel += (sym.location == UDYNLINK_SYM_LOCATION_CODE)
                              ? (uint32_t)code_delta : (uint32_t)data_delta;
                break;

            case UDYNLINK_SYM_TYPE_WEAK: {
                // Detect module-default vs host-override: the default is the
                // module's own (pre-move) code/data base + section offset.
                uintptr_t default_old = (sym.location == UDYNLINK_SYM_LOCATION_CODE)
                                            ? old_code : old_data;
                default_old += sym.val;
                if (*p_rel == (uint32_t)default_old)
                    *p_rel += (sym.location == UDYNLINK_SYM_LOCATION_CODE)
                                  ? (uint32_t)code_delta : (uint32_t)data_delta;
                // else: host override — preserve host-absolute address.
                break;
            }

            case UDYNLINK_SYM_TYPE_EXTERN:
            case UDYNLINK_SYM_TYPE_MODULE_NAME:
                // Host-absolute or no-op: leave untouched.
                break;
        }
    }

    (void)old_code; (void)old_data; // referenced via default_old above
    return UDYNLINK_OK;
}

////////////////////////////////////////////////////////////////////////////////
// Image builders

void udynlink_image_from_memory(const void *base_addr, udynlink_module_image_t *out) {
    const udynlink_module_header_t *h = base_addr;
    out->p_header = h;
    out->p_relocations = (const uint32_t *)h + get_header_size(h) / sizeof(uint32_t);
    out->p_symtab = out->p_relocations + h->num_rels * 2;
    out->p_code = (const uint8_t *)h + get_code_offset_from_header(h);
    out->p_data = out->p_code + h->code_size;
}

void udynlink_image_from_module(const udynlink_module_t *p_mod, udynlink_module_image_t *out) {
    udynlink_image_from_memory(p_mod->p_header, out);
}

////////////////////////////////////////////////////////////////////////////////
// Public interface - planning / validation

udynlink_error_t udynlink_validate_header(const udynlink_module_header_t *header) {
    if (header->sign != UDYNLINK_MODULE_SIGN) {
        return UDYNLINK_ERR_LOAD_INVALID_SIGN;
    }
    if (header->udynlink_version > UDYNLINK_LOADER_ABI_VERSION) {
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, "Module udynlink version %d.%d > loader version %d.%d\n",
            UDYNLINK_GET_MAJOR_VERSION(header->udynlink_version), UDYNLINK_GET_MINOR_VERSION(header->udynlink_version),
            UDYNLINK_GET_MAJOR_VERSION(UDYNLINK_LOADER_ABI_VERSION), UDYNLINK_GET_MINOR_VERSION(UDYNLINK_LOADER_ABI_VERSION));
        return UDYNLINK_ERR_LOAD_VERSION_MISMATCH;
    }
    return UDYNLINK_OK;
}

size_t udynlink_compute_ram_size(const udynlink_module_header_t *header, udynlink_load_mode_t mode) {
    return get_ram_size_for_header(header, mode);
}

size_t udynlink_get_image_metadata_size(const udynlink_module_header_t *header) {
    return get_code_offset_from_header(header);
}

const char *udynlink_image_get_module_name(const uint32_t *p_symtab) {
    if (!p_symtab) return NULL;
    udynlink_sym_t sym;
    /* Public API has no symt_size argument; the caller must hand us a valid
     * symtab whose size covers index 0. Pass SIZE_MAX to disable the bound
     * (behaves like the pre-fix path) — callers come from udynlink_image_*,
     * always pointing at a well-formed image's symtab. */
    udynlink_sym_t *p_sym = get_sym_at_raw(p_symtab, UDYNLINK_SYM_NAME_OFFSET, &sym, SIZE_MAX);
    if (p_sym == NULL) {
        return NULL;
    } else {
        return p_sym->name;
    }
}

////////////////////////////////////////////////////////////////////////////////
// Public interface - module loading

udynlink_error_t udynlink_load_module_image(udynlink_module_t *p_mod,
    const udynlink_module_image_t *image,
    void *load_addr, size_t load_size,
    udynlink_load_mode_t load_mode) {

    void *ram_addr = NULL;
    udynlink_error_t res = UDYNLINK_OK;
    const udynlink_module_header_t *p_header = image->p_header;

    if (!p_mod)
        return UDYNLINK_ERR_INVALID_MODULE;

    p_mod->p_header = p_header;
    // Zeroed up front so the shared error path below is safe on exits that
    // happen before the table is parsed (bad signature/version).
    sect_view_t tab = { NULL, NULL, 0 };
    size_t main_align = sizeof(uint32_t);
    UDYNLINK_LOAD_SET_MODE(p_mod, load_mode);

    res = udynlink_validate_header(p_header);
    if (res != UDYNLINK_OK)
        goto exit;

    // Parse and validate the section table before anything derived from it
    // is used (the flag/count combination, entry fields and extents are all
    // attacker-controlled in the fuzz threat model).
    res = get_sectab(p_header, &tab, 1);
    if (res != UDYNLINK_OK)
        goto exit;
    if (tab.num > 0) {
        main_align = get_main_align(p_header, &tab);
    }

    // Reject images whose derived total (header + relocs + symtab + section
    // table + payloads) exceeds the sanity cap, before any derived length is
    // used as a copy size. Sectioned extents are table-derived and fail with
    // BAD_SECTION_TABLE; the untagged path keeps today's error code.
    uint64_t image_extent = (uint64_t)get_code_offset_from_header(p_header) + p_header->code_size + p_header->data_size;
    if (tab.num > 0) {
        image_extent += sum_section_sizes(&tab, 3, (1u << UDYNLINK_SEC_CLASS_CODE) | (1u << UDYNLINK_SEC_CLASS_DATA));
    }
    if (image_extent > UDYNLINK_MAX_IMAGE_SIZE) {
        res = (tab.num > 0) ? UDYNLINK_ERR_LOAD_BAD_SECTION_TABLE : UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE;
        goto exit;
    }

    UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Processing module image named '%s' with load mode %d\n", udynlink_image_get_module_name(image->p_symtab), (int)load_mode);

    // Allocate RAM or check given RAM region, as needed
    size_t ram_size = (tab.num > 0) ? get_ram_size_sections(p_header, &tab, load_mode)
                                    : get_ram_size_for_header(p_header, load_mode);
    if (ram_size > UDYNLINK_MAX_RAM_SIZE) {
        res = UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE;
        goto exit;
    }
    if (tab.num > 0) {
        // Every tagged section becomes its own host allocation; cap their
        // total together with the main block (contract §1.2) so a table that
        // inflates one section is rejected before any callback runs.
        uint64_t alloc_total = (uint64_t)ram_size + sum_section_sizes(&tab, 3, 0xFFu);
        if (alloc_total > UDYNLINK_MAX_RAM_SIZE) {
            res = UDYNLINK_ERR_LOAD_BAD_SECTION_TABLE;
            goto exit;
        }
    }
    if (ram_size > 0) {
        if (load_addr == NULL) {
            UDYNLINK_LOAD_CLR_FOREIGN_RAM(p_mod);
            if ((ram_addr = udynlink_external_malloc(ram_size, NULL, main_align, 0)) == NULL) {
                res = UDYNLINK_ERR_LOAD_OUT_OF_MEMORY;
                goto exit;
            }
            UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Allocated %u bytes for module\n", ram_size);
        } else {
            UDYNLINK_LOAD_SET_FOREIGN_RAM(p_mod);
            if (load_size < ram_size) {
                res = UDYNLINK_ERR_LOAD_RAM_LEN_LOW;
                goto exit;
            }
            ram_addr = load_addr;
        }
        // Every access into this block is word-granular (LOT entries and
        // relocations/data are read and written as uint32_t), so the base
        // must be at least word-aligned; a sectioned module additionally
        // demands its main-block alignment (the max over its main sections).
        // Neither source guarantees that: a custom udynlink_external_malloc
        // may return a lesser alignment and load_addr is caller-controlled.
        // Failing the load beats corrupting every word the module touches.
        if ((uintptr_t)ram_addr & (main_align - 1)) {
            res = UDYNLINK_ERR_LOAD_RAM_UNALIGNED;
            if (load_addr == NULL) {
                /* p_ram is not set yet, so the shared error path below cannot
                 * free this block; give it back here to keep the failure
                 * leak-free. */
                udynlink_external_free(ram_addr, NULL, main_align, 0);
            }
            goto exit;
        }
        p_mod->p_ram = ram_addr;
        if (tab.num > 0) {
            // Zero the non-main base array right after the LOT so the error
            // paths only ever see bases the loader actually recorded.
            memset((uint8_t *)ram_addr + p_header->num_lot * sizeof(uint32_t), 0, get_nonmain_bases_size(&tab));
        }
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "RAM area for module is at %p (%u bytes)\n", ram_addr, ram_size);
    } else {
        p_mod->p_ram = NULL;
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Awesome! Module doesn't need any RAM\n");
    }

    // Copy to RAM as needed
    uint8_t *p_temp8 = (uint8_t *)ram_addr + p_header->num_lot * sizeof(uint32_t) + get_nonmain_bases_size(&tab);
    size_t code_offset = get_code_offset_from_header(p_header);

    if (tab.num > 0) {
        // Sectioned path. Main-section bases are aligned inside the block by
        // the layout walk; the metadata (COPY_ALL) keeps the section table so
        // post-load lookups resolve through it in every mode.
        sect_entry_t me;
        if (load_mode == UDYNLINK_LOAD_MODE_COPY_ALL) {
            // One region copy covers header, relocations, symbol table,
            // section table and its alignment padding.
            memcpy(p_temp8, image->p_header, code_offset);
            p_mod->p_header = (const udynlink_module_header_t *)p_temp8;
            for (size_t i = 0; i < 2; i++) {
                sect_entry_at(&tab, i, &me);
                memcpy((uint8_t *)p_mod->p_ram + get_main_section_offset(p_header, &tab, load_mode, i),
                       (i == 0) ? image->p_code : image->p_data, me.size);
            }
            UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Copied metadata (%u bytes) and main sections to RAM at %p\n", (unsigned)code_offset, p_temp8);
        } else if (load_mode == UDYNLINK_LOAD_MODE_COPY_TEXT_DATA) {
            sect_entry_at(&tab, 0, &me);
            if (me.size > 0) {
                memcpy((uint8_t *)p_mod->p_ram + get_main_section_offset(p_header, &tab, load_mode, 0), image->p_code, me.size);
            }
            sect_entry_at(&tab, 1, &me);
            if (me.size > 0) {
                memcpy((uint8_t *)p_mod->p_ram + get_main_section_offset(p_header, &tab, load_mode, 1), image->p_data, me.size);
            }
            UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Copied main code and data to RAM at %p\n", p_temp8);
        } else {
            // XIP: the main .text executes in the image; copy only main .data
            sect_entry_at(&tab, 1, &me);
            if (me.size > 0) {
                memcpy((uint8_t *)p_mod->p_ram + get_main_section_offset(p_header, &tab, load_mode, 1), image->p_data, me.size);
            }
            UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Copied main data to RAM at %p (%u bytes)\n", p_temp8, (unsigned)me.size);
        }

        // Tagged sections: one host allocation each, payload copied (or BSS
        // zeroed) at the resolved address — in every load mode, including
        // XIP: the host asked for that memory explicitly.
        size_t payload_off = (size_t)p_header->code_size + p_header->data_size; // payload concat: main text then data
        for (size_t k = 3; k < tab.num; k++) {
            sect_entry_t e;
            sect_entry_at(&tab, k, &e);
            void *sec = udynlink_external_malloc(e.size, sect_name(&tab, &e), e.align, sect_host_flags(&e));
            if (sec == NULL) {
                res = UDYNLINK_ERR_LOAD_SECTION_UNRESOLVED;
                goto exit;
            }
            // Record before the alignment check so the shared error path
            // frees this block with the allocation's (section, align, flags).
            set_section_base_idx(p_mod, k - 3, (uintptr_t)sec);
            if ((uintptr_t)sec & (e.align - 1)) {
                res = UDYNLINK_ERR_LOAD_SECTION_UNALIGNED;
                goto exit;
            }
            if (e.cls == UDYNLINK_SEC_CLASS_BSS) {
                memset(sec, 0, e.size);
            } else {
                memcpy(sec, image->p_code + payload_off, e.size);
                payload_off += e.size; // BSS has no payload slot in the concat
            }
        }

        // Main BSS at its aligned in-block offset.
        sect_entry_at(&tab, 2, &me);
        if (me.size > 0) {
            memset((uint8_t *)p_mod->p_ram + get_main_section_offset(p_header, &tab, load_mode, 2), 0, me.size);
        }
    } else {
    if (load_mode == UDYNLINK_LOAD_MODE_COPY_ALL) {
        // Copy header
        memcpy(p_temp8, image->p_header, get_header_size(p_header));
        // Copy relocations
        memcpy(p_temp8 + get_header_size(p_header), image->p_relocations,
               p_header->num_rels * 2 * sizeof(uint32_t));
        // Copy symbol table
        memcpy(p_temp8 + get_header_size(p_header) + p_header->num_rels * 2 * sizeof(uint32_t),
               image->p_symtab, p_header->symt_size);
        // Pad to code_offset
        size_t pad = code_offset - (get_header_size(p_header) + p_header->num_rels * 2 * sizeof(uint32_t) + p_header->symt_size);
        if (pad > 0 && pad < 4) {
            memset(p_temp8 + get_header_size(p_header) + p_header->num_rels * 2 * sizeof(uint32_t) + p_header->symt_size, 0, pad);
        }
        // Copy code
        memcpy(p_temp8 + code_offset, image->p_code, p_header->code_size);
        // Copy data
        memcpy(p_temp8 + code_offset + p_header->code_size, image->p_data, p_header->data_size);
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Copied full module to RAM at %p (%u bytes)\n", p_temp8, code_offset + p_header->code_size + p_header->data_size);
        p_mod->p_header = (const udynlink_module_header_t *)p_temp8;
    } else if (load_mode == UDYNLINK_LOAD_MODE_COPY_TEXT_DATA) {
        // Copy code and data only; metadata stays in source. Guard against
        // ram_size == 0 (empty module: num_lot=bss_size=code_size=data_size=0)
        // which leaves p_temp8 NULL — calling memcpy with a NULL argument is
        // UB even when the size is zero.
        size_t cd_size = p_header->code_size + p_header->data_size;
        if (cd_size > 0) {
            memcpy(p_temp8, image->p_code, cd_size);
        }
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Copied code and data to RAM at %p (%u bytes)\n", p_temp8, p_header->code_size + p_header->data_size);
    } else {
        // XIP: copy only data
        if (p_header->data_size > 0) {
            memcpy(p_temp8, image->p_data, p_header->data_size);
        }
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Copied data to RAM at %p (%u bytes)\n", p_temp8, p_header->data_size);
    }

    // Zero out BSS (guarded the same way: when ram_size == 0, both the data
    // pointer and bss_size are zero, so the memset would be a no-op UB).
    if (p_header->bss_size > 0) {
        memset(get_data_pointer(p_mod) + p_header->data_size, 0, p_header->bss_size);
    }
    }

    // Process relocations
    res = udynlink_load_apply_relocations(p_mod, p_header, image->p_relocations, image->p_symtab);
    if (res != UDYNLINK_OK)
        goto exit;

    p_mod->num_named_syms = compute_num_named_syms(p_mod->p_header);

    UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Done loading module image\n");

exit:
    if (res != UDYNLINK_OK) {
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, error_codes[(int)res]);
        // Free the tagged sections before the main block: their bases live
        // in the block's base array. The array was zeroed right after the
        // main allocation, so a partially-loaded module frees exactly the
        // sections that were recorded (foreign main blocks still leave their
        // tagged sections loader-owned and freed here).
        free_nonmain_sections(p_mod, &tab);
        if ((p_mod->p_ram != NULL) && !UDYNLINK_LOAD_IS_FOREIGN_RAM(p_mod)) {
            udynlink_external_free(p_mod->p_ram, NULL, main_align, 0);
            UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Deallocated memory area at %p\n", p_mod->p_ram);
        }
        if (p_mod != NULL) {
            mark_module_free(p_mod);
        }
    }
    return res;
}

udynlink_error_t udynlink_load_module(udynlink_module_t *p_mod, const void *base_addr, void *load_addr, size_t load_size, udynlink_load_mode_t load_mode) {
    udynlink_module_image_t image;
    udynlink_image_from_memory(base_addr, &image);
    return udynlink_load_module_image(p_mod, &image, load_addr, load_size, load_mode);
}

/* Saves/restores the caller's r9 around the module call, mirroring
 * UDYNLINK_CALL_VOID in udynlink_call.h. The restore clobber also acts as a
 * hard barrier that defeats sibling-call optimization: without it, at -O2/-O3
 * /-Os and under LTO GCC rewrites the f() invocation as a tail call (bx r3)
 * AFTER the function epilogue (ldmia {…, r9, lr}), which restores the caller's
 * r9 from the stack and so clobbers the LOT base the module code relies on. */
void udynlink_cpp_init(udynlink_module_t *p_mod){
    udynlink_sym_t init_array_sym = {};
    if (udynlink_lookup_symbol(p_mod, "__init_array", &init_array_sym) == NULL) {
        return;
    }
    typedef void (*void_func)(void);
    void_func f = (void_func)init_array_sym.val;
    uint32_t prev_r9;
#if defined(__arm__) || defined(__thumb__)
    __asm volatile ("mov %0, r9" : "=r"(prev_r9) : :);
#else
    /* Host builds (fuzz/sanitizer gates) never execute ARM module code, so
     * there is no r9/LOT base to preserve. The asm also cannot be emitted
     * there: on x86 GAS a bare `r9` parses as a symbol reference and the
     * resulting R_X86_64_32S relocation breaks the PIE link. */
    (void)prev_r9;
#endif
    UDYNLINK_PREPARE_CALL(p_mod);
    f();
#if defined(__arm__) || defined(__thumb__)
    __asm volatile ("mov r9, %0" :: "r"(prev_r9) : "r9");
#endif
}

udynlink_error_t udynlink_unload_module(udynlink_module_t *p_mod) {
    if ((p_mod == NULL) || (p_mod->p_header == NULL)) {
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, error_codes[(int)UDYNLINK_ERR_INVALID_MODULE]);
        return UDYNLINK_ERR_INVALID_MODULE;
    }
    UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Unloading module at %p\n", p_mod);
    sect_view_t tab;
    size_t main_align = sizeof(uint32_t);
    if (get_sectab(p_mod->p_header, &tab, 0) == UDYNLINK_OK) {
        if (tab.num > 0) {
            main_align = get_main_align(p_mod->p_header, &tab);
        }
        // Tagged sections first: the base array lives in the main block.
        // They are loader-allocated even when the main block is foreign.
        free_nonmain_sections(p_mod, &tab);
    }
    if ((p_mod->p_ram != NULL) && !UDYNLINK_LOAD_IS_FOREIGN_RAM(p_mod)) {
        udynlink_external_free(p_mod->p_ram, NULL, main_align, 0);
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Deallocated memory area at %p\n", p_mod->p_ram);
    }
    mark_module_free(p_mod);
    return UDYNLINK_OK;
}

const char *udynlink_error_msg(udynlink_error_t* err) {
    return error_codes[(int)(intptr_t)err];
}

size_t udynlink_get_ram_size(const udynlink_module_t *p_mod) {
    // Main block only (LOT, base array, metadata, main sections with their
    // alignment padding); tagged sections live outside it. Same arithmetic
    // as udynlink_compute_ram_size(), keyed off the module's own mode.
    return get_ram_size_for_header(p_mod->p_header, UDYNLINK_LOAD_GET_MODE(p_mod));
}

////////////////////////////////////////////////////////////////////////////////
// Sectioned rebase

/* Context for re-applying a sectioned module's relocations after a move.
 * Every delta is derived from the image and the section bases — never from
 * the moved bytes. */
typedef struct {
    udynlink_module_t *p_mod;
    const sect_view_t *tab;
    void *old_p_ram;                        /* pre-move main block */
    const udynlink_section_move_t *moves;   /* tagged sections the host moved */
    size_t num_moves;
} sect_rebase_t;

/* Pre-move base of section idx: main sections derive from the pre-move
 * block, non-main sections from the base array — which still holds the old
 * values while the walk runs (the caller publishes the new bases after it). */
static uintptr_t sect_base_old(const sect_rebase_t *ctx, size_t idx) {
    if (idx < 3) {
        if (idx == 0 && UDYNLINK_LOAD_GET_MODE(ctx->p_mod) == UDYNLINK_LOAD_MODE_XIP) {
            // The main .text stayed in the image through the move.
            return (uintptr_t)ctx->p_mod->p_header + get_code_offset_from_header(ctx->p_mod->p_header);
        }
        void *block = (ctx->old_p_ram != NULL) ? ctx->old_p_ram : ctx->p_mod->p_ram;
        return (uintptr_t)block + get_main_section_offset(ctx->p_mod->p_header, ctx->tab, UDYNLINK_LOAD_GET_MODE(ctx->p_mod), idx);
    }
    return get_section_base_idx(ctx->p_mod, ctx->tab, idx);
}

/* Post-move base of section idx: a moved tagged section uses its move
 * target (the host has already copied the payload there); everything else
 * reads the current block and array. */
static uintptr_t sect_base_new(const sect_rebase_t *ctx, size_t idx) {
    for (size_t m = 0; m < ctx->num_moves; m++) {
        if (ctx->moves[m].idx == idx) {
            return (uintptr_t)ctx->moves[m].new_base;
        }
    }
    return get_section_base_idx(ctx->p_mod, ctx->tab, idx);
}

/* Section whose pre-move base range contains the runtime address `addr`,
 * or -1. */
static int sect_find_by_runtime(const sect_rebase_t *ctx, uintptr_t addr) {
    for (size_t i = 0; i < ctx->tab->num; i++) {
        sect_entry_t e;
        sect_entry_at(ctx->tab, i, &e);
        uintptr_t base = sect_base_old(ctx, i);
        if (addr >= base && (uint64_t)(addr - base) < e.size) {
            return (int)i;
        }
    }
    return -1;
}

/* Re-apply the module's relocations with per-section move deltas.  Slots in
 * a moved section are patched at their new addresses (the host's copy must
 * be byte-identical to the pre-move contents); values pointing into a moved
 * section are shifted by that section's delta; sections that did not move
 * contribute a delta of 0, so their slots only change when they point into
 * a moved section.  EXTERN slots and host-overridden weak slots keep their
 * host-absolute values.  Never fails: a slot that cannot be mapped is left
 * untouched. */
static void rebase_sections_pointers(sect_rebase_t *ctx) {
    const udynlink_module_header_t *p_header = ctx->p_mod->p_header;
    const uint32_t *p_rels = get_relocs_pointer(ctx->p_mod);
    uint32_t num_lot = p_header->num_lot;

    for (size_t i = 0; i < p_header->num_rels; i++) {
        uint32_t lot_offset = p_rels[i * 2];
        uint32_t symt_offset = p_rels[i * 2 + 1];

        if ((symt_offset & (1u << 31)) || (symt_offset & (1u << 30))) {
            // Base-additive forms: the word holds a runtime address written
            // at load.  Reverse-map it to its (pre-move) section and shift
            // by that section's delta, patching the word at its post-move
            // address.
            if (lot_offset < num_lot) {
                continue;
            }
            uint32_t va = p_header->code_size + 4u * (lot_offset - num_lot);
            int ti = sect_find_by_va(ctx->tab, va);
            if (ti < 0) {
                continue;
            }
            sect_entry_t te;
            sect_entry_at(ctx->tab, (size_t)ti, &te);
            if (te.cls == UDYNLINK_SEC_CLASS_BSS) {
                continue;
            }
            uint32_t *p = (uint32_t *)(sect_base_new(ctx, (size_t)ti) + (va - te.va));
            int si = sect_find_by_runtime(ctx, *p);
            if (si < 0) {
                continue;
            }
            *p += (uint32_t)(sect_base_new(ctx, (size_t)si) - sect_base_old(ctx, (size_t)si));
            continue;
        }

        udynlink_sym_t sym;
        if (get_sym_at_raw(get_sym_table_pointer(p_header), symt_offset, &sym, p_header->symt_size) == NULL) {
            continue;
        }

        uint32_t *p_slot;
        if (lot_offset < num_lot) {
            p_slot = (uint32_t *)ctx->p_mod->p_ram + lot_offset;
        } else {
            uint32_t va = p_header->code_size + 4u * (lot_offset - num_lot);
            int ti = sect_find_by_va(ctx->tab, va);
            if (ti < 0) {
                continue;
            }
            sect_entry_t te;
            sect_entry_at(ctx->tab, (size_t)ti, &te);
            if (te.cls == UDYNLINK_SEC_CLASS_BSS) {
                continue;
            }
            p_slot = (uint32_t *)(sect_base_new(ctx, (size_t)ti) + (va - te.va));
        }

        switch (sym.type) {
            case UDYNLINK_SYM_TYPE_INTERNAL:
            case UDYNLINK_SYM_TYPE_EXPORTED: {
                // The value is re-derived from the image and the new bases.
                int si = sect_find_by_va(ctx->tab, (uint32_t)sym.val);
                if (si < 0) {
                    break;
                }
                sect_entry_t se;
                sect_entry_at(ctx->tab, (size_t)si, &se);
                *p_slot = (uint32_t)(sect_base_new(ctx, (size_t)si) + ((uint32_t)sym.val - se.va));
                break;
            }

            case UDYNLINK_SYM_TYPE_WEAK: {
                // Shift only the module's own default; a host override stays
                // host-absolute.
                int si = sect_find_by_va(ctx->tab, (uint32_t)sym.val);
                if (si < 0) {
                    break;
                }
                sect_entry_t se;
                sect_entry_at(ctx->tab, (size_t)si, &se);
                if (*p_slot == (uint32_t)(sect_base_old(ctx, (size_t)si) + ((uint32_t)sym.val - se.va))) {
                    *p_slot = (uint32_t)(sect_base_new(ctx, (size_t)si) + ((uint32_t)sym.val - se.va));
                }
                break;
            }

            case UDYNLINK_SYM_TYPE_EXTERN:
            case UDYNLINK_SYM_TYPE_MODULE_NAME:
                // Host-absolute or no-op: leave untouched.
                break;
        }
    }
}

/* Core of udynlink_relocate_module / udynlink_relocate_module_sections:
 * moves the main RAM block and, for a sectioned module, re-binds the
 * tagged sections listed in `moves` (whose payloads the host has already
 * copied to their new bases). */
static udynlink_error_t relocate_main_block(udynlink_module_t *p_mod, void *new_ram, size_t new_size,
                                            const udynlink_section_move_t *moves, size_t num_moves) {
    udynlink_load_mode_t mode = UDYNLINK_LOAD_GET_MODE(p_mod);
    size_t ram_size = udynlink_get_ram_size(p_mod);

    if (!is_sectioned(p_mod->p_header)) {
        if (ram_size == 0)
            return UDYNLINK_OK; // nothing in RAM to move

        // Provision the destination buffer.  Malloc before freeing the old region
        // so an adjacent heap block can't be clobbered by the copy and malloc
        // cannot recycle the old block (use-after-free).
        void *dest;
        uint8_t old_foreign = UDYNLINK_LOAD_IS_FOREIGN_RAM(p_mod);
        if (new_ram == NULL) {
            UDYNLINK_LOAD_CLR_FOREIGN_RAM(p_mod);
            dest = udynlink_external_malloc(ram_size, NULL, sizeof(uint32_t), 0);
            if (dest == NULL) {
                UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, error_codes[(int)UDYNLINK_ERR_LOAD_OUT_OF_MEMORY]);
                return UDYNLINK_ERR_LOAD_OUT_OF_MEMORY;
            }
            UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Relocate: auto-allocated %u bytes at %p\n", (unsigned)ram_size, dest);
        } else {
            UDYNLINK_LOAD_SET_FOREIGN_RAM(p_mod);
            if (new_size < ram_size) {
                UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, error_codes[(int)UDYNLINK_ERR_LOAD_RAM_LEN_LOW]);
                return UDYNLINK_ERR_LOAD_RAM_LEN_LOW;
            }
            dest = new_ram;
        }

        // Capture old state before mutating the handle.
        uint8_t *old_p_ram = (uint8_t *)p_mod->p_ram;
        uintptr_t old_code = (uintptr_t)get_code_pointer(p_mod);
        uintptr_t old_data = (uintptr_t)get_data_pointer(p_mod);

        // No-op shortcut: caller passed the same buffer (avoid self-overlap copy).
        if (dest == old_p_ram) {
            if (old_foreign == 0)
                udynlink_external_free(old_p_ram, NULL, sizeof(uint32_t), 0);
            return UDYNLINK_OK;
        }

        // The data/LOT/bss block moves with the region base.  In XIP the code lives
        // in flash and is not in the moved block; in COPY_ALL/COPY_TEXT_DATA the
        // code is inside the block and moves with it.
        uintptr_t data_delta = (uintptr_t)dest - (uintptr_t)old_p_ram;
        uintptr_t code_delta = (mode == UDYNLINK_LOAD_MODE_XIP) ? 0 : data_delta;

        // Copy the whole region (no overlap: dest != old, and malloc-before-free).
        memcpy(dest, old_p_ram, ram_size);

        // Update the handle AFTER the copy so the copy used the old pointers.
        p_mod->p_ram = dest;
        if (mode == UDYNLINK_LOAD_MODE_COPY_ALL) {
            // In COPY_ALL the header lives right after the LOT inside the block.
            const udynlink_module_header_t *p_header = p_mod->p_header;
            p_mod->p_header = (const udynlink_module_header_t *)
                ((uint8_t *)dest + p_header->num_lot * sizeof(uint32_t));
        }
        // COPY_TEXT_DATA and XIP: p_header points at the unmoved source metadata.

        rebase_module_pointers(p_mod, code_delta, data_delta, old_code, old_data);

        // Free the old region if the loader owned it.  Foreign old buffers stay
        // caller-owned — the test harness frees its own.
        if (old_foreign == 0) {
            udynlink_external_free(old_p_ram, NULL, sizeof(uint32_t), 0);
            UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Relocate: freed old region at %p\n", old_p_ram);
        }

        UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Relocated module to %p (code_delta=0x%X, data_delta=0x%X)\n",
                       dest, (uint32_t)code_delta, (uint32_t)data_delta);
        return UDYNLINK_OK;
    }

    sect_view_t tab;
    if (get_sectab(p_mod->p_header, &tab, 0) != UDYNLINK_OK || tab.num == 0) {
        // Loaded modules always carry a validated table; this is defensive.
        return UDYNLINK_ERR_LOAD_BAD_SECTION_TABLE;
    }
    size_t main_align = get_main_align(p_mod->p_header, &tab);
    if (ram_size == 0 && num_moves == 0) {
        return UDYNLINK_OK;
    }
    if (ram_size > 0 && new_ram == (uint8_t *)p_mod->p_ram) {
        // Caller passed the block's own address: nothing to move (the tagged
        // moves, if any, are no-ops against unchanged bases).
        return UDYNLINK_OK;
    }

    // Provision the destination buffer.  All checks run before the handle's
    // ownership flags change, so a rejected call leaves the module exactly
    // as it was.  Malloc before freeing the old region so malloc cannot
    // recycle it (use-after-free).
    void *dest = NULL;
    uint8_t old_foreign = UDYNLINK_LOAD_IS_FOREIGN_RAM(p_mod);
    if (ram_size > 0) {
        if (new_ram == NULL) {
            dest = udynlink_external_malloc(ram_size, NULL, main_align, 0);
            if (dest == NULL) {
                UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, error_codes[(int)UDYNLINK_ERR_LOAD_OUT_OF_MEMORY]);
                return UDYNLINK_ERR_LOAD_OUT_OF_MEMORY;
            }
            UDYNLINK_LOAD_CLR_FOREIGN_RAM(p_mod);
            UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Relocate: auto-allocated %u bytes at %p\n", (unsigned)ram_size, dest);
        } else {
            if (new_size < ram_size) {
                UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, error_codes[(int)UDYNLINK_ERR_LOAD_RAM_LEN_LOW]);
                return UDYNLINK_ERR_LOAD_RAM_LEN_LOW;
            }
            if ((uintptr_t)new_ram & (main_align - 1)) {
                return UDYNLINK_ERR_LOAD_RAM_UNALIGNED;
            }
            UDYNLINK_LOAD_SET_FOREIGN_RAM(p_mod);
            dest = new_ram;
        }
    }

    uint8_t *old_p_ram = (uint8_t *)p_mod->p_ram;
    if (ram_size > 0) {
        // Copy the whole region (no overlap; malloc-before-free), then fix
        // the in-block header (COPY_ALL): it sits after the LOT and the base
        // array.  The base array travels with the block and still holds the
        // pre-move values for the rebase walk below.
        memcpy(dest, old_p_ram, ram_size);
        p_mod->p_ram = dest;
        if (mode == UDYNLINK_LOAD_MODE_COPY_ALL) {
            p_mod->p_header = (const udynlink_module_header_t *)
                ((uint8_t *)dest + p_mod->p_header->num_lot * sizeof(uint32_t) + get_nonmain_bases_size(&tab));
        }
    }

    // Re-apply relocations with per-section deltas (the walk reads pre-move
    // bases from old_p_ram and from the copied array), then publish the
    // moved bases.
    sect_rebase_t ctx = { p_mod, &tab, old_p_ram, moves, num_moves };
    rebase_sections_pointers(&ctx);
    for (size_t m = 0; m < num_moves; m++) {
        set_section_base_idx(p_mod, moves[m].idx - 3, (uintptr_t)moves[m].new_base);
    }

    // The old main block is the loader's to free; moved tagged sections are
    // the host's — it owns both the old and the new storage.
    if (old_foreign == 0 && ram_size > 0) {
        udynlink_external_free(old_p_ram, NULL, main_align, 0);
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Relocate: freed old region at %p\n", old_p_ram);
    }

    UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Relocated module main block to %p with %u section moves\n",
                   dest, (unsigned)num_moves);
    return UDYNLINK_OK;
}

udynlink_error_t udynlink_relocate_module(udynlink_module_t *p_mod,
                                          void *new_ram, size_t new_size) {
    if ((p_mod == NULL) || (p_mod->p_header == NULL)) {
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, error_codes[(int)UDYNLINK_ERR_INVALID_MODULE]);
        return UDYNLINK_ERR_INVALID_MODULE;
    }
    return relocate_main_block(p_mod, new_ram, new_size, NULL, 0);
}

udynlink_error_t udynlink_relocate_module_sections(udynlink_module_t *p_mod,
                                                   void *new_ram, size_t new_size,
                                                   const udynlink_section_move_t *moves,
                                                   size_t num_moves) {
    if ((p_mod == NULL) || (p_mod->p_header == NULL)) {
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, error_codes[(int)UDYNLINK_ERR_INVALID_MODULE]);
        return UDYNLINK_ERR_INVALID_MODULE;
    }
    if (num_moves > 0) {
        if (!is_sectioned(p_mod->p_header) || moves == NULL) {
            // Main sections move only with the block; a module without
            // section placement has nothing to move individually.
            return UDYNLINK_ERR_INVALID_MODULE;
        }
        sect_view_t tab;
        if (get_sectab(p_mod->p_header, &tab, 0) != UDYNLINK_OK || tab.num == 0) {
            return UDYNLINK_ERR_LOAD_BAD_SECTION_TABLE;
        }
        for (size_t m = 0; m < num_moves; m++) {
            if (moves[m].idx < 3 || moves[m].idx >= tab.num) {
                return UDYNLINK_ERR_INVALID_MODULE;
            }
            for (size_t j = 0; j < m; j++) {
                if (moves[j].idx == moves[m].idx) {
                    return UDYNLINK_ERR_INVALID_MODULE; // duplicate move
                }
            }
            if (moves[m].new_base == NULL) {
                return UDYNLINK_ERR_LOAD_SECTION_UNRESOLVED;
            }
            sect_entry_t e;
            sect_entry_at(&tab, moves[m].idx, &e);
            if ((uintptr_t)moves[m].new_base & (e.align - 1)) {
                return UDYNLINK_ERR_LOAD_SECTION_UNALIGNED;
            }
        }
    }
    return relocate_main_block(p_mod, new_ram, new_size, moves, num_moves);
}

////////////////////////////////////////////////////////////////////////////////
// Public interface - section placement

size_t udynlink_get_section_count(const udynlink_module_header_t *p_header) {
    if (p_header == NULL) {
        return 0;
    }
    if (!is_sectioned(p_header)) {
        return 3; // the implicit .text/.data/.bss view of an untagged module
    }
    sect_view_t tab;
    if (get_sectab(p_header, &tab, 0) != UDYNLINK_OK) {
        return 0;
    }
    return tab.num;
}

udynlink_error_t udynlink_get_section_info(const udynlink_module_header_t *p_header, size_t idx,
                                           udynlink_section_info_t *out) {
    if (p_header == NULL || out == NULL) {
        return UDYNLINK_ERR_INVALID_MODULE;
    }
    sect_view_t tab;
    if (get_sectab(p_header, &tab, 0) != UDYNLINK_OK) {
        return UDYNLINK_ERR_LOAD_BAD_SECTION_TABLE;
    }
    size_t num = (tab.num > 0) ? tab.num : 3;
    if (idx >= num) {
        return UDYNLINK_ERR_INVALID_MODULE;
    }
    memset(out, 0, sizeof(*out));
    if (tab.num == 0) {
        // Untagged: the implicit main sections, exactly as the loader treats
        // them — word-aligned bases derived by formula, no hint flags.
        static const uint8_t cls[3] = { UDYNLINK_SEC_CLASS_CODE, UDYNLINK_SEC_CLASS_DATA, UDYNLINK_SEC_CLASS_BSS };
        out->align = 4;
        out->sec_class = cls[idx];
        out->size = (idx == 0) ? p_header->code_size : (idx == 1) ? p_header->data_size : p_header->bss_size;
        return UDYNLINK_OK;
    }
    sect_entry_t e;
    sect_entry_at(&tab, idx, &e);
    out->size = e.size;
    out->align = e.align;
    out->flags = sect_host_flags(&e);
    out->sec_class = (uint8_t)e.cls;
    if (e.name_off != 0) {
        if (e.name_off >= p_header->symt_size ||
            memchr(tab.p_pool + e.name_off, 0, p_header->symt_size - e.name_off) == NULL) {
            return UDYNLINK_ERR_LOAD_BAD_SECTION_TABLE;
        }
        out->name = tab.p_pool + e.name_off;
    }
    return UDYNLINK_OK;
}

void *udynlink_get_section_base(const udynlink_module_t *p_mod, size_t idx) {
    if (p_mod == NULL || p_mod->p_header == NULL) {
        return NULL;
    }
    sect_view_t tab;
    if (get_sectab(p_mod->p_header, &tab, 0) != UDYNLINK_OK) {
        return NULL;
    }
    size_t num = (tab.num > 0) ? tab.num : 3;
    if (idx >= num) {
        return NULL;
    }
    if (tab.num == 0) {
        // Untagged implicit view: today's formula bases (XIP .text and the
        // data/bss arena addresses are exactly what the loader uses).
        if (idx == 0) {
            return get_code_pointer(p_mod);
        }
        if (idx == 1) {
            return get_data_pointer(p_mod);
        }
        return (uint8_t *)get_data_pointer(p_mod) + p_mod->p_header->data_size;
    }
    return (void *)get_section_base_idx(p_mod, &tab, idx);
}


const char *udynlink_get_module_name(const udynlink_module_t *p_mod) {
    udynlink_sym_t sym;
    udynlink_sym_t *p_sym = get_sym_at(p_mod->p_header, UDYNLINK_SYM_NAME_OFFSET, &sym);

    if (p_sym == NULL) {
        return NULL;
    } else {
        return p_sym->name;
    }
}

const char *udynlink_get_module_name_from_image(const void *base_addr) {

    const udynlink_module_header_t *p_header = (const udynlink_module_header_t*)base_addr;
    udynlink_sym_t sym;
    udynlink_sym_t *p_sym = get_sym_at(p_header, UDYNLINK_SYM_NAME_OFFSET, &sym);

    if (p_sym == NULL) {
        return NULL;
    } else {
        return p_sym->name;
    }
}

udynlink_sym_t *udynlink_lookup_symbol(const udynlink_module_t *p_mod, const char *name, udynlink_sym_t *p_sym) {
    if (p_mod == NULL)
        return NULL;

    if (p_mod->num_named_syms > 0) {
        // Binary search over named symbol entries [1, num_named_syms].
        // mkmodule emits these lexicographically sorted; local (nameless)
        // symbols come after num_named_syms and are not searchable.
        size_t lo = 1;
        size_t hi = p_mod->num_named_syms;

        while (lo <= hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (get_sym_at(p_mod->p_header, mid, p_sym) == NULL)
                break;
            int cmp = strcmp(p_sym->name, name);
            if (cmp == 0) {
                offset_sym(p_mod, p_sym);
                if (p_sym->type == UDYNLINK_SYM_TYPE_WEAK) {
                    uintptr_t sym_addr = resolve_symbol(p_mod, name);
                    if (sym_addr == UDYNLINK_SYM_DEFERRED) {
                        // Keep module's own definition
                    } else if (sym_addr > 0) {
                        p_sym->val = sym_addr;
                    }
                }
                return p_sym;
            } else if (cmp < 0) {
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        return NULL;
    }

    // Linear search fallback for old-format (unsorted) modules.
    size_t idx = 1;
    while (get_sym_at(p_mod->p_header, idx++, p_sym) != NULL) {
        if (p_sym->type != UDYNLINK_SYM_TYPE_INTERNAL && !strcmp(p_sym->name, name)) {
            offset_sym(p_mod, p_sym);
            if (p_sym->type == UDYNLINK_SYM_TYPE_WEAK) {
                uintptr_t sym_addr = resolve_symbol(p_mod, name);
                if (sym_addr == UDYNLINK_SYM_DEFERRED) {
                    // Keep module's own definition
                } else if (sym_addr > 0) {
                    p_sym->val = sym_addr;
                }
            }
            return p_sym;
        }
    }
    return NULL;
}

uintptr_t udynlink_get_symbol_value(const udynlink_module_t *p_mod, const char *name) {
    udynlink_sym_t sym;

    if (udynlink_lookup_symbol(p_mod, name, &sym) == NULL) {
        return 0;
    }
    return sym.val;
}

size_t udynlink_image_get_symbol_count(const udynlink_module_image_t *image) {
    if (image == NULL || image->p_header == NULL || image->p_symtab == NULL) {
        return 0;
    }
    return bounded_sym_count(image->p_symtab, image->p_header->symt_size);
}

const udynlink_sym_t *udynlink_image_get_symbol(const udynlink_module_image_t *image, size_t index, udynlink_sym_t *p_sym) {
    if (image == NULL || image->p_header == NULL || image->p_symtab == NULL || p_sym == NULL) {
        return NULL;
    }
    /* Pre-load: no RAM allocated, so return the raw (unrelocated) descriptor. */
    return get_sym_at_raw(image->p_symtab, index, p_sym, image->p_header->symt_size);
}

size_t udynlink_get_symbol_count(const udynlink_module_t *p_mod) {
    if (p_mod == NULL || p_mod->p_header == NULL) {
        return 0;
    }
    return bounded_sym_count(get_sym_table_pointer(p_mod->p_header), p_mod->p_header->symt_size);
}

const udynlink_sym_t *udynlink_get_symbol(const udynlink_module_t *p_mod, size_t index, udynlink_sym_t *p_sym) {
    if (p_mod == NULL || p_mod->p_header == NULL || p_sym == NULL) {
        return NULL;
    }
    if (get_sym_at(p_mod->p_header, index, p_sym) == NULL) {
        return NULL;
    }
    /* Post-load: fold in the loaded code/data base so INTERNAL/EXPORTED/WEAK
     * symbols report absolute addresses.  EXTERN/MODULE_NAME are left raw,
     * matching udynlink_lookup_symbol().  No host callback is invoked, so this
     * is side-effect-free and safe from any context after load. */
    offset_sym(p_mod, p_sym);
    return p_sym;
}

void udynlink_set_debug_level(udynlink_debug_level_t level) {
    (void)level;
}

size_t udynlink_get_image_size(const void *base_addr)
{
    if (memcmp(base_addr, "UDLM", 4))
        return 0;

    const udynlink_module_header_t *p_header = (const udynlink_module_header_t *)base_addr;

    size_t tot_size = 0;
    tot_size += get_code_offset_from_header(p_header);
    tot_size += p_header->code_size;
    tot_size += p_header->data_size;

    return tot_size;
}

size_t udynlink_get_image_size_bounded(const void *base_addr, size_t avail) {
    if ((base_addr == NULL) || (avail < sizeof(udynlink_module_header_t))) {
        return 0;
    }
    const udynlink_module_header_t *p_header = (const udynlink_module_header_t *)base_addr;
    size_t tot_size = udynlink_get_image_size(base_addr);
    if (tot_size == 0) {
        return 0; // not a module image
    }
    if (tot_size > avail) {
        return 0; // the caller's view ends inside the image's own extent
    }
    if (!is_sectioned(p_header)) {
        return tot_size; // header-only size is already exact
    }
    /* The lower bound above covers the whole metadata block, so the section
     * table (which lives inside it) is within the caller's buffer and safe to
     * read; get_sectab re-validates every field of the untrusted table. */
    sect_view_t tab;
    if (get_sectab(p_header, &tab, 0) != UDYNLINK_OK) {
        return 0;
    }
    for (size_t k = 3; k < tab.num; k++) {
        sect_entry_t e;
        sect_entry_at(&tab, k, &e);
        if (e.cls != UDYNLINK_SEC_CLASS_BSS) {
            tot_size += e.size; // payloads are concatenated in ascending VA order
        }
    }
    return tot_size;
}

uint8_t *udynlink_get_text_pointer(const udynlink_module_t *p_mod) {
    return get_code_pointer(p_mod);
}

size_t udynlink_get_ram_requirements(const void *base_addr, udynlink_load_mode_t mode) {
    const udynlink_module_header_t *p_header = (const udynlink_module_header_t *)base_addr;
    return get_ram_size_for_header(p_header, mode);
}

// Helper: resolve EXTERN relocations in a loaded module.
// If @p incremental is non-zero, only slots that are currently zero are
// resolved; otherwise all EXTERN slots are re-resolved from scratch.
static void apply_extern_relocations_impl(udynlink_module_t *p_mod, int incremental) {
    const udynlink_module_header_t *p_header = p_mod->p_header;
    const uint32_t *p_rels = get_relocs_pointer(p_mod);
    sect_view_t tab;

    // Loaded modules carry a validated table; a malformed one simply leaves
    // every sectioned write target unmapped and the walk becomes a no-op.
    (void)get_sectab(p_header, &tab, 0);

    for (size_t i = 0; i < p_header->num_rels; i++) {
        uint32_t lot_offset = *p_rels++;
        uint32_t symt_offset = *p_rels++;

        if (symt_offset & (1u << 31)) continue; // R_ARM_ABS32 data relocation
        if (symt_offset & (1u << 30)) continue; // R_ARM_TARGET1 relocation

        udynlink_sym_t sym;
        if (get_sym_at(p_header, symt_offset, &sym) == NULL) continue;
        if (sym.type != UDYNLINK_SYM_TYPE_EXTERN) continue;

        uint32_t *p_rel_location = get_reloc_target(p_mod, &tab, lot_offset);
        if (p_rel_location == NULL) continue;

        if (incremental && *p_rel_location != 0)
            continue;

        uintptr_t sym_addr = resolve_symbol(p_mod, sym.name);
        if (sym_addr == UDYNLINK_SYM_DEFERRED) {
            *p_rel_location = 0;
            continue;
        }
        if (sym_addr > 0) {
            *p_rel_location = (uint32_t)sym_addr;
        } else {
            *p_rel_location = 0; // unresolved
        }
    }
}

udynlink_error_t udynlink_link_incremental(udynlink_module_t *p_mod) {
    if (!p_mod || !p_mod->p_header) return UDYNLINK_ERR_INVALID_MODULE;
    apply_extern_relocations_impl(p_mod, 1);
    return UDYNLINK_OK;
}

udynlink_error_t udynlink_relink_all(udynlink_module_t *p_mod) {
    if (!p_mod || !p_mod->p_header) return UDYNLINK_ERR_INVALID_MODULE;
    apply_extern_relocations_impl(p_mod, 0);
    return UDYNLINK_OK;
}

udynlink_error_t udynlink_link_symbol(udynlink_module_t *p_mod, const char *sym_name, uintptr_t sym_addr) {
    if (!p_mod || !sym_name || !p_mod->p_header) return UDYNLINK_ERR_INVALID_MODULE;

    const udynlink_module_header_t *p_header = p_mod->p_header;
    const uint32_t *p_rels = get_relocs_pointer(p_mod);
    sect_view_t tab;
    (void)get_sectab(p_header, &tab, 0);
    int found = 0;

    for (size_t i = 0; i < p_header->num_rels; i++) {
        uint32_t lot_offset = *p_rels++;
        uint32_t symt_offset = *p_rels++;

        if (symt_offset & (1u << 31)) continue;
        if (symt_offset & (1u << 30)) continue;

        udynlink_sym_t sym;
        if (get_sym_at(p_header, symt_offset, &sym) == NULL) continue;
        if (sym.name == NULL || strcmp(sym.name, sym_name) != 0) continue;

        uint32_t *p_rel_location = get_reloc_target(p_mod, &tab, lot_offset);
        if (p_rel_location == NULL) continue;
        *p_rel_location = (uint32_t)sym_addr;
        found = 1;
    }

    return found ? UDYNLINK_OK : UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL;
}

int udynlink_is_symbol_resolved(const udynlink_module_t *p_mod, const char *sym_name) {
    if (!p_mod || !sym_name) return 0;
    udynlink_sym_t sym;
    if (!udynlink_lookup_symbol(p_mod, sym_name, &sym)) return 0;
    return sym.val != 0;
}
