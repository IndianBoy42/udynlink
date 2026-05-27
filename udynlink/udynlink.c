#include "udynlink.h"
#include "udynlink_externals.h"
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

__attribute__((weak))
uintptr_t udynlink_external_resolve_symbol(const char *name) {
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

static size_t get_code_offset_from_header(const udynlink_module_header_t *p_header) {
    size_t res = get_header_size(p_header) + p_header->num_rels * 2 * sizeof(uint32_t) + p_header->symt_size;
    res = (res + 3) & ~3U;
    return res;
}

// Gets the address of the code
static uint8_t *get_code_pointer(const udynlink_module_t *p_mod) {
    const udynlink_module_header_t *p_header = p_mod->p_header;

    if (UDYNLINK_LOAD_GET_MODE(p_mod) == UDYNLINK_LOAD_MODE_COPY_TEXT_DATA) { // the code is after the LOT in RAM.
        return (uint8_t*)p_mod->p_ram + p_header->num_lot * sizeof(uint32_t);// the code is after the LOT in RAM.
    } else { // the code is after the module header, the relocations and the symbol table
        return (uint8_t*)p_header + get_code_offset_from_header(p_header);
    }
}

// Gets the address of the data section (in RAM)
static uint8_t *get_data_pointer(const udynlink_module_t *p_mod) {
    const udynlink_module_header_t *p_header = p_mod->p_header;

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

// Gets the pointer to the symbol table according to the given module header
static const uint32_t *get_sym_table_pointer(const udynlink_module_header_t *p_header) {
    return (uint32_t*)p_header + get_header_size(p_header) / sizeof(uint32_t) + p_header->num_rels * 2;
}

// Return a pointer to the relocation data (after the header)
static const uint32_t *get_relocs_pointer(const udynlink_module_t *p_mod) {
    const udynlink_module_header_t *p_header = p_mod->p_header;

    return (const uint32_t*)p_header + get_header_size(p_header) / sizeof(uint32_t);
}

// Compute num_named_syms for the given symbol table base.
// Returns the index of the last contiguous named (non-INTERNAL) entry starting
// from index 1, provided that (a) all named entries come before any INTERNAL
// entries, and (b) the named entries are sorted lexicographically.  Returns 0
// for old-format (unsorted) modules, which signals udynlink_lookup_symbol to
// fall back to linear search.
static uint16_t compute_num_named_syms_raw(const uint32_t *p_symt) {
    uint32_t num_entries = *p_symt;
    uint16_t last_named = 0;
    uint8_t found_local = 0;

    for (size_t i = 1; i < num_entries; i++) {
        uint32_t name_off = p_symt[i * 2 + 1];
        uint8_t sym_type = (name_off >> UDYNLINK_SYM_INFO_SHIFT) & UDYNLINK_SYM_INFO_TYPE_MASK;
        if (sym_type == UDYNLINK_SYM_TYPE_INTERNAL) {
            found_local = 1;
        } else {
            if (found_local) return 0;
            last_named = (uint16_t)i;
        }
    }

    for (size_t i = 2; i <= last_named; i++) {
        const char *prev = (const char*)p_symt + (p_symt[(i - 1) * 2 + 1] & UDYNLINK_SYM_OFFSET_MASK);
        const char *cur  = (const char*)p_symt + (p_symt[i * 2 + 1] & UDYNLINK_SYM_OFFSET_MASK);
        if (strcmp(prev, cur) > 0) return 0;
    }

    return last_named;
}

static uint16_t compute_num_named_syms(const udynlink_module_header_t *p_header) {
    return compute_num_named_syms_raw(get_sym_table_pointer(p_header));
}

////////////////////////////////////////////////////////////////////////////////
// Helpers - various

// Marks the given module as "free" by zeroing its data structure
static void mark_module_free(udynlink_module_t *p_mod) {
    memset(p_mod, 0, sizeof(udynlink_module_t));
}

// Return the entry with the specified index in the given symbol table
// Returns "p_sym" if OK, NULL if index is out of range or an error occurred
static udynlink_sym_t *get_sym_at_raw(const uint32_t *p_symt, size_t index, udynlink_sym_t *p_sym) {
    uint32_t name_off;
    uint32_t info;

    if (index >= *p_symt) { // first word in the symbol table is the number of entries
        return NULL;
    }
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
        p_sym->name = (const char*)p_symt + (name_off & UDYNLINK_SYM_OFFSET_MASK);
    } else {
        p_sym->name = "(N/A)";
    }
    return p_sym;
}

static udynlink_sym_t *get_sym_at(const udynlink_module_header_t *p_header, size_t index, udynlink_sym_t *p_sym) {
    return get_sym_at_raw(get_sym_table_pointer(p_header), index, p_sym);
}

// Offset the given symbol relative to the required base address (.code or .data), based on the symbol location
// The function returns p_sym after it applies the offset to p_sym->val.
static udynlink_sym_t *offset_sym(const udynlink_module_t *p_mod, udynlink_sym_t *p_sym) {
    uintptr_t prev_val = p_sym->val;

    // Weak symbols are initially offset like internal/exported symbols so the
    // module's own definition is the default.  If the host provides an override
    // the loader patches the LOT/data entry afterwards.
    if ((p_sym->type == UDYNLINK_SYM_TYPE_INTERNAL) || (p_sym->type == UDYNLINK_SYM_TYPE_EXPORTED) || (p_sym->type == UDYNLINK_SYM_TYPE_WEAK)) {
        if (p_sym->location == UDYNLINK_SYM_LOCATION_CODE) {
            p_sym->val += (uintptr_t)get_code_pointer(p_mod);
        } else {
            p_sym->val += (uintptr_t)get_data_pointer(p_mod);
        }
    }
    UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Symbol %s relocated relative to %s, orig value is %08X, new value is %08X\n", p_sym->name, p_sym->location == UDYNLINK_SYM_LOCATION_CODE ? "code" : "data", (uint32_t)prev_val, (uint32_t)p_sym->val);
    return p_sym;
}

////////////////////////////////////////////////////////////////////////////////
// Helpers - RAM size

static size_t get_ram_size_for_header(const udynlink_module_header_t *p_header, udynlink_load_mode_t load_mode) {
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
static uintptr_t resolve_symbol(const char *name) {
    uintptr_t sym_addr = udynlink_external_resolve_symbol(name);
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
    uint32_t *p_data = (uint32_t *)get_data_pointer(p_mod);
    udynlink_sym_t sym;
    udynlink_error_t res = UDYNLINK_OK;

    UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "LOT base: %p, .data starts at %p, .code starts at %p\n", p_lot, p_data, get_code_pointer(p_mod));

    for (size_t i = 0; i < p_header->num_rels; i++) {
        uint32_t lot_offset = p_relocations[i * 2];
        uint32_t symt_offset = p_relocations[i * 2 + 1];

        if (symt_offset & (1u << 31)) {
            // https://stackoverflow.com/questions/75558729/position-independent-code-gcc-versus-armcc
            // R_ARM_ABS32 data relocation
            // *offset += &data - value
            uint32_t *p = p_data + (lot_offset - p_header->num_lot);
            *p += (uint32_t)(uintptr_t)p_data - (symt_offset & 0x7FFFFFFF);
            continue;
        }

        if (symt_offset & (1u << 30)) {
            uint32_t *p = p_data + (lot_offset - p_header->num_lot);
            *p = ((uint32_t)(uintptr_t)get_code_pointer(p_mod) + *p);
            continue;
        }

        if (get_sym_at_raw(p_symtab, symt_offset, &sym) == NULL) { // symbol table offset is out of range, shouldn't happen
            res = UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE;
            goto exit;
        }

        // Relocations in LOT and .data are encoded in the same way, they can be differentiated based on the value of lot_offset.
        // If lot_offset is larger than or equal to the number of LOT entries, this relocation applies to data, not to LOT.
        uint32_t *p_rel_location = (lot_offset < p_header->num_lot) ? p_lot + lot_offset : p_data + lot_offset - p_header->num_lot;

        switch (sym.type) {
            case UDYNLINK_SYM_TYPE_INTERNAL:
            case UDYNLINK_SYM_TYPE_EXPORTED:
                UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Applying relocation for symbol at index %u, name=%s, type=%d, data_reloc=%d at lot_offset=%u, value=%08X\n", symt_offset, sym.name, sym.type, sym.location, lot_offset, sym.val);
                *p_rel_location = offset_sym(p_mod, &sym)->val;
                break;

            case UDYNLINK_SYM_TYPE_WEAK:
                // Write the module's own address first (default fallback), then
                // try host override.  Unlike EXTERN, failure to resolve a weak
                // symbol is not fatal — the module definition remains.
                UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Applying weak relocation for symbol at index %u, name=%s at lot_offset=%u\n", symt_offset, sym.name, lot_offset);
                *p_rel_location = offset_sym(p_mod, &sym)->val;
                {
                    uintptr_t sym_addr = resolve_symbol(sym.name);
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
                    uintptr_t sym_addr = resolve_symbol(sym.name);
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
    udynlink_sym_t *p_sym = get_sym_at_raw(p_symtab, UDYNLINK_SYM_NAME_OFFSET, &sym);
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
    UDYNLINK_LOAD_SET_MODE(p_mod, load_mode);

    res = udynlink_validate_header(p_header);
    if (res != UDYNLINK_OK)
        goto exit;

    UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Processing module image named '%s' with load mode %d\n", udynlink_image_get_module_name(image->p_symtab), (int)load_mode);

    // Allocate RAM or check given RAM region, as needed
    size_t ram_size = udynlink_compute_ram_size(p_header, load_mode);
    if (ram_size > 0) {
        if (load_addr == NULL) {
            UDYNLINK_LOAD_CLR_FOREIGN_RAM(p_mod);
            if ((ram_addr = udynlink_external_malloc(ram_size)) == NULL) {
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
        p_mod->p_ram = ram_addr;
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "RAM area for module is at %p (%u bytes)\n", ram_addr, ram_size);
    } else {
        p_mod->p_ram = NULL;
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Awesome! Module doesn't need any RAM\n");
    }

    // Copy to RAM as needed
    uint8_t *p_temp8 = (uint8_t *)ram_addr + p_header->num_lot * sizeof(uint32_t);
    size_t code_offset = get_code_offset_from_header(p_header);

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
        // Copy code and data only; metadata stays in source
        memcpy(p_temp8, image->p_code, p_header->code_size + p_header->data_size);
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Copied code and data to RAM at %p (%u bytes)\n", p_temp8, p_header->code_size + p_header->data_size);
    } else {
        // XIP: copy only data
        memcpy(p_temp8, image->p_data, p_header->data_size);
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Copied data to RAM at %p (%u bytes)\n", p_temp8, p_header->data_size);
    }

    // Zero out BSS
    memset(get_data_pointer(p_mod) + p_header->data_size, 0, p_header->bss_size);

    // Process relocations
    res = udynlink_load_apply_relocations(p_mod, p_header, image->p_relocations, image->p_symtab);
    if (res != UDYNLINK_OK)
        goto exit;

    p_mod->num_named_syms = compute_num_named_syms(p_mod->p_header);

    UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Done loading module image\n");

exit:
    if (res != UDYNLINK_OK) {
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, error_codes[(int)res]);
        if ((p_mod->p_ram != NULL) && !UDYNLINK_LOAD_IS_FOREIGN_RAM(p_mod)) {
            udynlink_external_free(p_mod->p_ram);
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

void udynlink_cpp_init(udynlink_module_t *p_mod){
    udynlink_sym_t __init_array= {};
    if(udynlink_lookup_symbol(p_mod, "__init_array", &__init_array) != NULL)
    {
        UDYNLINK_PREPARE_CALL(p_mod);
        typedef void (*void_func)(void);
        void_func f = (void_func)__init_array.val;
        f();
    }   
}

udynlink_error_t udynlink_unload_module(udynlink_module_t *p_mod) {
    if ((p_mod == NULL) || (p_mod->p_header == NULL)) {
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, error_codes[(int)UDYNLINK_ERR_INVALID_MODULE]);
        return UDYNLINK_ERR_INVALID_MODULE;
    }
    UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Unloading module at %p\n", p_mod);
    if ((p_mod->p_ram != NULL) && !UDYNLINK_LOAD_IS_FOREIGN_RAM(p_mod)) {
        udynlink_external_free(p_mod->p_ram);
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Deallocated memory area at %p\n", p_mod->p_ram);
    }
    mark_module_free(p_mod);
    return UDYNLINK_OK;
}

const char *udynlink_error_msg(udynlink_error_t* err) {
    return error_codes[(int)(intptr_t)err];
}

size_t udynlink_get_ram_size(const udynlink_module_t *p_mod) {
    const udynlink_module_header_t *p_header = p_mod->p_header;
    udynlink_load_mode_t load_mode = UDYNLINK_LOAD_GET_MODE(p_mod);

    // RAM is always needed for relocations, .data and .bss section
    size_t tot_size = p_header->num_lot * sizeof(uint32_t) + p_header->data_size + p_header->bss_size;
    // Depending on the copy mode, more RAM might be needed:
    // - if only code is copied, add size of the code
    // - if everything is copied, add the size of the header (including the symbol table and the relocations) and the code
    if (load_mode == UDYNLINK_LOAD_MODE_COPY_TEXT_DATA) {
        tot_size += p_header->code_size;
    }
    else if (load_mode == UDYNLINK_LOAD_MODE_COPY_ALL) {
        tot_size += get_code_offset_from_header(p_header) + p_header->code_size;
    }
    return tot_size;
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
                    uintptr_t sym_addr = resolve_symbol(name);
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
                uintptr_t sym_addr = resolve_symbol(name);
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
    uint32_t *p_lot = (uint32_t *)p_mod->p_ram;
    uint32_t *p_data = (uint32_t *)get_data_pointer(p_mod);

    for (size_t i = 0; i < p_header->num_rels; i++) {
        uint32_t lot_offset = *p_rels++;
        uint32_t symt_offset = *p_rels++;

        if (symt_offset & (1u << 31)) continue; // R_ARM_ABS32 data relocation
        if (symt_offset & (1u << 30)) continue; // R_ARM_TARGET1 relocation

        udynlink_sym_t sym;
        if (get_sym_at(p_header, symt_offset, &sym) == NULL) continue;
        if (sym.type != UDYNLINK_SYM_TYPE_EXTERN) continue;

        uint32_t *p_rel_location = (lot_offset < p_header->num_lot) ?
            p_lot + lot_offset : p_data + lot_offset - p_header->num_lot;

        if (incremental && *p_rel_location != 0)
            continue;

        uintptr_t sym_addr = resolve_symbol(sym.name);
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
    uint32_t *p_lot = (uint32_t *)p_mod->p_ram;
    uint32_t *p_data = (uint32_t *)get_data_pointer(p_mod);
    int found = 0;

    for (size_t i = 0; i < p_header->num_rels; i++) {
        uint32_t lot_offset = *p_rels++;
        uint32_t symt_offset = *p_rels++;

        if (symt_offset & (1u << 31)) continue;
        if (symt_offset & (1u << 30)) continue;

        udynlink_sym_t sym;
        if (get_sym_at(p_header, symt_offset, &sym) == NULL) continue;
        if (sym.name == NULL || strcmp(sym.name, sym_name) != 0) continue;

        uint32_t *p_rel_location = (lot_offset < p_header->num_lot) ?
            p_lot + lot_offset : p_data + lot_offset - p_header->num_lot;
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
