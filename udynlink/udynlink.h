/* ARM Cortex-M micro dynamic linker (udynlink) public interface.
 *
 * Copyright (c) 2016 Bogdan Marinescu
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __UDYNLINK_H__
#define __UDYNLINK_H__

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

////////////////////////////////////////////////////////////////////////////////
// Data structures and macros

/**
 * @brief Module image header.
 *
 * Every loadable binary module begins with this header.  The on-disk
 * layout that follows the header is:
 *   - Relocation table (@c num_rels * 8 bytes)
 *   - Symbol table (@c symt_size bytes, rounded up to 4)
 *   - Dependency string table (@c deps_strtab_size bytes, v2.0+ only)
 *   - Code section (rounded up to a multiple of 4 bytes)
 *   - Data section
 */
typedef struct {
    /** Module signature ('UDLM' in little-endian). */
    uint32_t sign;
    /** Module ABI version (major.minor) used when the module was built. */
    uint16_t mod_version;
    /** Minimum loader ABI version required to load this module. */
    uint16_t udynlink_version;
    /** Target architecture tag (core family, FPU, float ABI). */
    uint16_t arch_tag;
    /** Number of Linker Offset Table (LOT) entries. */
    uint16_t num_lot;
    /** Number of relocation entries. */
    uint16_t num_rels;
    /** Number of dependency names (0 for ABI v1.0 modules). */
    uint16_t num_deps;
    /** Size of the symbol table in bytes. */
    uint32_t symt_size;
    /** Size of the code (.text) section in bytes. */
    uint32_t code_size;
    /** Size of the initialized data (.data) section in bytes. */
    uint32_t data_size;
    /** Size of the zero-initialized data (.bss) section in bytes. */
    uint32_t bss_size;
    /** Size of the dependency string table in bytes (0 for ABI v1.0 modules). */
    uint32_t deps_strtab_size;
    /* Then relocations (num_rels * 8 bytes) */
    /* Then the symbol table (symt_size bytes, rounded up to 4) */
    /* Then the dependency string table (deps_strtab_size bytes) [v2.0+] */
    /* Then the code (rounded up to a multiple of 4 bytes) */
    /* Then data */
} udynlink_module_header_t;

/**
 * @brief Load mode for udynlink_load_module().
 *
 * Controls how much of the module image is copied into RAM versus
 * executed directly from its original location (e.g., flash).
 */
typedef enum {
    /** Copy header, code, and data into RAM. */
    UDYNLINK_LOAD_MODE_COPY_ALL,
    /** Copy code and data into RAM; leave the header at @c base_addr. */
    UDYNLINK_LOAD_MODE_COPY_TEXT_DATA,
    /** Execute code in place (XIP); copy only data into RAM. */
    UDYNLINK_LOAD_MODE_XIP,
} udynlink_load_mode_t;

/**
 * @brief Sentinel returned by udynlink_external_get_module_handle() to defer a dependency.
 *
 * On ARM Cortex-M (32-bit), address 0x00000001 is never a valid heap-allocated
 * struct pointer, so it cannot collide with real module handles.  Existing hosts
 * that never return this sentinel keep the old strict behavior unchanged.
 */
#define UDYNLINK_DEP_DEFERRED                 ((udynlink_module_t*)1)

/**
 * @brief Sentinel returned by symbol-resolution callbacks to defer an extern symbol.
 *
 * Address 0x00000001 is not a valid code or data address on Cortex-M.  When a
 * resolve callback returns this value the loader writes 0 to the relocation slot
 * and continues loading instead of failing.
 */
#define UDYNLINK_SYM_DEFERRED                 ((uintptr_t)1)

/**
 * @brief Non-contiguous module image descriptor.
 *
 * Points to each section of a module image independently.  The loader
 * reads relocation and symbol information through these pointers; it
 * never assumes the image is contiguous.
 *
 * For memory-mapped images that follow the standard UDLM layout,
 * use udynlink_image_from_memory() to populate this structure.
 *
 * @note For COPY_TEXT_DATA and XIP load modes, the metadata (header,
 * relocation table, symbol table) must remain contiguous with the header
 * starting at @c p_header, because post-load symbol lookups derive the
 * symtab offset from the header.  If your source layout does not satisfy
 * this, use COPY_ALL.
 */
