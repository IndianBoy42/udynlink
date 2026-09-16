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
 *   - Section table (only when ::UDYNLINK_HDR_FLAG_SECTIONS is set;
 *     num_sections * 24 bytes of entries, rounded up to 4)
 *   - Section payloads, concatenated in ascending VA order (BSS has no
 *     payload): code, data, then tagged sections
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
    /**
     * Header flags.  Bit 0 (::UDYNLINK_HDR_FLAG_SECTIONS) marks an image
     * that carries a section table, in which case bits 7:1 hold the
     * section count (1..63).  Bits 15:8 and — for images without the
     * flag — bits 7:1 are reserved and must be 0.  Modules built without
     * section placement keep this field 0 (it was a reserved field before
     * loader ABI 3.1).
     */
    uint16_t flags;
    /** Size of the symbol table in bytes. */
    uint32_t symt_size;
    /** Size of the code (.text) section in bytes. */
    uint32_t code_size;
    /** Size of the initialized data (.data) section in bytes. */
    uint32_t data_size;
    /** Size of the zero-initialized data (.bss) section in bytes. */
    uint32_t bss_size;
    /* Then relocations (num_rels * 8 bytes) */
    /* Then the symbol table (symt_size bytes, rounded up to 4) */
    /* Then the section table entries, when UDYNLINK_HDR_FLAG_SECTIONS is set */
    /* Then the section payloads (ascending VA order, BSS absent) */
} udynlink_module_header_t;

/** Header @c flags bit 0: the image carries a section table (loader ABI 3.1+). */
#define UDYNLINK_HDR_FLAG_SECTIONS              0x0001
/** Mask of the section-count field in @c flags bits 7:1 (valid only when
 *  ::UDYNLINK_HDR_FLAG_SECTIONS is set; the count itself is 1..63). */
#define UDYNLINK_HDR_SECTIONS_MASK              0x00FE
/** Number of sections declared by a sectioned image header. */
#define UDYNLINK_HDR_NUM_SECTIONS(flags)        (((flags) & UDYNLINK_HDR_SECTIONS_MASK) >> 1)
/* Header flags bits 15:8 are reserved and must be 0 (as are bits 7:1 when
 * the section-table flag is clear). */

/** Section class: executable code (copied to its resolved address in every load mode). */
#define UDYNLINK_SEC_CLASS_CODE                 0
/** Section class: initialized data. */
#define UDYNLINK_SEC_CLASS_DATA                 1
/** Section class: zero-initialized data (no image payload). */
#define UDYNLINK_SEC_CLASS_BSS                  2

/**
 * Section hint flags, bits 7:0 — the loader-defined vocabulary (v1).  Bits
 * 15:8 are host-defined pass-through bits the loader never interprets, bits
 * 23:16 are reserved (must be 0), and bit 31 (::UDYNLINK_SEC_FLAG_MAIN) is
 * loader-internal.  Hints are inputs to host placement policy; the loader
 * never acts on them beyond passing them to the allocator callbacks.
 */
#define UDYNLINK_SEC_FLAG_NOCACHE               0x01
/** Section must be reachable by the DMA controller (e.g. not DTCM/ITCM). */
#define UDYNLINK_SEC_FLAG_DMA                   0x02
/** Section may share memory with other modules or host code. */
#define UDYNLINK_SEC_FLAG_SHARED                0x04
/* Bits 7:3 are free for future loader vocabulary. */
/** Section lives in the module's main RAM block (loader-internal; masked
 *  out before the flags reach the host allocator). */
