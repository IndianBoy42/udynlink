#include "udynlink.h"
#include "udynlink_externals.h"
#include <string.h>
#include <stdint.h>
#include <stdarg.h>


////////////////////////////////////////////////////////////////////////////////
// Local macros and data

#define UDYNLINK_MODULE_SIGN                  (((uint32_t)'M' << 24) | ((uint32_t)'L' << 16) | ((uint32_t)'D' << 8) | (uint32_t)'U')

static udynlink_debug_level_t debug_level;

#define _UDYNLINK_EXPAND(x)                   #x"\n"
static const char * const error_codes[] = {
    UDYNLINK_ERROR_CODES
};
#undef _UDYNLINK_EXPAND

// Symbol table masks and data
#define UDYNLINK_SYM_OFFSET_MASK              0x0FFFFFFF
#define UDYNLINK_SYM_INFO_SHIFT               28
#define UDYNLINK_SYM_INFO_CODE_MASK           0x04
#define UDYNLINK_SYM_INFO_TYPE_MASK           0x03
#define UDYNLINK_SYM_NAME_OFFSET              0

// Module structure masks
#define UDYNLINK_LOAD_MODE_MASK               (uint8_t)0x03
#define UDYNLINK_LOAD_FOREIGN_RAM_MASK        (uint8_t)0x04
#define UDYNLINK_LOAD_GET_MODE(p_mod)         (udynlink_load_mode_t)(p_mod->info & UDYNLINK_LOAD_MODE_MASK)
#define UDYNLINK_LOAD_SET_MODE(p_mod, m)      p_mod->info = (p_mod->info & (uint8_t)~UDYNLINK_LOAD_MODE_MASK) | ((uint8_t)m)
#define UDYNLINK_LOAD_IS_FOREIGN_RAM(p_mod)   ((p_mod->info & UDYNLINK_LOAD_FOREIGN_RAM_MASK) != 0)
#define UDYNLINK_LOAD_SET_FOREIGN_RAM(p_mod)  p_mod->info |= UDYNLINK_LOAD_FOREIGN_RAM_MASK
#define UDYNLINK_LOAD_CLR_FOREIGN_RAM(p_mod)  p_mod->info &= (uint8_t)~UDYNLINK_LOAD_FOREIGN_RAM_MASK

#define UDYNLINK_LOAD_STREAM_HDR_MASK        (uint8_t)0x08
#define UDYNLINK_LOAD_IS_STREAM_HDR(p_mod)   ((p_mod->info & UDYNLINK_LOAD_STREAM_HDR_MASK) != 0)
#define UDYNLINK_LOAD_SET_STREAM_HDR(p_mod)   p_mod->info |= UDYNLINK_LOAD_STREAM_HDR_MASK
#define UDYNLINK_LOAD_CLR_STREAM_HDR(p_mod)   p_mod->info &= (uint8_t)~UDYNLINK_LOAD_STREAM_HDR_MASK

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
    // If the array below is modified, remember to also modify the "udynlink_debug_level_t" enum in the header!
    static const char * const names[] = {"n/a", "error", "warning", "info"};

    if ((int)level > (int)debug_level) {
        return;
    }
    internal_printf("[udynlink %s in function %s, line %d] ", names[(int)level], func, line);
    va_start(va, msg);
    udynlink_external_vprintf(msg, va);
    va_end(va);
}

////////////////////////////////////////////////////////////////////////////////
// Helpers - offsets and addresses

// Returns the offset of code from the given module header address
// The code comes after the header, the relocations and the symbol table.
static uint32_t get_header_size(const udynlink_module_header_t *p_header) {
    if (p_header->udynlink_version < UDYNLINK_MAKE_VERSION(2, 0))
        return 32;
    return sizeof(udynlink_module_header_t);
}

static uint32_t get_deps_strtab_offset(const udynlink_module_header_t *p_header) {
    return get_header_size(p_header) + p_header->num_rels * 2 * sizeof(uint32_t) + p_header->symt_size;
}

static const char *get_deps_strtab(const udynlink_module_header_t *p_header) {
    if (p_header->udynlink_version < UDYNLINK_MAKE_VERSION(2, 0) || p_header->num_deps == 0)
        return NULL;
    return (const char *)p_header + get_deps_strtab_offset(p_header);
}