typedef struct {
    /** Module header (36 bytes). Only @c p_header is read by functions that take the full descriptor. */
    const udynlink_module_header_t *p_header;
    /** Relocation table: num_rels * 2 * uint32_t. */
    const uint32_t *p_relocations;
    /** Symbol table base (first word = entry count). The string pool is assumed contiguous with entries. */
    const uint32_t *p_symtab;
    /** Dependency string table (NULL for v1.0 or no deps). */
    const char     *p_deps_strtab;
    /** Code section (.text) in the source. */
    const uint8_t  *p_code;
    /** Data section (.data) in the source. */
    const uint8_t  *p_data;
} udynlink_module_image_t;

/**
 * @brief Runtime module handle.
 *
 * Holds the loader's internal state for one loaded module instance.
 * The host may attach arbitrary context via the @c user_ctx field.
 */
typedef struct _udynlink_module_t {
    /** Pointer to the module header (in flash or RAM depending on load mode). */
    const udynlink_module_header_t *p_header;
    /** Anonymous union of RAM base pointer and its numeric representation. */
    union {
        /** Pointer to the module's RAM region (LOT, data, bss, optional code). */
        void *p_ram;
        /** Same address as an unsigned integer. */
        uintptr_t ram_base;
    };
    /** Bitmask storing the load mode and RAM ownership flags. */
    uint8_t info;
    /** Number of valid entries in @c deps (populated by the loader for
     *  non-deferred dependencies, or by the host for deferred links). */
    uint8_t num_deps;
    /** Number of other loaded modules that list this module as a dependency. */
    uint8_t dep_refcount;
    /** Capacity of the @c deps backing array (number of slots the host
     *  allocated). Must be at least @c p_header->num_deps. */
    uint8_t max_deps;
    /** Array of pointers to dependency modules.
     *
     *  This is a host-allocated backing array of @c max_deps slots.
     *  The first @c num_deps entries are valid dependency pointers;
     *  remaining slots should be left @c NULL.  The loader populates
     *  entries during udynlink_load_module() for dependencies that are
     *  already loaded.  For deferred or optional dependencies, the host
     *  must manually append pointers (bumping @c num_deps and the
     *  target module's @c dep_refcount) before calling
     *  udynlink_link_incremental() or udynlink_relink_all().
     *
     *  May be @c NULL if the module declares no dependencies. */
    const struct _udynlink_module_t **deps;
    /** Opaque user context pointer.  Never read or written by the loader;
     *  provided for the host to associate arbitrary state with a module
     *  handle (e.g., a filesystem path, a reference counter, or a
     *  higher-level language runtime handle). */
    void *user_ctx;
    /**
     * Number of named (searchable) symbol entries in the module's symbol
     * table, starting at index 1.  The symbol table is sorted
     * lexicographically so that udynlink_lookup_symbol can use binary
     * search over indices [1, num_named_syms].  Local (nameless) symbols
     * occupy the remaining entries after num_named_syms.  Computed once at
     * load time.
     */
    uint16_t num_named_syms;
} udynlink_module_t;

/**
 * @brief Symbol descriptor.
 *
 * Describes a single symbol entry in the module's symbol table.
 * Symbols can represent functions or variables and can live in the
 * code region or the data region.
 */
typedef struct {
    /** Symbol name, or "(N/A)" for local symbols. */
    const char *name;
    /** Symbol value (offset or absolute address, depending on load phase). */
    uintptr_t val;
    /** Symbol type (see ::UDYNLINK_SYM_TYPE_INTERNAL et al.). */
    uint8_t type;
    /** Memory location (code or data, see ::UDYNLINK_SYM_LOCATION_CODE). */
    uint8_t location;
} udynlink_sym_t;

/** Static (module-local) symbol. */
#define UDYNLINK_SYM_TYPE_INTERNAL               0
/** Exported symbol (visible to other modules and the host). */
#define UDYNLINK_SYM_TYPE_EXPORTED            1
/** Extern symbol (unresolved at link time; resolved by the host at load time). */
#define UDYNLINK_SYM_TYPE_EXTERN              2
/** Special symbol representing the module name. */
#define UDYNLINK_SYM_TYPE_MODULE_NAME                3
/**
 * Weak symbol (defined in the module).
 *
 * At load time the loader first applies the module's own address (as for
 * INTERNAL/EXPORTED), then attempts host/dependency override via the same
 * three-tier resolution used for EXTERN symbols.  If no override is found the
 * module's own definition remains in place.
 *
 * Limitation: direct PC-relative calls inside the module (e.g. `bl weak_func`)
 * are resolved at link time and cannot be rewritten at load time.  Therefore
 * internal callers still reach the prologue wrapper, which unconditionally
 * branches to the module's renamed local implementation.  Host override only
 * takes effect for:
 *   - LOT/data relocations (data weak symbols, function pointers)
 *   - External callers via `udynlink_lookup_symbol()`
 */