#define UDYNLINK_SEC_FLAG_MAIN                  0x80000000u


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
    /** Module header (32 bytes). Only @c p_header is read by functions that take the full descriptor. */
    const udynlink_module_header_t *p_header;
    /** Relocation table: num_rels * 2 * uint32_t. */
    const uint32_t *p_relocations;
    /** Symbol table base (first word = entry count). The string pool is assumed contiguous with entries. */
    const uint32_t *p_symtab;
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
    /** Reserved for future use. */
    uint8_t reserved;
    /** Reserved for future use. */
    uint16_t reserved2;
    /**
     * Number of named (searchable) symbol entries in the module's symbol
     * table, starting at index 1.  The symbol table is sorted
     * lexicographically so that udynlink_lookup_symbol can use binary
     * search over indices [1, num_named_syms].  Local (nameless) symbols
     * occupy the remaining entries after num_named_syms.  Computed once at
     * load time.
     */
    uint16_t num_named_syms;
    /** Opaque user context pointer.  Never read or written by the loader;
     *  provided for the host to associate arbitrary state with a module
     *  handle (e.g., a filesystem path, a reference counter, or a
     *  higher-level language runtime handle). */
    void *user_ctx;
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
 * INTERNAL/EXPORTED), then attempts host override via
 * udynlink_external_resolve_symbol().  If no override is found the
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
/* Append-only: the values are public ABI and documented by index in
 * docs/api-reference.md. Never insert in the middle or reorder. */
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
_UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_IO_ERROR),\
_UDYNLINK_EXPAND(UDYNLINK_ERR_INVALID_MODULE),\
_UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_HOOK_ABORTED),\
_UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_RAM_UNALIGNED),\
_UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_SECTION_UNRESOLVED),\
_UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_SECTION_UNALIGNED),\
_UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_BAD_SECTION_TABLE)

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

/** ABI version of this loader (3.1 — adds section placement; 3.0 images load unchanged). */
#define UDYNLINK_LOADER_ABI_VERSION           UDYNLINK_MAKE_VERSION(3, 1)

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
 * @brief Set r9 to the module's RAM base before calling a module function.
 *
 * The module's assembly prologue (if present) saves the caller's r9 on
 * the stack and restores it after the call.  For --no-prologue modules,
 * the host must ensure r9 is set and restored manually, or use
 * UDYNLINK_CALL() which handles save/restore automatically.
 *
 * @param[in] p_mod Pointer to the loaded module handle.
 */
#ifndef UDYNLINK_PREPARE_CALL
/* Provided as a function-like macro so a host build that never executes ARM
 * module code (e.g. the loader fuzz/sanitizer harnesses in tests/fuzz) can
 * override this on the compile line with a no-op body. On Cortex-M this
 * emits the r9 load required by the PIC code model. */
#define UDYNLINK_PREPARE_CALL(p_mod) do { \
    __asm volatile ("mov r9, %0" :: "r"((uint32_t)(p_mod)->ram_base) : "r9"); \
} while(0)
#endif

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
 * Returns the size of the module's **main RAM block** only: LOT, section
 * base array (sectioned images), metadata (COPY_ALL), and the main
 * .text/.data/.bss sections including their alignment padding.  Tagged
 * sections are allocated separately by the loader; size their pools from
 * udynlink_get_section_count()/udynlink_get_section_info().
 *
 * For a module built without section placement this is today's exact
 * layout: no padding, no base array.
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
 * beginning of the code section.  For sectioned images the section table is
 * part of the metadata.  For non-contiguous images, this is the
 * total size of the metadata buffers that must be provided.
 *
 * @note Header-only: no byte beyond the 32-byte header is dereferenced, so
 *       the function is safe on truncated buffers.
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
 * @brief Return the number of symbol entries in a module image's symbol table.
 *
 * The count includes the module-name entry at index 0.  Use it with
 * udynlink_image_get_symbol() to enumerate a module's symbols before loading
 * it (e.g. to inspect exports, check for a required symbol, or log a
 * manifest).  Only @p image->p_header and @p image->p_symtab are read.
 *
 * The returned count is bounded by what the (attacker-controlled) @c
 * symt_size can actually hold, so a malformed image claiming a huge entry
 * count cannot drive a host into billions of iterations.
 *
 * @param[in] image Module image descriptor (p_header and p_symtab valid).
 *
 * @return Number of symbol entries, or 0 on error / empty table.
 */
size_t udynlink_image_get_symbol_count(const udynlink_module_image_t *image);

