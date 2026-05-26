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
    _UDYNLINK_LOAD_MODE_FIRST = UDYNLINK_LOAD_MODE_COPY_ALL, /* for testing only */
    /** Copy code and data into RAM; leave the header at @c base_addr. */
    UDYNLINK_LOAD_MODE_COPY_CODE,
    /** Execute code in place (XIP); copy only data into RAM. */
    UDYNLINK_LOAD_MODE_XIP,
    _UDYNLINK_LOAD_MODE_LAST = UDYNLINK_LOAD_MODE_XIP /* for testing only */
} udynlink_load_mode_t;

#ifndef UDYNLINK_MAX_DEPS
/** Maximum number of dependencies a single module may declare. */
#define UDYNLINK_MAX_DEPS 4
#endif

/**
 * @brief Runtime module handle.
 *
 * Holds the loader's internal state for one loaded module instance.
 */
typedef struct _udynlink_module_t {
    /** Pointer to the module header (in flash or RAM depending on load mode). */
    const udynlink_module_header_t *p_header;
    /** Anonymous union of RAM base pointer and its numeric representation. */
    union {
        /** Pointer to the module's RAM region (LOT, data, bss, optional code). */
        void *p_ram;
        /** Same address as an unsigned 32-bit integer. */
        uint32_t ram_base;
    };
    /** Bitmask storing the load mode and RAM ownership flags. */
    uint8_t info;
    /** Number of successfully resolved dependencies. */
    uint8_t num_deps;
    /** Number of other loaded modules that list this module as a dependency. */
    uint8_t dep_refcount;
    /** Array of pointers to dependency modules. */
    const struct _udynlink_module_t *deps[UDYNLINK_MAX_DEPS];
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
    uint32_t val;
    /** Symbol type (see ::UDYNLINK_SYM_TYPE_LOCAL et al.). */
    uint8_t type;
    /** Memory location (code or data, see ::UDYNLINK_SYM_LOCATION_CODE). */
    uint8_t location;
} udynlink_sym_t;

/** Static (module-local) symbol. */
#define UDYNLINK_SYM_TYPE_LOCAL               0
/** Exported symbol (visible to other modules and the host). */
#define UDYNLINK_SYM_TYPE_EXPORTED            1
/** Extern symbol (unresolved at link time; resolved by the host at load time). */
#define UDYNLINK_SYM_TYPE_EXTERN              2
/** Special symbol representing the module name. */
#define UDYNLINK_SYM_TYPE_NAME                3

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
_UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_UNABLE_TO_XIP),\
_UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_NO_MORE_HANDLES),\
_UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_INVALID_MODE),\
_UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE),\
_UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL),\
_UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_DUPLICATE_NAME),\
_UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_VERSION_MISMATCH),\
_UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_ARCH_MISMATCH),\
_UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_MISSING_DEP),\
_UDYNLINK_EXPAND(UDYNLINK_ERR_LOAD_IO_ERROR),\
_UDYNLINK_EXPAND(UDYNLINK_ERR_MODULE_IN_USE),\
_UDYNLINK_EXPAND(UDYNLINK_ERR_INVALID_MODULE)

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

#ifndef UDYNLINK_MAX_HANDLES
/** Must be defined by the host before including this header. */
#error "UDYNLINK_MAX_HANDLES must be defined before including udynlink.h"
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

/** Internal macro used to emit debug messages with file context. */
#define UDYNLINK_DEBUG(...)                   udynlink_debug(__func__, __LINE__, __VA_ARGS__)

////////////////////////////////////////////////////////////////////////////////
// Streaming I/O interface

/**
 * @brief Streaming read callback.
 *
 * Reads up to @p num_bytes bytes starting at @p offset into @p buf.
 *
 * @param pv_ctx    Opaque context pointer supplied by the caller.
 * @param buf       Destination buffer.
 * @param num_bytes Number of bytes to read.
 * @param offset    Byte offset within the stream.
 *
 * @return Number of bytes actually read, or -1 on error.
 */
typedef int32_t (*udynlink_read_cb_t)(void *pv_ctx, void *buf, uint32_t num_bytes, uint32_t offset);

/**
 * @brief Streaming size query callback.
 *
 * @param pv_ctx Opaque context pointer supplied by the caller.
 *
 * @return Total image size in bytes, or -1 on error.
 */
typedef int32_t (*udynlink_get_size_cb_t)(void *pv_ctx);

/**
 * @brief Streaming I/O descriptor.
 *
 * Passed to udynlink_load_module_stream() to abstract the module
 * image source (e.g., a file system, serial flash, or network buffer).
 */
typedef struct {
    /** Read callback. */
    udynlink_read_cb_t      read;
    /** Size query callback. */
    udynlink_get_size_cb_t  get_size;
    /** Opaque context forwarded to both callbacks. */
    void                   *pv_ctx;
} udynlink_io_t;

#ifndef UDYNLINK_STREAM_BUF_SIZE
/** Default scratch-buffer size for streaming loads (512 bytes, matches FatFS sector size). */
#define UDYNLINK_STREAM_BUF_SIZE 512
#endif