#define UDYNLINK_SYM_TYPE_WEAK                       4

/** Symbol resides in the code (.text) section. */
#define UDYNLINK_SYM_LOCATION_CODE            0
/** Symbol resides in the data section. */
#define UDYNLINK_SYM_LOCATION_DATA            1

/**
 * @brief Error codes returned by loader functions.
 *
 * The list is expanded by the preprocessor to produce both an enum
 * and the corresponding string table.
 */
#define UDYNLINK_ERROR_CODES \
_UDYNLINK_EXPAND(UDYNLINK_OK),\
_UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_INVALID_SIGN),\
_UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_RAM_LEN_LOW),\
_UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_OUT_OF_MEMORY),\
_UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_XIP_UNSUPPORTED),\
_UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_INVALID_MODE),\
_UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE),\
_UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL),\
_UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_DUPLICATE_NAME),\
_UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_VERSION_MISMATCH),\
_UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_ARCH_MISMATCH),\
_UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_MISSING_DEP),\
_UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_CIRCULAR_DEP),\
_UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_IO_ERROR),\
_UDYNLINK_EXPAND(UDYNLINK_ERR_MODULE_HAS_DEPENDENTS),\
_UDYNLINK_EXPAND(UDYNLINK_ERR_INVALID_MODULE),\
_UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_HOOK_ABORTED)

#define _UDYNLINK_EXPAND(x)                   x
/**
 * @brief Loader result codes.
 *
 * @see udynlink_error_msg()
 */
typedef enum {
    UDYNLINK_ERROR_CODES
} udynlink_error_t;
#undef _UDYNLINK_EXPAND

/**
 * @brief Debug verbosity levels.
 *
 * Used with udynlink_set_debug_level().  If this enum is modified,
 * the corresponding array in udynlink.c must also be updated.
 */
typedef enum {
    /** No debug output. */
    UDYNLINK_DEBUG_NONE,
    /** Fatal errors only. */
    UDYNLINK_DEBUG_ERROR,
    /** Non-fatal warnings. */
    UDYNLINK_DEBUG_WARNING,
    /** Informational messages. */
    UDYNLINK_DEBUG_INFO
} udynlink_debug_level_t;

/* Compile-time configuration */

#ifndef UDYNLINK_DEBUG_LEVEL
/** Compile-time debug level (defaults to none). Overridden with -DUDYNLINK_DEBUG_LEVEL=... */
#define UDYNLINK_DEBUG_LEVEL UDYNLINK_DEBUG_NONE
#endif

#if UDYNLINK_DEBUG_LEVEL > UDYNLINK_DEBUG_NONE
#define UDYNLINK_DEBUG(...) udynlink_debug(__func__, __LINE__, __VA_ARGS__)
#else
#define UDYNLINK_DEBUG(...) ((void)0)
#endif

#ifndef UDYNLINK_HOST_ARCH_TAG
/** Architecture tag of the host MCU (defaults to Cortex-M4). */
#define UDYNLINK_HOST_ARCH_TAG UDYNLINK_ARCH_TAG_CORTEX_M4
#endif

#ifndef UDYNLINK_LOT_BASE_ADDR
/**
 * @brief Fixed memory address where the loader writes the LOT base.
 *
 * The host must write @c p_mod->ram_base to this address before
 * calling any module function, because the module's assembly prologue
 * loads @c r9 from here.
 */
#define UDYNLINK_LOT_BASE_ADDR 0x20000000
#endif

/** ABI version of this loader (2.0). */
#define UDYNLINK_LOADER_ABI_VERSION           UDYNLINK_MAKE_VERSION(2, 0)

/**
 * @brief Architecture tag constants.
 *
 * Encoded in a @c uint16_t with the following bit layout:
 *   - Bits [3:0]  — core family ID
 *   - Bit  4      — FPU present (1) or absent (0)
 *   - Bits [6:5]  — float ABI (00=soft, 01=softfp, 10=hard)
 *   - Bits [15:7] — reserved
 */