/**
 * @brief Retrieve a symbol descriptor by index from a module image.
 *
 * Returns the raw, **unrelocated** symbol: @c val is an offset within the
 * symbol's section (code or data), not an absolute address.  No RAM has been
 * allocated and no relocations applied, so this is safe to call before
 * udynlink_load_module() / udynlink_load_module_image().  For relocated
 * absolute values, load the module and use udynlink_get_symbol().
 *
 * Index 0 is the module name (::UDYNLINK_SYM_TYPE_MODULE_NAME); indices
 * [1, num_named_syms] are the sorted, named (searchable) symbols; the
 * remaining entries are local (nameless, @c "(N/A)") symbols.
 *
 * @param[in]  image Module image descriptor.
 * @param[in]  index Symbol index in [0, udynlink_image_get_symbol_count()-1].
 * @param[out] p_sym Symbol descriptor to fill.
 *
 * @return @p p_sym on success, NULL if @p index is out of range, the image
 *         is malformed, or an argument is NULL.
 */
const udynlink_sym_t *udynlink_image_get_symbol(const udynlink_module_image_t *image, size_t index, udynlink_sym_t *p_sym);

////////////////////////////////////////////////////////////////////////////////
// Public interface - module loading

/**
 * @brief Load a module from a memory-mapped image.
 *
 * Validates the header, checks ABI version, allocates RAM, copies sections
 * according to @p load_mode, applies relocations, and resolves extern symbols
 * via the host callback.
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
 *                       Must contain the complete image: for sectioned
 *                       images the section table and tagged payloads extend
 *                       past what the header sizes alone describe.
 * @param[in]  load_addr  RAM address for the module's main block, or NULL
 *                        to auto-allocate. Must be aligned to the module's
 *                        main-block alignment (word-aligned for modules
 *                        built without section placement); a misaligned
 *                        address fails the load with
 *                        ::UDYNLINK_ERR_LOAD_RAM_UNALIGNED.
 * @param[in]  load_size  Size of the region at @p load_addr (ignored if NULL).
 * @param[in]  load_mode  Copy mode (COPY_ALL, COPY_TEXT_DATA, or XIP).
 *
 * @return ::UDYNLINK_OK on success, or an error code on failure.
 *
 * @note When @p load_addr is NULL, the loader calls
 *       udynlink_external_malloc() for the main block and once per tagged
 *       section (sectioned images require loader ABI 3.1; see
 *       udynlink/udynlink_externals.h).
 * @note Before calling any function from the loaded module, the host
 *       must set r9 to @c p_mod->ram_base via UDYNLINK_PREPARE_CALL().
 * @note Not thread-safe. The loader uses no locks or atomics. Concurrent
 *       calls from multiple interrupt levels will corrupt internal state.
 *       The host must provide synchronization around load/unload.
 * @note Alignment: modules built without section placement get only word
 *       (4-byte) alignment — the data area starts at p_ram + num_lot * 4,
 *       so __attribute__((aligned(N))) for N > 4 is not honored at runtime.
 *       A sectioned image declares per-section alignments; the loader
 *       aligns each main section inside the block and validates each
 *       tagged section base returned by the allocator
 *       (::UDYNLINK_ERR_LOAD_SECTION_UNALIGNED).
 */
udynlink_error_t udynlink_load_module(udynlink_module_t *p_mod, const void *base_addr, void *load_addr, size_t load_size, udynlink_load_mode_t load_mode);