static uint32_t get_code_offset_from_header(const udynlink_module_header_t *p_header) {
    uint32_t res = get_deps_strtab_offset(p_header);
    if (p_header->udynlink_version >= UDYNLINK_MAKE_VERSION(2, 0)) {
        res += p_header->deps_strtab_size;
        res = (res + 3) & ~3U;
    }
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

////////////////////////////////////////////////////////////////////////////////
// Helpers - various

// Marks the given module as "free" by zeroing its data structure
static void mark_module_free(udynlink_module_t *p_mod) {
    memset(p_mod, 0, sizeof(udynlink_module_t));
}

// Return the entry with the specified index in the given symbol table
// Returns "p_sym" if OK, NULL if index is out of range or an error occured
static udynlink_sym_t *get_sym_at(const udynlink_module_header_t *p_header, uint32_t index, udynlink_sym_t *p_sym) {
    uint32_t name_off, info;
    const uint32_t *p_symt = get_sym_table_pointer(p_header);

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

// Offset the given symbol relative to the required base address (.code or .data), based on the symbol location
// The function returns p_sym after it applies the offset to p_sym->val.
static udynlink_sym_t *offset_sym(const udynlink_module_t *p_mod, udynlink_sym_t *p_sym) {
    uint32_t prev_val = p_sym->val;

    if ((p_sym->type == UDYNLINK_SYM_TYPE_INTERNAL) || (p_sym->type == UDYNLINK_SYM_TYPE_EXPORTED)) {
        if (p_sym->location == UDYNLINK_SYM_LOCATION_CODE) {
            p_sym->val += (uint32_t)(uintptr_t)get_code_pointer(p_mod);
        } else {
            p_sym->val += (uint32_t)(uintptr_t)get_data_pointer(p_mod);
        }
    }
    UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Symbol %s relocated relative to %s, orig value is %08X, new value is %08X\n", p_sym->name, p_sym->location == UDYNLINK_SYM_LOCATION_CODE ? "code" : "data", prev_val, p_sym->val);
    return p_sym;
}

////////////////////////////////////////////////////////////////////////////////
// Helpers - various (continued)

static uint32_t get_ram_size_for_header(const udynlink_module_header_t *p_header, udynlink_load_mode_t load_mode) {
    uint32_t tot_size = p_header->num_lot * sizeof(uint32_t) + p_header->data_size + p_header->bss_size;
    if (load_mode == UDYNLINK_LOAD_MODE_COPY_TEXT_DATA) {
        tot_size += p_header->code_size;
    } else if (load_mode == UDYNLINK_LOAD_MODE_COPY_ALL) {
        tot_size += get_code_offset_from_header(p_header) + p_header->code_size;
    }
    return tot_size;
}

////////////////////////////////////////////////////////////////////////////////
// Streaming I/O helpers

static int32_t stream_read_exact(const udynlink_io_t *p_io, void *dest,
                                 uint32_t offset, uint32_t len,
                                 void *work_buf, uint32_t work_buf_size) {
    uint8_t *d = (uint8_t *)dest;
    uint32_t pos = 0;
    while (pos < len) {
        uint32_t chunk = len - pos;
        if (chunk > work_buf_size) chunk = work_buf_size;
        int32_t n = p_io->read(p_io->pv_ctx, work_buf, chunk, offset + pos);
        if (n < 0 || (uint32_t)n != chunk) return -1;
        memcpy(d + pos, work_buf, chunk);
        pos += chunk;
    }
    return (int32_t)len;
}

static int32_t stream_read_string(const udynlink_io_t *p_io, char *dest,
                                  uint32_t offset, uint32_t max_len,
                                  void *work_buf, uint32_t work_buf_size) {
    uint32_t total_read = 0;
    uint32_t cur_offset = offset;
    while (total_read + 1 < max_len) {
        uint32_t chunk = work_buf_size;
        if (chunk > max_len - total_read - 1) chunk = max_len - total_read - 1;
        int32_t n = p_io->read(p_io->pv_ctx, work_buf, chunk, cur_offset);
        if (n <= 0) return -1;
        const uint8_t *src = (const uint8_t *)work_buf;
        for (int32_t i = 0; i < n; i++) {
            dest[total_read++] = (char)src[i];
            if (src[i] == '\0') return (int32_t)total_read;
        }
        cur_offset += (uint32_t)n;
        if ((uint32_t)n < chunk) return -1;
    }
    dest[total_read] = '\0';
    return -1;
}

////////////////////////////////////////////////////////////////////////////////
// Public interface

udynlink_error_t udynlink_load_module(udynlink_module_t *p_mod, const void *base_addr, void *load_addr, uint32_t load_size, udynlink_load_mode_t load_mode) {
    void *ram_addr = NULL;
    udynlink_error_t res = UDYNLINK_OK;
    const udynlink_module_header_t *p_header = (const udynlink_module_header_t*)base_addr;
    udynlink_sym_t sym;

    if(!p_mod)
        return UDYNLINK_ERR_INVALID_MODULE;

    // Setup the module structure. Depending on the copy mode, we might need to rewrite it later.
    p_mod->p_header = p_header;
    UDYNLINK_LOAD_SET_MODE(p_mod, load_mode);

    // Check signature
    if (p_header->sign != UDYNLINK_MODULE_SIGN) {
        res = UDYNLINK_ERR_LOAD_INVALID_SIGN;
        goto exit;
    }

    // Check loader ABI version
    if (p_header->udynlink_version > UDYNLINK_LOADER_ABI_VERSION) {
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, "Module udynlink version %d.%d > loader version %d.%d\n",
            UDYNLINK_GET_MAJOR_VERSION(p_header->udynlink_version), UDYNLINK_GET_MINOR_VERSION(p_header->udynlink_version),
            UDYNLINK_GET_MAJOR_VERSION(UDYNLINK_LOADER_ABI_VERSION), UDYNLINK_GET_MINOR_VERSION(UDYNLINK_LOADER_ABI_VERSION));
        res = UDYNLINK_ERR_LOAD_VERSION_MISMATCH;
        goto exit;
    }

    // Check architecture tag compatibility
    {
        uint16_t host_arch = UDYNLINK_HOST_ARCH_TAG;
        uint16_t mod_arch = p_header->arch_tag;
        if ((mod_arch & UDYNLINK_ARCH_FAMILY_MASK) != (host_arch & UDYNLINK_ARCH_FAMILY_MASK)) {
            UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, "Module architecture family mismatch (mod=0x%04X, host=0x%04X)\n", mod_arch, host_arch);
            res = UDYNLINK_ERR_LOAD_ARCH_MISMATCH;
            goto exit;
        }
        uint16_t mod_float = (mod_arch >> UDYNLINK_ARCH_FLOAT_ABI_SHIFT) & 0x03;
        uint16_t host_float = (host_arch >> UDYNLINK_ARCH_FLOAT_ABI_SHIFT) & 0x03;
        if (mod_float == UDYNLINK_ARCH_FLOAT_ABI_HARD && host_float != UDYNLINK_ARCH_FLOAT_ABI_HARD) {
            UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, "Module requires hard-float, host has soft-float\n");
            res = UDYNLINK_ERR_LOAD_ARCH_MISMATCH;
            goto exit;
        }
        if (mod_float == UDYNLINK_ARCH_FLOAT_ABI_SOFTFP && host_float == UDYNLINK_ARCH_FLOAT_ABI_SOFT) {
            UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, "Module requires softfp, host has soft-float\n");
            res = UDYNLINK_ERR_LOAD_ARCH_MISMATCH;
            goto exit;
        }
    }

    UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Processing module at %p named '%s' with load mode %d\n", base_addr, udynlink_get_module_name(p_mod), (int)load_mode);

    // Dependency validation (v2.0+ modules only)
    p_mod->num_deps = 0;
    p_mod->dep_refcount = 0;
    if (p_header->udynlink_version >= UDYNLINK_MAKE_VERSION(2, 0) && p_header->num_deps > 0) {
        if (p_header->num_deps > UDYNLINK_MAX_DEPS) {
            UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, "Module has %u dependencies, max is %u\n", p_header->num_deps, UDYNLINK_MAX_DEPS);
            res = UDYNLINK_ERR_LOAD_MISSING_DEP;
            goto exit;
        }
        const char *dep_str = get_deps_strtab(p_header);
        if (dep_str == NULL && p_header->num_deps > 0) {
            res = UDYNLINK_ERR_LOAD_MISSING_DEP;
            goto exit;
        }
        for (uint16_t d = 0; d < p_header->num_deps; d++) {
            if (dep_str == NULL || *dep_str == '\0') {
                UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, "Missing dependency name at index %u\n", d);
                res = UDYNLINK_ERR_LOAD_MISSING_DEP;
                goto exit;
            }
            udynlink_module_t *dep_mod = udynlink_external_get_module_handle(dep_str);
            if (dep_mod == NULL) {
                UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, "Dependency '%s' not found\n", dep_str);
                res = UDYNLINK_ERR_LOAD_MISSING_DEP;
                goto exit;
            }
            p_mod->deps[d] = dep_mod;
            dep_mod->dep_refcount++;
            p_mod->num_deps++;
            dep_str += strlen(dep_str) + 1;
        }
    }

    // Allocate RAM or check given RAM region, as needed
    uint32_t ram_size = udynlink_get_ram_size(p_mod);
    if (ram_size > 0) { // is any RAM needed at all?
        if (load_addr == NULL) { // RAM must be allocated
            UDYNLINK_LOAD_CLR_FOREIGN_RAM(p_mod);
            if ((ram_addr = udynlink_external_malloc(ram_size)) == NULL) {
                res = UDYNLINK_ERR_LOAD_OUT_OF_MEMORY;
                goto exit;
            }
            UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Allocated %u bytes for module at %p\n", ram_size, base_addr);
        } else { // check if the user-provided RAM region is large enough
            UDYNLINK_LOAD_SET_FOREIGN_RAM(p_mod);
            if (load_size < ram_size) {
                res = UDYNLINK_ERR_LOAD_RAM_LEN_LOW;
                goto exit;
            }
            ram_addr = load_addr;
        }
        p_mod->p_ram = ram_addr;
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "RAM area for module at %p is at %p (%u bytes)\n", base_addr, ram_addr, ram_size);
    } else {
        p_mod->p_ram = NULL;
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Awesome! Module %p doesn't need any RAM\n", base_addr);
    }

    // Copy to RAM as needed. The first part of RAM is always the LOT, so skip it.
    uint8_t *p_temp8 = (uint8_t*)ram_addr + p_header->num_lot * sizeof(uint32_t);
    // Reuse "load_size" (since it's not used anymore) to hold the offset to code, according to the header.
    load_size = get_code_offset_from_header(p_header);
    if (load_mode == UDYNLINK_LOAD_MODE_COPY_ALL) {
        // We need to copy the whole module to RAM (header, symbol table, relocs, code, data)
        memcpy(p_temp8, base_addr, load_size + p_header->code_size + p_header->data_size);
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Copied module at %p to RAM at %p (%u bytes)\n", base_addr, p_temp8, load_size + p_header->code_size + p_header->data_size);
        // Since we copied everything, move the pointer to the header to RAM, since the original (base_addr) might be freed eventually.
        p_mod->p_header = p_header = (const udynlink_module_header_t*)p_temp8;
    } else if (load_mode == UDYNLINK_LOAD_MODE_COPY_TEXT_DATA) {
        // Copy just code and data
        memcpy(p_temp8, (const uint8_t*)base_addr + load_size, p_header->code_size + p_header->data_size);
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Copied code and data of module %p to RAM at %p (%u bytes)\n", base_addr, p_temp8, p_header->code_size + p_header->data_size);
    } else {
        // XIP mode: copy only data
        memcpy(p_temp8, (const uint8_t*)base_addr + load_size + p_header->code_size, p_header->data_size);
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Copied data of module %p to RAM at %p (%u bytes)\n", base_addr, p_temp8, p_header->data_size);
    }

    // Zero out BSS
    memset(get_data_pointer(p_mod) + p_header->data_size, 0, p_header->bss_size);

    // Process relocations
    // TODO: find the correct condition for the error "unable to execute in place"
    const uint32_t *p_rels = get_relocs_pointer(p_mod);
    uint32_t *p_lot = (uint32_t*)ram_addr;
    uint32_t *p_data = (uint32_t*)get_data_pointer(p_mod);
    UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "LOT base: %p, .data starts at %p, .code starts at %p\n", p_lot, p_data, get_code_pointer(p_mod));
    // Read and apply each (lot_offset, symt_offset) pair in turn
    for (uint32_t i = 0; i < p_header->num_rels; i ++) {
        uint32_t lot_offset = *p_rels ++;
        uint32_t symt_offset = *p_rels ++;

        if(symt_offset & (1 << 31)) {
            // https://stackoverflow.com/questions/75558729/position-independent-code-gcc-versus-armcc
            // R_ARM_ABS32 data relocation
            // *offset += &data - value
            uint32_t *p = p_data + (lot_offset - p_header->num_lot);
            *p += (uint32_t)(uintptr_t)p_data - (symt_offset & 0x7FFFFFFF);
            continue;
        }

        if(symt_offset & (1 << 30)) {
            uint32_t *p = p_data + (lot_offset - p_header->num_lot);
            *p = ((uint32_t)(uintptr_t)get_code_pointer(p_mod) + (uint32_t)*p);
            continue;
        }

        if (get_sym_at(p_mod->p_header, symt_offset, &sym) == NULL) { // symbol table offset is out of range, shouldn't happen
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

            case UDYNLINK_SYM_TYPE_EXTERN:
                UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Applying extern relocation for symbol at index %u, name=%s at lot_offset=%u\n", symt_offset, sym.name, lot_offset);
                {
                    uint32_t sym_addr = udynlink_external_resolve_critical_symbol(sym.name);
                    if (sym_addr == 0) {
                        for (uint8_t d = 0; d < p_mod->num_deps; d++) {
                            udynlink_sym_t dep_sym;
                            if (udynlink_lookup_symbol(p_mod->deps[d], sym.name, &dep_sym) != NULL) {
                                sym_addr = dep_sym.val;
                                break;
                            }
                        }
                    }
                    if (sym_addr == 0) {
                        sym_addr = udynlink_external_resolve_symbol(sym.name);
                    }
                    if (sym_addr > 0) {
                        *p_rel_location = sym_addr;
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

    // All done
    UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Done loading module at %p\n", base_addr);

exit:
    if (res != UDYNLINK_OK) { // there's an error, so cleanup allocated structures and memory
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, error_codes[(int)res]);
        if ((p_mod->p_ram != NULL) && !UDYNLINK_LOAD_IS_FOREIGN_RAM(p_mod)) { // free allocated memory
            udynlink_external_free(p_mod->p_ram);
            UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Deallocated memory area at %p\n", p_mod->p_ram);
        }
        if (p_mod != NULL) { // mark entry in module table as "free"
            mark_module_free(p_mod);
        }
    }
    return res;
}

void udynlink_cpp_init(udynlink_module_t *p_mod){
    udynlink_sym_t __init_array= {};
    if(udynlink_lookup_symbol(p_mod, "__init_array", &__init_array) != NULL)
    {
        uint32_t* mod_base = (uint32_t*)UDYNLINK_LOT_BASE_ADDR;
        *mod_base = p_mod->ram_base;
        typedef void (*void_func)(void);
        void_func f = (void_func)(uintptr_t)__init_array.val;
        f();
    }   
}

udynlink_error_t udynlink_unload_module(udynlink_module_t *p_mod) {
    if ((p_mod == NULL) || (p_mod->p_header == NULL)) {
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, error_codes[(int)UDYNLINK_ERR_INVALID_MODULE]);
        return UDYNLINK_ERR_INVALID_MODULE;
    }
    if (p_mod->dep_refcount > 0) {
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, "Cannot unload module: %u other modules depend on it\n", p_mod->dep_refcount);
        return UDYNLINK_ERR_MODULE_HAS_DEPENDENTS;
    }
    UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Unloading module at %p\n", p_mod);
    for (uint8_t i = 0; i < p_mod->num_deps; i++) {
        if (p_mod->deps[i] != NULL) {
            ((udynlink_module_t *)p_mod->deps[i])->dep_refcount--;
        }
    }
    if ((p_mod->p_ram != NULL) && !UDYNLINK_LOAD_IS_FOREIGN_RAM(p_mod)) {
        void *p_free = UDYNLINK_LOAD_IS_STREAM_HDR(p_mod) ? (void *)p_mod->p_header : p_mod->p_ram;
        udynlink_external_free(p_free);
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Deallocated memory area at %p\n", p_free);
    }
    mark_module_free(p_mod);
    return UDYNLINK_OK;
}