#define UDYNLINK_ARCH_FAMILY_MASK           0x0F
#define UDYNLINK_ARCH_FPU_MASK              0x10
#define UDYNLINK_ARCH_FLOAT_ABI_MASK        0x60
#define UDYNLINK_ARCH_FLOAT_ABI_SHIFT       5

#define UDYNLINK_ARCH_FLOAT_ABI_SOFT        0
#define UDYNLINK_ARCH_FLOAT_ABI_SOFTFP      1
#define UDYNLINK_ARCH_FLOAT_ABI_HARD        2

#define UDYNLINK_ARCH_FLAG_NO_PROLOGUE    0x80

#define UDYNLINK_ARCH_TAG_CORTEX_M0         0x01
#define UDYNLINK_ARCH_TAG_CORTEX_M0PLUS     0x02
#define UDYNLINK_ARCH_TAG_CORTEX_M3         0x03
#define UDYNLINK_ARCH_TAG_CORTEX_M4         0x04
#define UDYNLINK_ARCH_TAG_CORTEX_M4F        0x54
#define UDYNLINK_ARCH_TAG_CORTEX_M7         0x57
#define UDYNLINK_ARCH_TAG_CORTEX_M33        0x08
#define UDYNLINK_ARCH_TAG_CORTEX_M55        0x59
#define UDYNLINK_ARCH_TAG_CORTEX_M85        0x5A

/**
 * @brief Check whether a module was built with the --no-prologue flag.
 *
 * No-prologue modules omit the assembly wrapper for exported functions.
 * The host must set r9 directly via UDYNLINK_PREPARE_CALL() before
 * calling any module function.
 *
 * @param[in] p_header Pointer to the module image header.
 * @return Non-zero if the no-prologue flag is set, 0 otherwise.
 */
static inline int udynlink_module_has_no_prologue(const udynlink_module_header_t *p_header) {
    return (p_header->arch_tag & UDYNLINK_ARCH_FLAG_NO_PROLOGUE) != 0;
}

/**
 * @brief Pack a major/minor version into a 16-bit value.
 * @param major Major version number.
 * @param minor Minor version number.
 */
#define UDYNLINK_MAKE_VERSION(major, minor)   (((major) << 8) | (minor))

/**
 * @brief Extract the major version from a packed version value.
 * @param v Packed version value.
 */
#define UDYNLINK_GET_MAJOR_VERSION(v)         (((v) >> 8) & 0xFF)

/**
 * @brief Extract the minor version from a packed version value.
 * @param v Packed version value.
 */
#define UDYNLINK_GET_MINOR_VERSION(v)         ((v) & 0xFF)

/**
 * @brief Prepare the LOT base and r9 before calling a module function.
 *
 * Writes @c p_mod->ram_base to ::UDYNLINK_LOT_BASE_ADDR so the module's
 * assembly prologue can load r9.  For modules built with --no-prologue,
 * also moves the RAM base directly into r9 via inline assembly.
 *
 * This macro is safe for both prologued and non-prologued modules.
 *
 * @param[in] p_mod Pointer to the loaded module handle.
 */
#define UDYNLINK_PREPARE_CALL(p_mod) do { \
    uint32_t *_mb = (uint32_t *)UDYNLINK_LOT_BASE_ADDR; \
    *_mb = (p_mod)->ram_base; \
    if (udynlink_module_has_no_prologue((p_mod)->p_header)) { \
        __asm volatile ("mov r9, %0" :: "r"((p_mod)->ram_base) : "r9"); \
    } \
} while(0)

////////////////////////////////////////////////////////////////////////////////
// Image builders

/**
 * @brief Populate an image descriptor from a contiguous memory buffer.
 *
 * Derives each section pointer from @p base_addr using the standard UDLM
 * layout.  The resulting descriptor is suitable for
 * udynlink_load_module_image() or for direct use with the low-level
 * relocation API.
 *
 * @param[in]  base_addr  Address of the module image in memory.
 * @param[out] out_image  Image descriptor to populate.
 */
void udynlink_image_from_memory(const void *base_addr, udynlink_module_image_t *out_image);