/**
 * @brief Load a module from a non-contiguous image descriptor.
 *
 * Validates the header, allocates RAM, copies sections according to
 * @p load_mode, and applies relocations.
 *
 * For COPY_ALL mode, this function copies the metadata (header, relocations,
 * symbol table, and — for sectioned images — the section table) plus each
 * section payload from the image descriptor into a single contiguous RAM
 * buffer.  For COPY_TEXT_DATA and XIP modes, the metadata (header,
 * relocations, symbol table, section table) is **not** copied; it must
 * remain accessible via @c image->p_header for post-load symbol lookups.
 * If your source metadata is not contiguous with the header, use COPY_ALL.
 *
 * For a sectioned image, the section payloads must be laid out contiguously
 * after @c image->p_code in ascending VA order (code, data, then tagged
 * sections; BSS has no payload) — exactly the layout
 * udynlink_image_from_memory() produces.  Tagged sections are always
 * allocated through udynlink_external_malloc() and copied/zeroed
 * individually, in every load mode.
 *
 * @param[out] p_mod      Module handle to populate on success. Must be
 *                       zero-initialized by the caller before the first call.
 * @param[in]  image      Module image descriptor with all section pointers
 *                       valid for the duration of the load.
 * @param[in]  load_addr  RAM address for the module's main block, or NULL to
 *                        auto-allocate. Must be aligned to the module's
 *                        main-block alignment (word-aligned for modules
 *                        built without section placement); a misaligned
 *                        address fails the load with
 *                        ::UDYNLINK_ERR_LOAD_RAM_UNALIGNED.
 * @param[in]  load_size  Size of the region at @p load_addr (ignored if NULL).
 * @param[in]  load_mode  Copy mode (COPY_ALL, COPY_TEXT_DATA, or XIP).
 *
 * @return ::UDYNLINK_OK on success, or an error code on failure.
 *
 * @note Alignment: modules built without section placement get only word
 *       (4-byte) alignment — the data area starts at p_ram + num_lot * 4,
 *       so __attribute__((aligned(N))) for N > 4 is not honored at runtime.
 *       A sectioned image declares per-section alignments; the loader
 *       aligns each main section inside the block and validates each
 *       tagged section base returned by the allocator
 *       (::UDYNLINK_ERR_LOAD_SECTION_UNALIGNED).
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

////////////////////////////////////////////////////////////////////////////////
// Public interface - section placement

/**
 * @brief Descriptor for one section of a module image.
 *
 * Filled by udynlink_get_section_info().  @c name points into the module
 * image's symbol string pool and is valid as long as the image (or, for
 * COPY_ALL loads, the loader's RAM copy of its metadata) is alive.
 */
typedef struct {
    /** Section name, or NULL for unnamed sections and for the three
     *  implicit main sections of a module built without section placement. */
    const char *name;
    /** Section size in bytes (multiple of 4). */
    size_t      size;
    /** Section alignment in bytes (power of two, >= 4). */
    size_t      align;
    /** Hint flags (::UDYNLINK_SEC_FLAG_NOCACHE et al); ::UDYNLINK_SEC_FLAG_MAIN
     *  is masked out.  Bits 15:8 are host-defined pass-through. */
    uint32_t    flags;
    /** One of ::UDYNLINK_SEC_CLASS_CODE / _DATA / _BSS. Named @c sec_class
     *  because @c class is a keyword in C++. */
    uint8_t     sec_class;
    /** Reserved; zeroed. */
    uint8_t     reserved[3];
} udynlink_section_info_t;

/**
 * @brief Number of sections declared by a module image.
 *
 * Modules built without section placement have three implicit sections
 * (.text, .data, .bss) with the indices 0/1/2; a sectioned image reports the
 * number of entries in its section table (the three main sections keep
 * indices 0/1/2, tagged sections follow in ascending VA order).
 *
 * @param[in] p_header Pointer to the module image header.
 *
 * @return Section count, or 0 if @p p_header is NULL or its section table
 *         is malformed.
 */
size_t udynlink_get_section_count(const udynlink_module_header_t *p_header);

/**
 * @brief Describe one section of a module image.
 *
 * Works both before a load (on an image header) and after one (on the
 * loaded module's header).  For untagged modules the three implicit main
 * sections are reported with @c name == NULL, @c align == 4 and empty flags.
 *
 * @param[in]  p_header Pointer to the module image header.
 * @param[in]  idx      Section index in [0, udynlink_get_section_count()-1].
 * @param[out] out      Descriptor to fill.
 *
 * @return ::UDYNLINK_OK on success, ::UDYNLINK_ERR_INVALID_MODULE for NULL
 *         arguments or an out-of-range @p idx, or
 *         ::UDYNLINK_ERR_LOAD_BAD_SECTION_TABLE if the section table is
 *         malformed.
 */
udynlink_error_t udynlink_get_section_info(const udynlink_module_header_t *p_header, size_t idx,
                                           udynlink_section_info_t *out);