const char *udynlink_error_msg(udynlink_error_t* err) {
    return error_codes[(int)(intptr_t)err];
}

uint32_t udynlink_get_ram_size(const udynlink_module_t *p_mod) {
    const udynlink_module_header_t *p_header = p_mod->p_header;
    udynlink_load_mode_t load_mode = UDYNLINK_LOAD_GET_MODE(p_mod);

    // RAM is always needed for relocations, .data and .bss section
    uint32_t tot_size = p_header->num_lot * sizeof(uint32_t) + p_header->data_size + p_header->bss_size;
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
    uint32_t idx;

    if (p_mod != NULL) { // but consider only the given one if not NULL
        idx = 0;
        while (get_sym_at(p_mod->p_header, idx ++, p_sym) != NULL) { // iterate through module's symbol table
            if (!strcmp(p_sym->name, name)) { // symbol found
                return offset_sym(p_mod, p_sym); // offset value properly before returning
            }
        }
    }
    return NULL;
}

uint32_t udynlink_get_symbol_value(const udynlink_module_t *p_mod, const char *name) {
    udynlink_sym_t sym;

    if (udynlink_lookup_symbol(p_mod, name, &sym) == NULL) {
        return 0;
    }
    return sym.val;
}

void udynlink_set_debug_level(udynlink_debug_level_t level) {
    debug_level = level;
}