/**
 * @brief Populate an image descriptor from an already-loaded module.
 *
 * This is a convenience wrapper around udynlink_image_from_memory()
 * using the module's current header pointer.  It assumes the loaded
 * module image remains contiguous (true for all load modes).
 *
 * @param[in]  p_mod     Pointer to the loaded module handle.
 * @param[out] out_image Image descriptor to populate.
 */
void udynlink_image_from_module(const udynlink_module_t *p_mod, udynlink_module_image_t *out_image);

////////////////////////////////////////////////////////////////////////////////
// Public interface - planning / validation

/**
 * @brief Check architecture tag compatibility between a module and the host.
 *
 * Compares the core family and float-ABI encoded in @p mod_arch against
 * @p host_arch.  Returns ::UDYNLINK_OK if the module can safely run on the
 * host, or ::UDYNLINK_ERR_LOAD_ARCH_MISMATCH if the families differ or the
 * float-ABI requirements are incompatible.
 *
 * This is an optional pre-load check; udynlink_load_module() does not
 * perform it automatically.  Call it before loading if your application
 * cares about catching mismatches early.
 *
 * @param[in] mod_arch  Architecture tag from the module header (@c arch_tag).
 * @param[in] host_arch Architecture tag of the host (typically
 *                      ::UDYNLINK_HOST_ARCH_TAG).
 *
 * @return ::UDYNLINK_OK if compatible, ::UDYNLINK_ERR_LOAD_ARCH_MISMATCH otherwise.
 */
udynlink_error_t udynlink_check_arch_tag(uint16_t mod_arch, uint16_t host_arch);

/**
 * @brief Validate a module header.
 *
 * Checks signature and ABI version.  Architecture tag compatibility is
 * **not** checked here; call udynlink_check_arch_tag() separately.
 *
 * @param[in] header Pointer to the module header.
 *
 * @return ::UDYNLINK_OK if valid, or an error code.
 */
udynlink_error_t udynlink_validate_header(const udynlink_module_header_t *header);

/**
 * @brief Compute the RAM size required to load a module.
 *
 * @param[in] header Pointer to the module header.
 * @param[in] mode   Intended load mode.
 *
 * @return Required RAM size in bytes.
 */
size_t udynlink_compute_ram_size(const udynlink_module_header_t *header, udynlink_load_mode_t mode);

/**
 * @brief Return the size of module metadata (everything before the code section).
 *
 * This is the byte offset from the start of the module image to the
 * beginning of the code section.  For non-contiguous images, this is the
 * total size of the metadata buffers that must be provided.
 *
 * @param[in] header Pointer to the module header.
 *
 * @return Metadata size in bytes.
 */
size_t udynlink_get_image_metadata_size(const udynlink_module_header_t *header);

/**
 * @brief Get the module name from a symbol table.
 *
 * Only reads @p p_symtab; the caller does not need to provide a full
 * udynlink_module_image_t.  The string pool is assumed to be contiguous
 * with the symbol table entries.
 *
 * @param[in] p_symtab Pointer to the symbol table base.
 *
 * @return Pointer to the null-terminated module name, or NULL on error.
 */
const char *udynlink_image_get_module_name(const uint32_t *p_symtab);

/**
 * @brief Read dependency names from a module image without loading it.
 *
 * Only reads @p p_header (for @c num_deps) and @p deps_strtab.  The
 * caller does not need to provide a full udynlink_module_image_t.
 *
 * @param[in]  p_header     Pointer to the module header.
 * @param[in]  deps_strtab  Pointer to the dependency string table.
 * @param[out] deps         Array to receive dependency name pointers.
 *                           May be NULL if @p max_deps is 0.
 * @param[in]  max_deps     Maximum number of entries to write into @p deps.
 *
 * @return Total number of dependencies declared in the module image.
 *         This may be larger than @p max_deps if the array was too
 *         small.  Returns 0 if the image is invalid or has no
 *         dependencies.
 */
size_t udynlink_image_get_deps(const udynlink_module_header_t *p_header,
                               const char *deps_strtab,
                               const char **deps, size_t max_deps);

////////////////////////////////////////////////////////////////////////////////
// Public interface - module loading