/**
 * @brief Runtime base address of a loaded module's section.
 *
 * The DMA use case: the host asks for the address of the section holding a
 * tagged buffer.  Indexing matches udynlink_get_section_info() (0/1/2 are
 * the main .text/.data/.bss sections; in XIP mode the main .text base points
 * into the source image).
 *
 * @param[in] p_mod Pointer to the loaded module handle.
 * @param[in] idx   Section index.
 *
 * @return Section base address, or NULL if @p p_mod is not loaded or
 *         @p idx is out of range.
 */
void *udynlink_get_section_base(const udynlink_module_t *p_mod, size_t idx);

/**
 * @brief One tagged-section move for udynlink_relocate_module_sections().
 */
typedef struct {
    /** Section index (>= 3; main sections move only with the main block). */
    size_t idx;
    /** New base address for the section (already populated by the host). */
    void *new_base;
} udynlink_section_move_t;

/**
 * @brief Relocate a loaded module's main RAM block and listed tagged sections.
 *
 * Behaves like udynlink_relocate_module() for the main block (LOT, in-RAM
 * code, main .data/.bss, and — for COPY_ALL — the in-RAM metadata are copied
 * to @p new_ram and rebased), and additionally re-binds the tagged sections
 * listed in @p moves:
 *
 * - The host must have **already copied each moved section's payload** to
 *   @c new_base before calling; the loader never copies host-owned blocks.
 *   The copied contents must be byte-identical to the pre-move contents:
 *   the loader re-applies relocations from the image's relocation table and
 *   reads/patches relocation slots in the moved data at their new addresses.
 * - Relocations whose target or referenced value lies in a moved section are
 *   re-applied using that section's move delta (new_base - old base); all
 *   other sections contribute a delta of 0, so sections that did not move
 *   keep their contents untouched unless they point into a moved section.
 * - The loader does not free the old storage of a moved section: both the
 *   old and the new block belong to the host, which moved it.
 *
 * EXTERN slots and host-overridden weak slots keep their host-absolute
 * values.  As with udynlink_relocate_module(), every symbol address the host
 * previously obtained (including udynlink_get_section_base() results) is
 * stale after a successful call.
 *
 * @param[in,out] p_mod     Loaded module handle.
 * @param[in]     new_ram   Destination for the main block, or NULL to
 *                          auto-allocate through udynlink_external_malloc()
 *                          with the module's main-block alignment.
 * @param[in]     new_size  Size of @p new_ram (ignored when NULL); must be
 *                          >= udynlink_get_ram_size(p_mod).
 * @param[in]     moves     Array of section moves (may be NULL if
 *                          @p num_moves is 0).
 * @param[in]     num_moves Number of entries in @p moves.
 *
 * @return ::UDYNLINK_OK on success, or
 *         ::UDYNLINK_ERR_INVALID_MODULE (bad handle, a move targeting a main
 *         section, a duplicate section index, or moves requested on a module
 *         built without section placement),
 *         ::UDYNLINK_ERR_LOAD_RAM_LEN_LOW (caller buffer too small),
 *         ::UDYNLINK_ERR_LOAD_OUT_OF_MEMORY (auto-alloc returned NULL),
 *         ::UDYNLINK_ERR_LOAD_RAM_UNALIGNED (caller buffer violates the
 *         main-block alignment),
 *         ::UDYNLINK_ERR_LOAD_SECTION_UNRESOLVED (a move's @c new_base is
 *         NULL),
 *         ::UDYNLINK_ERR_LOAD_SECTION_UNALIGNED (a move's @c new_base
 *         violates the section's declared alignment).
 *
 * @note Not thread-safe; the host must serialize with load/unload and must
 *       not move a section while module code is executing.
 */
udynlink_error_t udynlink_relocate_module_sections(udynlink_module_t *p_mod,
                                                   void *new_ram, size_t new_size,
                                                   const udynlink_section_move_t *moves,
                                                   size_t num_moves);


/**
 * @brief Unload a previously loaded module.
 *
 * Releases the module's RAM — the main block (unless it was
 * caller-supplied) and every tagged section the loader allocated — and
 * clears the module handle.  Free callbacks receive the same (section,
 * align, flags) arguments the allocation used.
 *
 * @param[in] p_mod Pointer to the loaded module handle.
 *
 * @return ::UDYNLINK_OK on success, or ::UDYNLINK_ERR_INVALID_MODULE.
 *
 * @note Not thread-safe. Concurrent calls from different interrupt levels
 *       will corrupt internal state. The host must provide synchronization.
 */