uint32_t udynlink_get_image_size(const void *base_addr)
{
    if (memcmp(base_addr, "UDLM", 4))
        return 0;

    const udynlink_module_header_t *p_header = (const udynlink_module_header_t *)base_addr;

    uint32_t tot_size = 0;
    tot_size += get_code_offset_from_header(p_header);
    tot_size += p_header->code_size;
    tot_size += p_header->data_size;

    return tot_size;
}

uint8_t *udynlink_get_text_pointer(const udynlink_module_t *p_mod) {
    return get_code_pointer(p_mod);
}

////////////////////////////////////////////////////////////////////////////////
// Streaming I/O public interface

udynlink_error_t udynlink_load_module_from_stream(udynlink_module_t *p_mod,
    const udynlink_io_t *p_io, void *load_addr, uint32_t load_size,
    udynlink_load_mode_t load_mode, void *work_buf, uint32_t work_buf_size) {

    void *ram_addr = NULL;
    udynlink_error_t res = UDYNLINK_OK;
    udynlink_module_header_t header;

    if (!p_mod) return UDYNLINK_ERR_INVALID_MODULE;
    if (load_mode == UDYNLINK_LOAD_MODE_XIP) return UDYNLINK_ERR_LOAD_XIP_UNSUPPORTED;
    if (work_buf == NULL || work_buf_size < UDYNLINK_STREAM_MIN_WORK_BUF_SIZE)
        return UDYNLINK_ERR_LOAD_INVALID_MODE;

    int32_t n = p_io->read(p_io->pv_ctx, &header, sizeof(header), 0);
    if (n < 0 || (uint32_t)n != sizeof(header))
        return UDYNLINK_ERR_LOAD_IO_ERROR;

    if (header.sign != UDYNLINK_MODULE_SIGN)
        return UDYNLINK_ERR_LOAD_INVALID_SIGN;

    if (header.udynlink_version > UDYNLINK_LOADER_ABI_VERSION) {
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, "Module udynlink version %d.%d > loader version %d.%d\n",
            UDYNLINK_GET_MAJOR_VERSION(header.udynlink_version), UDYNLINK_GET_MINOR_VERSION(header.udynlink_version),
            UDYNLINK_GET_MAJOR_VERSION(UDYNLINK_LOADER_ABI_VERSION), UDYNLINK_GET_MINOR_VERSION(UDYNLINK_LOADER_ABI_VERSION));
        res = UDYNLINK_ERR_LOAD_VERSION_MISMATCH;
        goto exit;
    }

    {
        uint16_t host_arch = UDYNLINK_HOST_ARCH_TAG;
        uint16_t mod_arch = header.arch_tag;
        if ((mod_arch & UDYNLINK_ARCH_FAMILY_MASK) != (host_arch & UDYNLINK_ARCH_FAMILY_MASK)) {
            UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, "Module architecture family mismatch (mod=0x%04X, host=0x%04X)\n", mod_arch, host_arch);
            res = UDYNLINK_ERR_LOAD_ARCH_MISMATCH;
            goto exit;
        }
        uint16_t mod_float = (mod_arch >> UDYNLINK_ARCH_FLOAT_ABI_SHIFT) & 0x03;
        uint16_t host_float = (host_arch >> UDYNLINK_ARCH_FLOAT_ABI_SHIFT) & 0x03;
        if (mod_float == UDYNLINK_ARCH_FLOAT_ABI_HARD && host_float != UDYNLINK_ARCH_FLOAT_ABI_HARD) {
            res = UDYNLINK_ERR_LOAD_ARCH_MISMATCH;
            goto exit;
        }
        if (mod_float == UDYNLINK_ARCH_FLOAT_ABI_SOFTFP && host_float == UDYNLINK_ARCH_FLOAT_ABI_SOFT) {
            res = UDYNLINK_ERR_LOAD_ARCH_MISMATCH;
            goto exit;
        }
    }

    UDYNLINK_LOAD_SET_MODE(p_mod, load_mode);
    UDYNLINK_LOAD_CLR_STREAM_HDR(p_mod);
    p_mod->num_deps = 0;
    p_mod->dep_refcount = 0;

    if (header.udynlink_version >= UDYNLINK_MAKE_VERSION(2, 0) && header.num_deps > 0) {
        if (header.num_deps > UDYNLINK_MAX_DEPS) {
            res = UDYNLINK_ERR_LOAD_MISSING_DEP;
            goto exit;
        }
        uint32_t strtab_offset = get_deps_strtab_offset(&header);
        uint32_t str_pos = 0;
        for (uint16_t d = 0; d < header.num_deps; d++) {
            char dep_name[64];
            int32_t nr = stream_read_string(p_io, dep_name, strtab_offset + str_pos,
                                            sizeof(dep_name), work_buf, work_buf_size);
            if (nr < 0) { res = UDYNLINK_ERR_LOAD_IO_ERROR; goto exit; }
            if (dep_name[0] == '\0') { res = UDYNLINK_ERR_LOAD_MISSING_DEP; goto exit; }
            udynlink_module_t *dep_mod = udynlink_external_get_module_handle(dep_name);
            if (dep_mod == NULL) {
                UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, "Dependency '%s' not found\n", dep_name);
                res = UDYNLINK_ERR_LOAD_MISSING_DEP;
                goto exit;
            }
            p_mod->deps[d] = dep_mod;
            dep_mod->dep_refcount++;
            p_mod->num_deps++;
            str_pos += (uint32_t)nr;
        }
    }

    uint32_t ram_size = get_ram_size_for_header(&header, load_mode);
    if (load_mode == UDYNLINK_LOAD_MODE_COPY_TEXT_DATA)
        ram_size += get_code_offset_from_header(&header);

    if (ram_size > 0) {
        if (load_addr == NULL) {
            UDYNLINK_LOAD_CLR_FOREIGN_RAM(p_mod);
            if ((ram_addr = udynlink_external_malloc(ram_size)) == NULL) {
                res = UDYNLINK_ERR_LOAD_OUT_OF_MEMORY;
                goto exit;
            }
        } else {
            UDYNLINK_LOAD_SET_FOREIGN_RAM(p_mod);
            if (load_size < ram_size) {
                res = UDYNLINK_ERR_LOAD_RAM_LEN_LOW;
                goto exit;
            }
            ram_addr = load_addr;
        }
    }

    {
        uint32_t code_offset = get_code_offset_from_header(&header);

        if (load_mode == UDYNLINK_LOAD_MODE_COPY_ALL) {
            p_mod->p_ram = ram_addr;
            uint8_t *p_temp8 = (uint8_t *)ram_addr + header.num_lot * sizeof(uint32_t);
            uint32_t copy_size = code_offset + header.code_size + header.data_size;
            if (stream_read_exact(p_io, p_temp8, 0, copy_size, work_buf, work_buf_size) < 0) {
                res = UDYNLINK_ERR_LOAD_IO_ERROR;
                goto exit;
            }
            p_mod->p_header = (const udynlink_module_header_t *)p_temp8;
        } else {
            uint8_t *p_temp8 = (uint8_t *)ram_addr + header.num_lot * sizeof(uint32_t);
            uint32_t code_offset_local = get_code_offset_from_header(&header);
            if (stream_read_exact(p_io, p_temp8, 0, code_offset_local,
                                  work_buf, work_buf_size) < 0) {
                res = UDYNLINK_ERR_LOAD_IO_ERROR;
                goto exit;
            }
            if (stream_read_exact(p_io, p_temp8 + code_offset_local, code_offset_local,
                                  header.code_size + header.data_size,
                                  work_buf, work_buf_size) < 0) {
                res = UDYNLINK_ERR_LOAD_IO_ERROR;
                goto exit;
            }
            p_mod->p_ram = ram_addr;
            p_mod->p_header = (const udynlink_module_header_t *)p_temp8;
            UDYNLINK_LOAD_CLR_STREAM_HDR(p_mod);
            UDYNLINK_LOAD_SET_MODE(p_mod, UDYNLINK_LOAD_MODE_COPY_ALL);
        }

        memset(get_data_pointer(p_mod) + header.data_size, 0, header.bss_size);
    }

    {
        uint32_t *p_lot = (uint32_t *)p_mod->p_ram;
        uint32_t *p_data = (uint32_t *)get_data_pointer(p_mod);
        uint32_t hdr_size = get_header_size(&header);
        uint32_t relocs_offset = hdr_size;
        uint32_t symt_base = hdr_size + header.num_rels * 2 * sizeof(uint32_t);

        for (uint32_t i = 0; i < header.num_rels; i++) {
            uint32_t rel_pair[2];
            if (stream_read_exact(p_io, rel_pair,
                                  relocs_offset + i * sizeof(rel_pair),
                                  sizeof(rel_pair), work_buf, work_buf_size) < 0) {
                res = UDYNLINK_ERR_LOAD_IO_ERROR;
                goto exit;
            }
            uint32_t lot_offset = rel_pair[0];
            uint32_t symt_off = rel_pair[1];

            if (symt_off & (1u << 31)) {
                uint32_t *p = p_data + (lot_offset - header.num_lot);
                *p += (uint32_t)(uintptr_t)p_data - (symt_off & 0x7FFFFFFF);
                continue;
            }

            if (symt_off & (1u << 30)) {
                uint32_t *p = p_data + (lot_offset - header.num_lot);
                *p = ((uint32_t)(uintptr_t)get_code_pointer(p_mod) + (uint32_t)*p);
                continue;
            }

            uint32_t sym_count;
            if (stream_read_exact(p_io, &sym_count, symt_base, sizeof(uint32_t),
                                  work_buf, work_buf_size) < 0) {
                res = UDYNLINK_ERR_LOAD_IO_ERROR;
                goto exit;
            }
            if (symt_off >= sym_count) {
                res = UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE;
                goto exit;
            }

            uint32_t sym_entry_offset = symt_base + sizeof(uint32_t) + symt_off * 2 * sizeof(uint32_t);
            uint32_t sym_entry[2];
            if (stream_read_exact(p_io, sym_entry, sym_entry_offset,
                                  sizeof(sym_entry), work_buf, work_buf_size) < 0) {
                res = UDYNLINK_ERR_LOAD_IO_ERROR;
                goto exit;
            }

            uint32_t name_off = sym_entry[0];
            uint32_t sym_val = sym_entry[1];
            uint32_t info = name_off >> UDYNLINK_SYM_INFO_SHIFT;
            uint8_t sym_type = info & UDYNLINK_SYM_INFO_TYPE_MASK;
            uint8_t sym_location = (info & UDYNLINK_SYM_INFO_CODE_MASK) ?
                UDYNLINK_SYM_LOCATION_CODE : UDYNLINK_SYM_LOCATION_DATA;

            if (sym_type == UDYNLINK_SYM_TYPE_MODULE_NAME) {
                res = UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE;
                goto exit;
            }

            uint32_t *p_rel_location = (lot_offset < header.num_lot) ?
                p_lot + lot_offset : p_data + lot_offset - header.num_lot;

            if (sym_type == UDYNLINK_SYM_TYPE_INTERNAL || sym_type == UDYNLINK_SYM_TYPE_EXPORTED) {
                if (sym_location == UDYNLINK_SYM_LOCATION_CODE)
                    sym_val += (uint32_t)(uintptr_t)get_code_pointer(p_mod);
                else
                    sym_val += (uint32_t)(uintptr_t)get_data_pointer(p_mod);
                *p_rel_location = sym_val;
            } else {
                char sym_name[64];
                uint32_t name_stream_offset = symt_base + (name_off & UDYNLINK_SYM_OFFSET_MASK);
                if (stream_read_string(p_io, sym_name, name_stream_offset,
                                       sizeof(sym_name), work_buf, work_buf_size) < 0) {
                    res = UDYNLINK_ERR_LOAD_IO_ERROR;
                    goto exit;
                }

                uint32_t sym_addr = udynlink_external_resolve_critical_symbol(sym_name);
                if (sym_addr == 0) {
                    for (uint8_t d = 0; d < p_mod->num_deps; d++) {
                        udynlink_sym_t dep_sym;
                        if (udynlink_lookup_symbol(p_mod->deps[d], sym_name, &dep_sym) != NULL) {
                            sym_addr = dep_sym.val;
                            break;
                        }
                    }
                }
                if (sym_addr == 0)
                    sym_addr = udynlink_external_resolve_symbol(sym_name);
                if (sym_addr > 0) {
                    *p_rel_location = sym_addr;
                } else {
                    UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, "Unable to resolve extern symbol '%s'\n", sym_name);
                    res = UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL;
                    goto exit;
                }
            }
        }
    }

    UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Done streaming loading module\n");