/** Minimum work-buffer size for udynlink_load_module_stream() (64 bytes). */
#define UDYNLINK_STREAM_MIN_WORK_BUF_SIZE 64

////////////////////////////////////////////////////////////////////////////////
// Public interface

/**
 * @brief Load a module from a memory-mapped image.
 *
 * Validates the header, checks ABI version and architecture tag
 * compatibility, resolves dependencies, allocates RAM, copies sections
 * according to @p load_mode, applies relocations, and resolves extern
 * symbols via the host callbacks.
 *
 * @param[out] p_mod      Module handle to populate on success.
 * @param[in]  base_addr  Address of the module image in memory (e.g., flash).
 * @param[in]  load_addr  RAM address for the module, or NULL to auto-allocate.
 * @param[in]  load_size  Size of the region at @p load_addr (ignored if NULL).
 * @param[in]  load_mode  Copy mode (COPY_ALL, COPY_CODE, or XIP).
 *
 * @return ::UDYNLINK_OK on success, or an error code on failure.
 *
 * @note When @p load_addr is NULL, the loader calls
 *       udynlink_external_malloc() to obtain RAM.
 * @note Before calling any function from the loaded module, the host
 *       must write @c p_mod->ram_base to ::UDYNLINK_LOT_BASE_ADDR.
 */
udynlink_error_t udynlink_load_module(udynlink_module_t *p_mod, const void *base_addr, void *load_addr, uint32_t load_size, udynlink_load_mode_t load_mode);

/**
 * @brief Unload a previously loaded module.
 *
 * Releases the module's RAM (unless it was caller-supplied), decrements
 * dependency reference counts, and clears the module handle.
 *
 * @param[in] p_mod Pointer to the loaded module handle.
 *
 * @return ::UDYNLINK_OK on success, or ::UDYNLINK_ERR_INVALID_MODULE /
 *         ::UDYNLINK_ERR_MODULE_IN_USE if the module is still referenced.
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
uint32_t udynlink_get_ram_size(const udynlink_module_t *p_mod);

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
const char *udynlink_get_module_name2(const void *base_addr);

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
uint32_t udynlink_get_symbol_value(const udynlink_module_t *p_mod, const char *name);

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
uint32_t udynlink_get_module_size(const void *base_addr);

/**
 * @brief Get the pointer to the code (.text) memory.
 *
 * @param[in] p_mod Pointer to the loaded module handle.
 *
 * @return Pointer to the module's code section.
 */
uint8_t *udynlink_get_code_pointer(const udynlink_module_t *p_mod);

/**
 * @brief Load a module from a streaming I/O source.
 *
 * Similar to udynlink_load_module() but reads the image incrementally
 * via callbacks.  XIP mode is not supported and returns
 * ::UDYNLINK_ERR_LOAD_UNABLE_TO_XIP.
 *
 * @param[out] p_mod          Module handle to populate on success.
 * @param[in]  p_io           Streaming I/O callbacks.
 * @param[in]  load_addr      RAM address for the module, or NULL to auto-allocate.
 * @param[in]  load_size      Size of the region at @p load_addr (ignored if NULL).
 * @param[in]  load_mode      COPY_ALL or COPY_CODE only.
 * @param[in]  work_buf       Caller-provided scratch buffer.
 * @param[in]  work_buf_size  Size of @p work_buf (minimum 64 bytes).
 *
 * @return ::UDYNLINK_OK on success, or an error code on failure.
 */
udynlink_error_t udynlink_load_module_stream(udynlink_module_t *p_mod,
    const udynlink_io_t *p_io, void *load_addr, uint32_t load_size,
    udynlink_load_mode_t load_mode, void *work_buf, uint32_t work_buf_size);

/**
 * @brief Return the RAM required to load a module from memory.
 *
 * @param[in] base_addr Address of the module image.
 * @param[in] mode      Intended load mode.
 *
 * @return Required RAM size in bytes.
 */
uint32_t udynlink_get_ram_requirements(const void *base_addr, udynlink_load_mode_t mode);

/**
 * @brief Return the RAM required to load a module from a stream.
 *
 * Reads only the header from the stream to compute the size.
 *
 * @param[in] p_io   Streaming I/O callbacks.
 * @param[in] mode   Intended load mode.
 *
 * @return Required RAM size in bytes, or 0 on I/O error.
 */
uint32_t udynlink_get_ram_requirements_stream(const udynlink_io_t *p_io, udynlink_load_mode_t mode);

/**
 * @brief Return the size of module metadata (everything before the code section).
 *
 * This is the byte offset from the start of the module image to the
 * beginning of the code section. It encompasses the header, relocation
 * table, symbol table, dependency string table, and any padding.
 * A work buffer of at least this size allows the streaming loader to
 * read all metadata in a single @c read() callback, minimizing I/O overhead.
 *
 * @param[in] p_io Streaming I/O callbacks.
 *
 * @return Metadata size in bytes (byte offset to code section), or 0 on I/O error.
 */
uint32_t udynlink_get_stream_metadata_size(const udynlink_io_t *p_io);

#ifdef __cplusplus
}
#endif

#endif // #ifndef __UDYNLINK_H__