udynlink_error_t udynlink_unload_module(udynlink_module_t *p_mod);
/**
 * @brief Relocate an already-loaded module's RAM region to a new buffer.
 *
 * Copies the module's entire contiguous RAM region (LOT, in-RAM code, .data,
 * .bss, and — for COPY_ALL — the in-RAM metadata) to @p new_ram, then rebases
 * every internal absolute pointer (LOT entries for module code/data symbols,
 * R_ARM_ABS32 data-section pointers, code-base data-section pointers, weak
 * module-default slots) by the move delta. EXTERN slots and host-overridden
 * weak slots are left untouched (they hold host-absolute addresses). Runtime
 * .data/.bss values and already-resolved extern slots are preserved.
 *
 * The load mode is unchanged. For XIP the code stays in flash (code-delta 0);
 * only the LOT/data/bss RAM block moves.
 *
 * Tagged sections of a sectioned module are absolute and unaffected — they
 * stay where they are, so moving just the main block remains valid (use
 * udynlink_relocate_module_sections() to move tagged sections as well).
 *
 * @param[out] p_mod      Loaded module handle (must already be loaded).
 * @param[in]  new_ram    Destination RAM buffer, or NULL to auto-allocate via
 *                        udynlink_external_malloc() with the module's
 *                        main-block alignment.
 * @param[in]  new_size   Size of the buffer at @p new_ram (ignored when NULL);
 *                        must be >= udynlink_get_ram_size(p_mod).
 *
 * @note A module's RAM size is fixed per mode; @p new_size may be larger
 *       than needed (extra space unused) but never smaller than required.
 * @note Invalidation contract (inherent to moving code/data in RAM): every
 *       symbol address previously returned to the host by
 *       udynlink_lookup_symbol()/udynlink_get_symbol_value() is STALE after a
 *       successful relocate — re-resolve before calling. Cross-module thunks
 *       and dependency gateways (udynlink_thunk/udynlink_deps) embed the old
 *       ram_base as a literal and must be torn down and rebuilt after
 *       relocate. Do not relocate a module while it is executing or while r9
 *       points at it. Not thread-safe; host must serialize with load/unload.
 */
udynlink_error_t udynlink_relocate_module(udynlink_module_t *p_mod,
                                          void *new_ram, size_t new_size);


/**
 * @brief Run C++ global constructors for a loaded module.
 *
 * Looks up the @c __init_array symbol and executes the constructor
 * chain.  The host must set r9 to @c p_mod->ram_base (via
 * UDYNLINK_PREPARE_CALL()) before calling this function.
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
 * The returned size is the module's main RAM block: LOT, section base array
 * (sectioned images), metadata (COPY_ALL), and the main .text/.data/.bss
 * sections with their alignment padding.  Tagged sections live outside this
 * block; enumerate them with udynlink_get_section_count() /
 * udynlink_get_section_base().
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
 * @brief Return the number of symbol entries in a loaded module's symbol table.
 *
 * The count includes the module-name entry at index 0.  Pair with
 * udynlink_get_symbol() to enumerate the loaded module's symbols.  This is
 * the post-load counterpart of udynlink_image_get_symbol_count(); unlike that
 * function it reads the symbol table through the loaded module's header, so
 * it works for every load mode (the symbol table is always reachable via @c
 * p_mod->p_header).
 *
 * @param[in] p_mod Pointer to the loaded module handle, or NULL.
 *
 * @return Number of symbol entries, or 0 if @p p_mod is NULL / not loaded.
 */
size_t udynlink_get_symbol_count(const udynlink_module_t *p_mod);