exit:
    if (res != UDYNLINK_OK) {
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, error_codes[(int)res]);
        void *p_free = UDYNLINK_LOAD_IS_STREAM_HDR(p_mod) ? (void *)p_mod->p_header : p_mod->p_ram;
        if (p_free != NULL && !UDYNLINK_LOAD_IS_FOREIGN_RAM(p_mod)) {
            udynlink_external_free(p_free);
            UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Deallocated memory area at %p\n", p_free);
        }
        mark_module_free(p_mod);
    }
    return res;
}

uint32_t udynlink_get_ram_requirements(const void *base_addr, udynlink_load_mode_t mode) {
    const udynlink_module_header_t *p_header = (const udynlink_module_header_t *)base_addr;
    return get_ram_size_for_header(p_header, mode);
}

uint32_t udynlink_get_ram_requirements_stream(const udynlink_io_t *p_io, udynlink_load_mode_t mode) {
    udynlink_module_header_t header;
    int32_t n = p_io->read(p_io->pv_ctx, &header, sizeof(header), 0);
    if (n < 0 || (uint32_t)n != sizeof(header)) return 0;
    if (header.sign != UDYNLINK_MODULE_SIGN) return 0;
    uint32_t ram_size = get_ram_size_for_header(&header, mode);
    if (mode == UDYNLINK_LOAD_MODE_COPY_TEXT_DATA)
        ram_size += get_code_offset_from_header(&header);
    return ram_size;
}

uint32_t udynlink_get_stream_metadata_size(const udynlink_io_t *p_io) {
    udynlink_module_header_t header;
    int32_t n = p_io->read(p_io->pv_ctx, &header, sizeof(header), 0);
    if (n < 0 || (uint32_t)n != sizeof(header)) return 0;
    if (header.sign != UDYNLINK_MODULE_SIGN) return 0;
    return get_code_offset_from_header(&header);
}