/**
 * @brief Load a module from a memory-mapped image.
 *
 * Validates the header, checks ABI version, resolves dependencies, allocates
 * RAM, copies sections according to @p load_mode, applies relocations, and
 * resolves extern symbols via the host callbacks.
 *
 * Architecture tag compatibility is **not** checked by this function.  Call
 * udynlink_check_arch_tag() beforehand if you want to enforce it.
 *
 * @param[out] p_mod      Module handle to populate on success. Must be
 *                       zero-initialized by the caller before the first call
 *                       (e.g. via @c memset(p_mod, 0, sizeof(*p_mod))), or
 *                       the error-path cleanup may attempt to free garbage
 *                       pointers.
 * @param[in]  base_addr  Address of the module image in memory (e.g., flash).
 * @param[in]  load_addr  RAM address for the module, or NULL to auto-allocate.
 * @param[in]  load_size  Size of the region at @p load_addr (ignored if NULL).
 * @param[in]  load_mode  Copy mode (COPY_ALL, COPY_TEXT_DATA, or XIP).
 *
 * @return ::UDYNLINK_OK on success, or an error code on failure.
 *
 * @note When @p load_addr is NULL, the loader calls
 *       udynlink_external_malloc() to obtain RAM.
 * @note Before calling any function from the loaded module, the host
 *       must write @c p_mod->ram_base to ::UDYNLINK_LOT_BASE_ADDR.
 * @note Not thread-safe. The loader uses no locks or atomics. Concurrent
 *       calls from multiple interrupt levels will corrupt internal state.
 *       The host must provide synchronization around load/unload.
 */
udynlink_error_t udynlink_load_module(udynlink_module_t *p_mod, const void *base_addr, void *load_addr, size_t load_size, udynlink_load_mode_t load_mode);

/**
 * @brief Load a module from a non-contiguous image descriptor.
 *
 * Validates the header, resolves dependencies, allocates RAM, copies
 * sections according to @p load_mode, and applies relocations.
 *
 * For COPY_ALL mode, this function copies each section from the image
 * descriptor into a single contiguous RAM buffer.  For COPY_TEXT_DATA
 * and XIP modes, the metadata (header, relocations, symbol table) is
 * **not** copied; it must remain accessible via @c image->p_header for
 * post-load symbol lookups.  If your source metadata is not contiguous
 * with the header, use COPY_ALL.
 *
 * @param[out] p_mod      Module handle to populate on success. Must be
 *                       zero-initialized by the caller before the first call.
 * @param[in]  image      Module image descriptor with all section pointers
 *                       valid for the duration of the load.
 * @param[in]  load_addr  RAM address for the module, or NULL to auto-allocate.
 * @param[in]  load_size  Size of the region at @p load_addr (ignored if NULL).
 * @param[in]  load_mode  Copy mode (COPY_ALL, COPY_TEXT_DATA, or XIP).
 *
 * @return ::UDYNLINK_OK on success, or an error code on failure.
 */
udynlink_error_t udynlink_load_module_image(udynlink_module_t *p_mod,
    const udynlink_module_image_t *image,
    void *load_addr, size_t load_size,
    udynlink_load_mode_t load_mode);

/**
 * @brief Apply relocations to a module whose sections are already in RAM.
 *
 * This is a low-level primitive for custom loading pipelines.  The caller
 * must have already allocated RAM, copied code/data, zeroed BSS, and
 * populated @c p_mod->p_ram and @c p_mod->p_header.  This function reads
 * relocation and symbol data from the provided pointers and patches the
 * module's LOT and .data slots.
 *
 * @param[in,out] p_mod          Module handle with RAM and header set.
 * @param[in]     p_header       Module header (for num_rels, num_lot).
 * @param[in]     p_relocations  Relocation table pointer.
 * @param[in]     p_symtab       Symbol table pointer.
 *
 * @return ::UDYNLINK_OK on success, or an error code if a relocation
 *         references an out-of-range symbol or an unresolved extern.
 */
udynlink_error_t udynlink_load_apply_relocations(udynlink_module_t *p_mod,
    const udynlink_module_header_t *p_header,
    const uint32_t *p_relocations,
    const uint32_t *p_symtab);

/**
 * @brief Unload a previously loaded module.
 *
 * Releases the module's RAM (unless it was caller-supplied), decrements
 * dependency reference counts, and clears the module handle.
 *
 * @param[in] p_mod Pointer to the loaded module handle.
 *
 * @return ::UDYNLINK_OK on success, or ::UDYNLINK_ERR_INVALID_MODULE /
 *         ::UDYNLINK_ERR_MODULE_HAS_DEPENDENTS if the module is still referenced.
 *
 * @note Not thread-safe. Concurrent calls from different interrupt levels
 *       will corrupt dep_refcount. The host must provide synchronization.
 */