/**
 * @brief Retrieve a symbol descriptor by index from a loaded module.
 *
 * Fills @p p_sym with the **relocated** symbol: for INTERNAL, EXPORTED, and
 * WEAK symbols @c val is the absolute address within the module's loaded
 * code or data section (the module's own definition is used for WEAK symbols;
 * a host override, if any, lives in the LOT slot and is reported by
 * udynlink_lookup_symbol()).  For EXTERN and MODULE_NAME symbols @c val is
 * the raw table value (offset / name token), matching
 * udynlink_lookup_symbol().
 *
 * This is a pure enumeration of the on-disk symbol table: it applies no host
 * callback and has no side effects, so it is safe to call at any time after a
 * successful load (including from ISRs, unlike the loader's load/unload
 * paths).
 *
 * @param[in]  p_mod Pointer to the loaded module handle.
 * @param[in]  index Symbol index in [0, udynlink_get_symbol_count()-1].
 * @param[out] p_sym Symbol descriptor to fill.
 *
 * @return @p p_sym on success, NULL if @p index is out of range, @p p_mod is
 *         NULL/not loaded, or @p p_sym is NULL.
 */
const udynlink_sym_t *udynlink_get_symbol(const udynlink_module_t *p_mod, size_t index, udynlink_sym_t *p_sym);

/**
 * @brief Set the loader debug verbosity.
 *
 * @param[in] level Desired debug level.
 */
void udynlink_set_debug_level(udynlink_debug_level_t level);

/**
 * @brief Compute the total on-disk size of a module image.
 *
 * For a module built without section placement this is the exact image
 * size (header + relocation table + symbol table + code + data).
 *
 * For a sectioned image this covers everything except the **tagged section
 * payloads**: the header tables (including the section table, whose extent
 * the header's section count fully determines) plus the main code/data
 * payloads.  Tagged payloads are described only by the section table —
 * size buffers holding sectioned images from the source (file size, array
 * length) rather than from this function.
 *
 * Only the 32-byte header is dereferenced; @p base_addr must point at
 * least at a complete header.
 *
 * @return Total size in bytes, or 0 if the signature is invalid.
 */
size_t udynlink_get_image_size(const void *base_addr);

/**
 * @brief Compute the total image size without reading past @p avail bytes.
 *
 * Where ::udynlink_get_image_size() cannot see tagged-section payloads (it
 * dereferences only the header), this returns the true total — the header
 * tables, the main payloads, and the tagged CODE/DATA payloads whose sizes
 * the section table declares.
 *
 * It is safe on untrusted input: every read is bounded by @p avail.  The
 * table itself is only read once the lower bound from
 * ::udynlink_get_image_size() has been shown to lie inside @p avail, which
 * places the whole metadata block — section table included — inside the
 * caller's buffer.
 *
 * @param[in] base_addr Module image start.
 * @param[in] avail     Bytes the caller guarantees at @p base_addr.
 *
 * @return Total image size in bytes; 0 when it cannot be established within
 *         @p avail (truncated buffer, malformed header, or an inconsistent
 *         section table), in which case the image must not be loaded.
 */
size_t udynlink_get_image_size_bounded(const void *base_addr, size_t avail);

/**
 * @brief Get the pointer to the main .text memory.
 *
 * For sectioned images this is the main (.text) code section; tagged code
 * sections live outside the main block and are reachable via
 * udynlink_get_section_base().
 *
 * @param[in] p_mod Pointer to the loaded module handle.
 *
 * @return Pointer to the module's main code section.
 */
uint8_t *udynlink_get_text_pointer(const udynlink_module_t *p_mod);

/**
 * @brief Return the RAM required to load a module from memory.
 *
 * Main RAM block only — see udynlink_compute_ram_size().  Tagged sections
 * of a sectioned image are allocated separately; size their pools from
 * udynlink_get_section_count()/udynlink_get_section_info().
 *
 * @param[in] base_addr Address of the module image (must be a complete
 *                      image when the section-table flag is set).
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
 * untouched.
 *
 * Resolution is performed via udynlink_external_resolve_symbol() only.
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
 * using udynlink_external_resolve_symbol().
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
 * not update any loader-internal state beyond the relocation slot itself.
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
 * @brief Check whether an extern symbol has a non-zero resolved value.
 *
 * A symbol that was deferred during load and has not yet been linked
 * will resolve to 0. After udynlink_link_incremental() or
 * udynlink_relink_all() resolves it, this function returns 1.
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
