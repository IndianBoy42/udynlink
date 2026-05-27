#include "udynlink.h"
#include "udynlink_externals.h"
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

__attribute__((weak))
int udynlink_external_is_module_loading(const char *module_name) {
    (void)module_name;
    return 0;
}

__attribute__((weak))
uintptr_t udynlink_external_resolve_critical_symbol(const char *name) {
    (void)name;
    return 0;
}

__attribute__((weak))
uintptr_t udynlink_external_resolve_symbol(const char *name) {
    (void)name;
    return 0;
}

__attribute__((weak))
struct _udynlink_module_t *udynlink_external_get_module_handle(const char *module_name) {
    (void)module_name;
    return NULL;
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
static size_t get_header_size(const udynlink_module_header_t *p_header) {
    if (p_header->udynlink_version < UDYNLINK_MAKE_VERSION(2, 0))
        return 32;
    return sizeof(udynlink_module_header_t);
}

static size_t get_deps_strtab_offset(const udynlink_module_header_t *p_header) {
    return get_header_size(p_header) + p_header->num_rels * 2 * sizeof(uint32_t) + p_header->symt_size;
}

static const char *get_deps_strtab(const udynlink_module_header_t *p_header) {
    if (p_header->udynlink_version < UDYNLINK_MAKE_VERSION(2, 0) || p_header->num_deps == 0)
        return NULL;
    return (const char *)p_header + get_deps_strtab_offset(p_header);
}

static size_t get_code_offset_from_header(const udynlink_module_header_t *p_header) {
    size_t res = get_deps_strtab_offset(p_header);
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
static udynlink_sym_t *get_sym_at(const udynlink_module_header_t *p_header, size_t index, udynlink_sym_t *p_sym) {
    uint32_t name_off;
    uint32_t info;
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
// Helpers - various (continued)

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
// Streaming I/O helpers

static int32_t stream_read_exact(const udynlink_io_t *p_io, void *dest,
                                 uint32_t offset, uint32_t len) {
    uint8_t *d = (uint8_t *)dest;
    uint32_t pos = 0;
    while (pos < len) {
        int32_t n = p_io->read(p_io->pv_ctx, d + pos, len - pos, offset + pos);
        if (n <= 0) return -1;
        pos += (uint32_t)n;
    }
    return (int32_t)len;
}

static int32_t stream_read_string(const udynlink_io_t *p_io, char *dest,
                                  uint32_t offset, uint32_t max_len) {
    if (max_len < 2) return -1;
    uint32_t total_read = 0;
    uint32_t cur_offset = offset;
    while (total_read + 1 < max_len) {
        uint32_t chunk = max_len - total_read - 1;
        int32_t n = p_io->read(p_io->pv_ctx, dest + total_read, chunk, cur_offset);
        if (n <= 0) return -1;
        for (int32_t i = 0; i < n; i++) {
            if (dest[total_read + i] == '\0') return (int32_t)(total_read + i + 1);
        }
        total_read += (uint32_t)n;
        cur_offset += (uint32_t)n;
        if ((uint32_t)n < chunk) return -1;
    }
    dest[total_read] = '\0';
    return -1;
}

////////////////////////////////////////////////////////////////////////////////
// Public interface

udynlink_error_t udynlink_load_module(udynlink_module_t *p_mod, const void *base_addr, void *load_addr, size_t load_size, udynlink_load_mode_t load_mode) {
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
                const char *mod_name = udynlink_get_module_name(p_mod);
                if (mod_name && !strcmp(dep_str, mod_name)) {
                    UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR,
                        "Circular dependency detected: module '%s' depends on itself\n",
                        mod_name);
                    res = UDYNLINK_ERR_LOAD_CIRCULAR_DEP;
                    goto exit;
                }
                if (udynlink_external_is_module_loading(dep_str)) {
                    UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR,
                        "Circular dependency detected: module '%s' depends on '%s' which is currently loading\n",
                        mod_name ? mod_name : "(unknown)", dep_str);
                    res = UDYNLINK_ERR_LOAD_CIRCULAR_DEP;
                    goto exit;
                }
                UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, "Dependency '%s' not found\n", dep_str);
                res = UDYNLINK_ERR_LOAD_MISSING_DEP;
                goto exit;
            }
            if (dep_mod == UDYNLINK_DEP_DEFERRED) {
                // Skip: don't add to deps[], don't increment num_deps, don't increment refcount
                dep_str += strlen(dep_str) + 1;
                continue;
            }
            p_mod->deps[p_mod->num_deps++] = dep_mod;
            dep_mod->dep_refcount++;
            dep_str += strlen(dep_str) + 1;
        }
    }

    // Allocate RAM or check given RAM region, as needed
    size_t ram_size = udynlink_get_ram_size(p_mod);
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
    for (size_t i = 0; i < p_header->num_rels; i ++) {
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

            case UDYNLINK_SYM_TYPE_WEAK:
                // Write the module's own address first (default fallback), then
                // try host/dependency override.  Unlike EXTERN, failure to
                // resolve a weak symbol is not fatal — the module definition
                // remains.
                UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Applying weak relocation for symbol at index %u, name=%s at lot_offset=%u\n", symt_offset, sym.name, lot_offset);
                *p_rel_location = offset_sym(p_mod, &sym)->val;
                {
                    uintptr_t sym_addr = udynlink_external_resolve_critical_symbol(sym.name);
                    if (sym_addr == UDYNLINK_SYM_DEFERRED) {
                        // Keep module's own default, defer override
                        break;
                    }
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
                        if (sym_addr == UDYNLINK_SYM_DEFERRED) {
                            break;
                        }
                    }
                    if (sym_addr > 0) {
                        *p_rel_location = (uint32_t)sym_addr;
                    }
                }
                break;

            case UDYNLINK_SYM_TYPE_EXTERN:
                UDYNLINK_DEBUG(UDYNLINK_DEBUG_INFO, "Applying extern relocation for symbol at index %u, name=%s at lot_offset=%u\n", symt_offset, sym.name, lot_offset);
                {
                    uintptr_t sym_addr = udynlink_external_resolve_critical_symbol(sym.name);
                    if (sym_addr == UDYNLINK_SYM_DEFERRED) {
                        *p_rel_location = 0;
                        break;
                    }
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
                        if (sym_addr == UDYNLINK_SYM_DEFERRED) {
                            *p_rel_location = 0;
                            break;
                        }
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
        uintptr_t* mod_base = (uintptr_t*)UDYNLINK_LOT_BASE_ADDR;
        *mod_base = p_mod->ram_base;
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

size_t udynlink_get_module_deps(const void *base_addr, const char **deps, size_t max_deps) {
    const udynlink_module_header_t *p_header = (const udynlink_module_header_t*)base_addr;

    if (p_header->sign != UDYNLINK_MODULE_SIGN)
        return 0;

    const char *dep_str = get_deps_strtab(p_header);
    if (dep_str == NULL)
        return 0;

    size_t count = 0;
    for (uint16_t d = 0; d < p_header->num_deps; d++) {
        if (*dep_str == '\0')
            break;
        if (count < max_deps && deps != NULL)
            deps[count] = dep_str;
        count++;
        dep_str += strlen(dep_str) + 1;
    }
    return count;
}

udynlink_sym_t *udynlink_lookup_symbol(const udynlink_module_t *p_mod, const char *name, udynlink_sym_t *p_sym) {
    size_t idx;

    if (p_mod != NULL) { // but consider only the given one if not NULL
        idx = 0;
        while (get_sym_at(p_mod->p_header, idx ++, p_sym) != NULL) { // iterate through module's symbol table
            if (!strcmp(p_sym->name, name)) { // symbol found
                offset_sym(p_mod, p_sym); // offset value properly before returning
                if (p_sym->type == UDYNLINK_SYM_TYPE_WEAK) {
                    // For weak symbols, runtime lookup must also resolve the
                    // host/dependency override so external callers see the
                    // correct address.  The three-tier resolution is identical
                    // to the load-time path above.
                    uintptr_t sym_addr = udynlink_external_resolve_critical_symbol(name);
                    if (sym_addr == 0) {
                        for (uint8_t d = 0; d < p_mod->num_deps; d++) {
                            udynlink_sym_t dep_sym;
                            if (udynlink_lookup_symbol(p_mod->deps[d], name, &dep_sym) != NULL) {
                                sym_addr = dep_sym.val;
                                break;
                            }
                        }
                    }
                    if (sym_addr == 0)
                        sym_addr = udynlink_external_resolve_symbol(name);
                    if (sym_addr > 0) {
                        p_sym->val = sym_addr;
                    }
                }
                return p_sym;
            }
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
    debug_level = level;
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

////////////////////////////////////////////////////////////////////////////////
// Streaming I/O public interface

udynlink_error_t udynlink_load_module_from_stream(udynlink_module_t *p_mod,
    const udynlink_io_t *p_io, void *load_addr, size_t load_size,
    udynlink_load_mode_t load_mode, void *scratch_buf, size_t scratch_buf_size) {

    _Static_assert(
        UDYNLINK_STREAM_MIN_SCRATCH_BUF_SIZE >= 36 + 64 + 8 + 4 + 8 + 12,
        "scratch buffer too small for ARM target layout");

    void *ram_addr = NULL;
    udynlink_error_t res = UDYNLINK_OK;

    if (!p_mod) return UDYNLINK_ERR_INVALID_MODULE;
    if (load_mode == UDYNLINK_LOAD_MODE_XIP) return UDYNLINK_ERR_LOAD_XIP_UNSUPPORTED;
    if (scratch_buf == NULL || scratch_buf_size < UDYNLINK_STREAM_MIN_SCRATCH_BUF_SIZE)
        return UDYNLINK_ERR_LOAD_INVALID_MODE;

    udynlink_module_header_t *header = (udynlink_module_header_t *)scratch_buf;
    char *name_buf = (char *)((uint8_t *)scratch_buf + sizeof(udynlink_module_header_t));
    uint32_t *rel_pair = (uint32_t *)((uint8_t *)scratch_buf + 100);
    uint32_t *sym_count_ptr = (uint32_t *)((uint8_t *)scratch_buf + 108);
    uint32_t *sym_entry = (uint32_t *)((uint8_t *)scratch_buf + 112);
    udynlink_sym_t *dep_sym = (udynlink_sym_t *)((uint8_t *)scratch_buf + 120);

    int32_t n = p_io->read(p_io->pv_ctx, header, sizeof(*header), 0);
    if (n < 0 || (size_t)n != sizeof(*header))
        return UDYNLINK_ERR_LOAD_IO_ERROR;

    if (header->sign != UDYNLINK_MODULE_SIGN)
        return UDYNLINK_ERR_LOAD_INVALID_SIGN;

    if (header->udynlink_version > UDYNLINK_LOADER_ABI_VERSION) {
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, "Module udynlink version %d.%d > loader version %d.%d\n",
            UDYNLINK_GET_MAJOR_VERSION(header->udynlink_version), UDYNLINK_GET_MINOR_VERSION(header->udynlink_version),
            UDYNLINK_GET_MAJOR_VERSION(UDYNLINK_LOADER_ABI_VERSION), UDYNLINK_GET_MINOR_VERSION(UDYNLINK_LOADER_ABI_VERSION));
        res = UDYNLINK_ERR_LOAD_VERSION_MISMATCH;
        goto exit;
    }

    {
        uint16_t host_arch = UDYNLINK_HOST_ARCH_TAG;
        uint16_t mod_arch = header->arch_tag;
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

    if (header->udynlink_version >= UDYNLINK_MAKE_VERSION(2, 0) && header->num_deps > 0) {
        if (header->num_deps > UDYNLINK_MAX_DEPS) {
            res = UDYNLINK_ERR_LOAD_MISSING_DEP;
            goto exit;
        }
        size_t strtab_offset = get_deps_strtab_offset(header);
        size_t str_pos = 0;
        for (uint16_t d = 0; d < header->num_deps; d++) {
            int32_t nr = stream_read_string(p_io, name_buf, strtab_offset + str_pos, 64);
            if (nr < 0) { res = UDYNLINK_ERR_LOAD_IO_ERROR; goto exit; }
            if (name_buf[0] == '\0') { res = UDYNLINK_ERR_LOAD_MISSING_DEP; goto exit; }
            udynlink_module_t *dep_mod = udynlink_external_get_module_handle(name_buf);
            if (dep_mod == NULL) {
                const char *mod_name = udynlink_get_module_name_from_image(header);
                if (mod_name && !strcmp(name_buf, mod_name)) {
                    UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR,
                        "Circular dependency detected: module '%s' depends on itself\n",
                        mod_name);
                    res = UDYNLINK_ERR_LOAD_CIRCULAR_DEP;
                    goto exit;
                }
                if (udynlink_external_is_module_loading(name_buf)) {
                    UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR,
                        "Circular dependency detected: module '%s' depends on '%s' which is currently loading\n",
                        mod_name ? mod_name : "(unknown)", name_buf);
                    res = UDYNLINK_ERR_LOAD_CIRCULAR_DEP;
                    goto exit;
                }
                UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, "Dependency '%s' not found\n", name_buf);
                res = UDYNLINK_ERR_LOAD_MISSING_DEP;
                goto exit;
            }
            if (dep_mod == UDYNLINK_DEP_DEFERRED) {
                str_pos += (size_t)nr;
                continue;
            }
            p_mod->deps[p_mod->num_deps++] = dep_mod;
            dep_mod->dep_refcount++;
            str_pos += (size_t)nr;
        }
    }

    size_t ram_size = get_ram_size_for_header(header, load_mode);
    if (load_mode == UDYNLINK_LOAD_MODE_COPY_TEXT_DATA)
        ram_size += get_code_offset_from_header(header);

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
        size_t code_offset = get_code_offset_from_header(header);

        if (load_mode == UDYNLINK_LOAD_MODE_COPY_ALL) {
            p_mod->p_ram = ram_addr;
            uint8_t *p_temp8 = (uint8_t *)ram_addr + header->num_lot * sizeof(uint32_t);
            size_t copy_size = code_offset + header->code_size + header->data_size;
            if (stream_read_exact(p_io, p_temp8, 0, copy_size) < 0) {
                res = UDYNLINK_ERR_LOAD_IO_ERROR;
                goto exit;
            }
            p_mod->p_header = (const udynlink_module_header_t *)p_temp8;
        } else {
            uint8_t *p_temp8 = (uint8_t *)ram_addr + header->num_lot * sizeof(uint32_t);
            size_t code_offset_local = get_code_offset_from_header(header);
            if (stream_read_exact(p_io, p_temp8, 0, code_offset_local) < 0) {
                res = UDYNLINK_ERR_LOAD_IO_ERROR;
                goto exit;
            }
            if (stream_read_exact(p_io, p_temp8 + code_offset_local, code_offset_local,
                                  header->code_size + header->data_size) < 0) {
                res = UDYNLINK_ERR_LOAD_IO_ERROR;
                goto exit;
            }
            p_mod->p_ram = ram_addr;
            p_mod->p_header = (const udynlink_module_header_t *)p_temp8;
            UDYNLINK_LOAD_CLR_STREAM_HDR(p_mod);
            UDYNLINK_LOAD_SET_MODE(p_mod, UDYNLINK_LOAD_MODE_COPY_ALL);
        }

        memset(get_data_pointer(p_mod) + header->data_size, 0, header->bss_size);
    }

    {
        uint32_t *p_lot = (uint32_t *)p_mod->p_ram;
        uint32_t *p_data = (uint32_t *)get_data_pointer(p_mod);
        size_t hdr_size = get_header_size(header);
        size_t relocs_offset = hdr_size;
        size_t symt_base = hdr_size + header->num_rels * 2 * sizeof(uint32_t);

        for (size_t i = 0; i < header->num_rels; i++) {
            if (stream_read_exact(p_io, rel_pair,
                                  relocs_offset + i * 2 * sizeof(uint32_t),
                                  sizeof(uint32_t[2])) < 0) {
                res = UDYNLINK_ERR_LOAD_IO_ERROR;
                goto exit;
            }
            uint32_t lot_offset = rel_pair[0];
            uint32_t symt_off = rel_pair[1];

            if (symt_off & (1u << 31)) {
                uint32_t *p = p_data + (lot_offset - header->num_lot);
                *p += (uint32_t)(uintptr_t)p_data - (symt_off & 0x7FFFFFFF);
                continue;
            }

            if (symt_off & (1u << 30)) {
                uint32_t *p = p_data + (lot_offset - header->num_lot);
                *p = ((uint32_t)(uintptr_t)get_code_pointer(p_mod) + *p);
                continue;
            }

            if (stream_read_exact(p_io, sym_count_ptr, symt_base, sizeof(uint32_t)) < 0) {
                res = UDYNLINK_ERR_LOAD_IO_ERROR;
                goto exit;
            }
            if (symt_off >= *sym_count_ptr) {
                res = UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE;
                goto exit;
            }

            size_t sym_entry_offset = symt_base + sizeof(uint32_t) + symt_off * 2 * sizeof(uint32_t);
            if (stream_read_exact(p_io, sym_entry, sym_entry_offset,
                                  sizeof(uint32_t[2])) < 0) {
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

            uint32_t *p_rel_location = (lot_offset < header->num_lot) ?
                p_lot + lot_offset : p_data + lot_offset - header->num_lot;

            if (sym_type == UDYNLINK_SYM_TYPE_INTERNAL || sym_type == UDYNLINK_SYM_TYPE_EXPORTED) {
                if (sym_location == UDYNLINK_SYM_LOCATION_CODE)
                    sym_val += (uintptr_t)get_code_pointer(p_mod);
                else
                    sym_val += (uintptr_t)get_data_pointer(p_mod);
                *p_rel_location = (uint32_t)sym_val;
            } else if (sym_type == UDYNLINK_SYM_TYPE_WEAK) {
                if (sym_location == UDYNLINK_SYM_LOCATION_CODE)
                    sym_val += (uintptr_t)get_code_pointer(p_mod);
                else
                    sym_val += (uintptr_t)get_data_pointer(p_mod);
                *p_rel_location = (uint32_t)sym_val;
                size_t name_stream_offset = symt_base + (name_off & UDYNLINK_SYM_OFFSET_MASK);
                if (stream_read_string(p_io, name_buf, name_stream_offset, 64) < 0) {
                    res = UDYNLINK_ERR_LOAD_IO_ERROR;
                    goto exit;
                }
                uintptr_t sym_addr = udynlink_external_resolve_critical_symbol(name_buf);
                if (sym_addr == UDYNLINK_SYM_DEFERRED) {
                    continue;
                }
                if (sym_addr == 0) {
                    for (uint8_t d = 0; d < p_mod->num_deps; d++) {
                        if (udynlink_lookup_symbol(p_mod->deps[d], name_buf, dep_sym) != NULL) {
                            sym_addr = dep_sym->val;
                            break;
                        }
                    }
                }
                if (sym_addr == 0) {
                    sym_addr = udynlink_external_resolve_symbol(name_buf);
                    if (sym_addr == UDYNLINK_SYM_DEFERRED) {
                        continue;
                    }
                }
                if (sym_addr > 0) {
                    *p_rel_location = (uint32_t)sym_addr;
                }
            } else {
                size_t name_stream_offset = symt_base + (name_off & UDYNLINK_SYM_OFFSET_MASK);
                if (stream_read_string(p_io, name_buf, name_stream_offset, 64) < 0) {
                    res = UDYNLINK_ERR_LOAD_IO_ERROR;
                    goto exit;
                }

                uintptr_t sym_addr = udynlink_external_resolve_critical_symbol(name_buf);
                if (sym_addr == UDYNLINK_SYM_DEFERRED) {
                    *p_rel_location = 0;
                    continue;
                }
                if (sym_addr == 0) {
                    for (uint8_t d = 0; d < p_mod->num_deps; d++) {
                        if (udynlink_lookup_symbol(p_mod->deps[d], name_buf, dep_sym) != NULL) {
                            sym_addr = dep_sym->val;
                            break;
                        }
                    }
                }
                if (sym_addr == 0) {
                    sym_addr = udynlink_external_resolve_symbol(name_buf);
                    if (sym_addr == UDYNLINK_SYM_DEFERRED) {
                        *p_rel_location = 0;
                        continue;
                    }
                }
                if (sym_addr > 0) {
                    *p_rel_location = (uint32_t)sym_addr;
                } else {
                    UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR, "Unable to resolve extern symbol '%s'\n", name_buf);
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

size_t udynlink_get_ram_requirements(const void *base_addr, udynlink_load_mode_t mode) {
    const udynlink_module_header_t *p_header = (const udynlink_module_header_t *)base_addr;
    return get_ram_size_for_header(p_header, mode);
}

size_t udynlink_get_ram_requirements_stream(const udynlink_io_t *p_io, udynlink_load_mode_t mode) {
    udynlink_module_header_t header;
    int32_t n = p_io->read(p_io->pv_ctx, &header, sizeof(header), 0);
    if (n < 0 || (size_t)n != sizeof(header)) return 0;
    if (header.sign != UDYNLINK_MODULE_SIGN) return 0;
    size_t ram_size = get_ram_size_for_header(&header, mode);
    if (mode == UDYNLINK_LOAD_MODE_COPY_TEXT_DATA)
        ram_size += get_code_offset_from_header(&header);
    return ram_size;
}

size_t udynlink_get_stream_metadata_size(const udynlink_io_t *p_io) {
    udynlink_module_header_t header;
    int32_t n = p_io->read(p_io->pv_ctx, &header, sizeof(header), 0);
    if (n < 0 || (size_t)n != sizeof(header)) return 0;
    if (header.sign != UDYNLINK_MODULE_SIGN) return 0;
    return get_code_offset_from_header(&header);
}

// Helper: re-resolve all EXTERN relocations in a loaded module.
// Used by udynlink_link_dependency() after a new dependency is added.
static void apply_extern_relocations(udynlink_module_t *p_mod) {
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

        uintptr_t sym_addr = udynlink_external_resolve_critical_symbol(sym.name);
        if (sym_addr == UDYNLINK_SYM_DEFERRED) {
            *p_rel_location = 0;
            continue;
        }
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
            if (sym_addr == UDYNLINK_SYM_DEFERRED) {
                *p_rel_location = 0;
                continue;
            }
        }
        if (sym_addr > 0) {
            *p_rel_location = (uint32_t)sym_addr;
        } else {
            *p_rel_location = 0; // unresolved after re-resolution
        }
    }
}

udynlink_error_t udynlink_link_dependency(udynlink_module_t *a, udynlink_module_t *b) {
    if (!a || !b) return UDYNLINK_ERR_INVALID_MODULE;

    const udynlink_module_header_t *ha = a->p_header;
    const udynlink_module_header_t *hb = b->p_header;

    // Direction A -> B
    const char *deps_a = get_deps_strtab(ha);
    if (deps_a) {
        for (uint16_t d = 0; d < ha->num_deps; d++) {
            if (strcmp(deps_a, udynlink_get_module_name(b)) == 0) {
                int already = 0;
                for (uint16_t i = 0; i < a->num_deps; i++) {
                    if (a->deps[i] == b) { already = 1; break; }
                }
                if (!already) {
                    a->deps[a->num_deps++] = b;
                    b->dep_refcount++;
                    apply_extern_relocations(a);
                }
                break;
            }
            deps_a += strlen(deps_a) + 1;
        }
    }

    // Direction B -> A
    const char *deps_b = get_deps_strtab(hb);
    if (deps_b) {
        for (uint16_t d = 0; d < hb->num_deps; d++) {
            if (strcmp(deps_b, udynlink_get_module_name(a)) == 0) {
                int already = 0;
                for (uint16_t i = 0; i < b->num_deps; i++) {
                    if (b->deps[i] == a) { already = 1; break; }
                }
                if (!already) {
                    b->deps[b->num_deps++] = a;
                    a->dep_refcount++;
                    apply_extern_relocations(b);
                }
                break;
            }
            deps_b += strlen(deps_b) + 1;
        }
    }

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

int udynlink_is_module_fully_linked(const udynlink_module_t *p_mod) {
    if (!p_mod || !p_mod->p_header) return 0;
    return p_mod->num_deps == p_mod->p_header->num_deps;
}

udynlink_module_t *udynlink_get_linked_dependency(const udynlink_module_t *p_mod, const char *dep_name) {
    if (!p_mod || !dep_name) return NULL;
    for (uint16_t i = 0; i < p_mod->num_deps; i++) {
        const char *name = udynlink_get_module_name(p_mod->deps[i]);
        if (name && strcmp(name, dep_name) == 0)
            return (udynlink_module_t *)p_mod->deps[i];
    }
    return NULL;
}

int udynlink_is_symbol_resolved(const udynlink_module_t *p_mod, const char *sym_name) {
    if (!p_mod || !sym_name) return 0;
    udynlink_sym_t sym;
    if (!udynlink_lookup_symbol(p_mod, sym_name, &sym)) return 0;
    return sym.val != 0;
}