udynlink_error_t udynlink_unload_module(udynlink_module_t *p_mod);

/**
 * @brief Run C++ global constructors for a loaded module.
 *
 * Looks up the @c __init_array symbol and executes the constructor
 * chain.  The host must set ::UDYNLINK_LOT_BASE_ADDR to
 * @c p_mod->ram_base before calling this function.
 *
 * @param[in] p_mod Pointer to the loaded module handle.
 */
void udynlink_cpp_init(udynlink_module_t *p_mod);

/**
 * @brief Convert an error code to a human-readable string.
 *
 * @param[in] err Pointer to the error code.
 *
 * @return Static string describing the error.
 */
const char *udynlink_error_msg(udynlink_error_t* err);

/**
 * @brief Compute the RAM size required by a loaded module.
 *
 * The returned size includes the LOT, .data, .bss, and optionally the
 * code section depending on the load mode.
 *
 * @param[in] p_mod Pointer to the loaded module handle.
 *
 * @return Required RAM size in bytes.
 */
size_t udynlink_get_ram_size(const udynlink_module_t *p_mod);

/**
 * @brief Return the name of a loaded module.
 *
 * @param[in] p_mod Pointer to the loaded module handle.
 *
 * @return Pointer to the null-terminated module name, or NULL on error.
 */
const char *udynlink_get_module_name(const udynlink_module_t *p_mod);

/**
 * @brief Return the name of a module given its image base address.
 *
 * This is a convenience helper that does not require a fully loaded
 * module handle; it reads the name directly from the header.
 *
 * @param[in] base_addr Address of the module image.
 *
 * @return Pointer to the null-terminated module name, or NULL on error.
 */
const char *udynlink_get_module_name_from_image(const void *base_addr);

/**
 * @brief Read dependency names from a module image without loading it.
 *
 * Inspects the header of the module image at @p base_addr and extracts
 * the dependency names from the dependency string table.  Up to
 * @p max_deps pointers are written into the @p deps array.  The
 * returned pointers point into the module image and remain valid as
 * long as @p base_addr remains valid.
 *
 * @param[in]  base_addr Address of the module image.
 * @param[out] deps      Array to receive dependency name pointers.
 *                       May be NULL if @p max_deps is 0.
 * @param[in]  max_deps  Maximum number of entries to write into @p deps.
 *
 * @return Total number of dependencies declared in the module image.
 *         This may be larger than @p max_deps if the array was too
 *         small.  Returns 0 if the image is invalid or has no
 *         dependencies.
 */
size_t udynlink_get_module_deps(const void *base_addr, const char **deps, size_t max_deps);

/**
 * @brief Look up a symbol in a module.
 *
 * Searches the symbol table of @p p_mod for @p name.  If @p p_mod is
 * NULL the search is skipped (returns NULL).
 *
 * @param[in]  p_mod Pointer to the module to search, or NULL.
 * @param[in]  name  Null-terminated symbol name.
 * @param[out] p_sym Symbol structure to fill on success.
 *
 * @return @p p_sym if the symbol is found, NULL otherwise.
 */
udynlink_sym_t *udynlink_lookup_symbol(const udynlink_module_t *p_mod, const char *name, udynlink_sym_t *p_sym);

/**
 * @brief Get the value of a symbol.
 *
 * Convenience wrapper around udynlink_lookup_symbol().
 *
 * @param[in] p_mod Pointer to the module to search, or NULL.
 * @param[in] name  Null-terminated symbol name.
 *
 * @return The symbol's relocated value if found, 0 otherwise.
 */
uintptr_t udynlink_get_symbol_value(const udynlink_module_t *p_mod, const char *name);

/**
 * @brief Set the loader debug verbosity.
 *
 * @param[in] level Desired debug level.
 */
void udynlink_set_debug_level(udynlink_debug_level_t level);

/**
 * @brief Compute the total on-disk size of a module image.
 *
 * @param[in] base_addr Address of the module image.
 *
 * @return Total size in bytes (header + code + data), or 0 if the
 *         signature is invalid.
 */
size_t udynlink_get_image_size(const void *base_addr);

/**
 * @brief Get the pointer to the code (.text) memory.
 *
 * @param[in] p_mod Pointer to the loaded module handle.
 *
 * @return Pointer to the module's code section.
 */
uint8_t *udynlink_get_text_pointer(const udynlink_module_t *p_mod);

/**
 * @brief Return the RAM required to load a module from memory.
 *
 * @param[in] base_addr Address of the module image.
 * @param[in] mode      Intended load mode.
 *
 * @return Required RAM size in bytes.
 */
size_t udynlink_get_ram_requirements(const void *base_addr, udynlink_load_mode_t mode);

/**
 * @brief Incrementally resolve unresolved EXTERN relocations in a loaded module.
 *
 * Scans the module's relocation table and attempts to resolve only those
 * EXTERN slots that are currently zero. Already-resolved slots are left
 * untouched, so host fallback symbols are not overwritten by later
 * dependency symbols.
 *
 * The caller is responsible for ensuring the module's @c deps[] array is
 * populated with all desired dependency modules before calling this function.
 * To populate a deferred dependency, append its pointer to
 * @c p_mod->deps[p_mod->num_deps++], then increment the dependency module's
 * @c dep_refcount, and finally call this function.
 *
 * @param[in] p_mod Pointer to the loaded module.
 *
 * @return ::UDYNLINK_OK on success.
 */
udynlink_error_t udynlink_link_incremental(udynlink_module_t *p_mod);

/**
 * @brief Re-resolve all EXTERN relocations in a loaded module from scratch.
 *
 * Scans the module's relocation table and re-resolves every EXTERN slot
 * using the current @c deps[] array and the three-tier resolution chain.
 * This is slower than the incremental variant but ensures that dependency
 * module symbols take precedence over host fallback symbols.
 *
 * The caller is responsible for ensuring the module's @c deps[] array is
 * populated with all desired dependency modules before calling this function.
 * To populate a deferred dependency, append its pointer to
 * @c p_mod->deps[p_mod->num_deps++], then increment the dependency module's
 * @c dep_refcount, and finally call this function.
 *
 * @param[in] p_mod Pointer to the loaded module.
 *
 * @return ::UDYNLINK_OK on success.
 */
udynlink_error_t udynlink_relink_all(udynlink_module_t *p_mod);

/**
 * @brief Directly patch a symbol's relocation slot in a loaded module.
 *
 * Scans the module's relocation table for entries referencing @p sym_name and
 * overwrites the slot with @p sym_addr.  This is a low-level patch: it does
 * not update refcounts, deps, or the three-tier resolution chain.
 *
 * @param[in] p_mod   Pointer to the loaded module.
 * @param[in] sym_name Null-terminated symbol name.
 * @param[in] sym_addr Address to write into matching relocation slots.
 *
 * @return ::UDYNLINK_OK if at least one slot was patched,
 *         ::UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL if the symbol is not found.
 */
udynlink_error_t udynlink_link_symbol(udynlink_module_t *p_mod, const char *sym_name, uintptr_t sym_addr);

/**
 * @brief Check whether all declared dependencies of a module are linked.
 *
 * @param[in] p_mod Pointer to a loaded module.
 *
 * @return 1 if every dependency declared in the module header is present
 *         in p_mod->deps, 0 otherwise.
 */
int udynlink_is_module_fully_linked(const udynlink_module_t *p_mod);

/**
 * @brief Return the handle of a linked dependency by name.
 *
 * @param[in] p_mod    Pointer to a loaded module.
 * @param[in] dep_name Null-terminated dependency name.
 *
 * @return Pointer to the dependency module if it is linked into @p p_mod,
 *         or NULL if the dependency is not linked (either deferred or
 *         not declared).
 */
udynlink_module_t *udynlink_get_linked_dependency(const udynlink_module_t *p_mod, const char *dep_name);

/**
 * @brief Check whether an extern symbol has a non-zero resolved value.
 *
 * A symbol that was deferred during load and has not yet been linked
 * will resolve to 0 (or the host fallback value). After
 * udynlink_link_incremental() or udynlink_relink_all() resolves it from
 * a dependency module, this function returns 1.
 *
 * @param[in] p_mod    Pointer to a loaded module.
 * @param[in] sym_name Null-terminated symbol name.
 *
 * @return 1 if the symbol exists and its value is non-zero, 0 otherwise.
 */
int udynlink_is_symbol_resolved(const udynlink_module_t *p_mod, const char *sym_name);

#ifdef __cplusplus
}
#endif

#endif // #ifndef __UDYNLINK_H__
