/**
 * @file udynlink_io_examples.h
 * @brief Example layered I/O wrappers for udynlink streaming load.
 *
 * These are **documentation-only examples** showing how to build custom
 * udynlink_io_t wrappers for byte-stream transformations (decompression,
 * decryption, ECC correction, etc.).  They are not compiled into the core
 * library and should be copied into your own firmware project.
 *
 * The udynlink loader itself never changes; you simply pass a wrapped
 * udynlink_io_t to udynlink_load_module_from_stream() or
 * udynlink_load_module_from_stream_ex().
 *
 * @note These examples assume block-based transforms (e.g. per-flash-page
 * compression, per-frame ECC).  Global streaming compression (single LZ4
 * stream over the whole image) does NOT work with the random-access
 * offset semantics of udynlink_read_cb_t.
 */

#ifndef __UDYNLINK_IO_EXAMPLES_H__
#define __UDYNLINK_IO_EXAMPLES_H__

#include "udynlink.h"
#include <string.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Example 1: Page-based decompression wrapper
//
// Use case: Module image is stored in flash with each 256-byte page
// compressed individually (e.g. with heatshrink or a custom RLE codec).
// The wrapper translates logical offsets into physical page indices,
// decompresses on demand, and caches one page to avoid redundant I/O.
// ---------------------------------------------------------------------------

typedef struct {
    /** Underlying raw I/O (e.g. flash or file system). */
    const udynlink_io_t *p_raw;
    /** Page size in the compressed domain (bytes). */
    uint32_t compressed_page_size;
    /** Page size in the decompressed domain (bytes). */
    uint32_t decompressed_page_size;
    /** Cached decompressed page buffer. */
    uint8_t page_buf[256];
    /** Index of the currently cached page (0xFFFFFFFF = none). */
    uint32_t cached_page;
} my_decompress_ctx_t;

/**
 * @brief Read callback for a page-based decompression wrapper.
 *
 * Translates the logical @p offset into a physical page index, reads the
 * compressed page from the underlying I/O, decompresses it into @c page_buf,
 * and copies the requested bytes into @p buf.
 *
 * @param pv_ctx    Pointer to a my_decompress_ctx_t.
 * @param buf       Destination buffer.
 * @param num_bytes Number of bytes to read.
 * @param offset    Logical byte offset within the decompressed stream.
 *
 * @return Number of bytes actually read, or -1 on error.
 */
static int32_t my_decompress_read(void *pv_ctx, void *buf, size_t num_bytes, size_t offset) {
    my_decompress_ctx_t *ctx = (my_decompress_ctx_t *)pv_ctx;

    // Bounds check against the uncompressed image size
    int32_t total_size = ctx->p_raw->get_size(ctx->p_raw->pv_ctx);
    if (total_size < 0) return -1;
    if (offset >= (size_t)total_size) return -1;
    if (num_bytes > (size_t)total_size - offset)
        num_bytes = (size_t)total_size - offset;

    uint8_t *out = (uint8_t *)buf;
    size_t copied = 0;

    while (copied < num_bytes) {
        uint32_t page_idx   = (uint32_t)((offset + copied) / ctx->decompressed_page_size);
        uint32_t page_off   = (uint32_t)((offset + copied) % ctx->decompressed_page_size);
        uint32_t chunk      = ctx->decompressed_page_size - page_off;
        if (chunk > num_bytes - copied) chunk = (uint32_t)(num_bytes - copied);

        // Cache miss: read and decompress the physical page
        if (page_idx != ctx->cached_page) {
            uint32_t phys_offset = page_idx * ctx->compressed_page_size;
            uint8_t  compressed[64]; // adjust to your max compressed page size
            if ((uint32_t)ctx->compressed_page_size > sizeof(compressed))
                return -1;

            int32_t n = ctx->p_raw->read(ctx->p_raw->pv_ctx, compressed,
                                         ctx->compressed_page_size, phys_offset);
            if (n < 0 || (uint32_t)n != ctx->compressed_page_size)
                return -1;

            // --- replace with your actual decompression routine ---
            // e.g. heatshrink_decoder_poll(...)
            // For this example we assume identity (no compression):
            memcpy(ctx->page_buf, compressed, ctx->decompressed_page_size);

            ctx->cached_page = page_idx;
        }

        memcpy(out + copied, ctx->page_buf + page_off, chunk);
        copied += chunk;
    }

    return (int32_t)copied;
}

/**
 * @brief Size callback for the decompression wrapper.
 *
 * Returns the **decompressed** image size.  In a real implementation you
 * would store the uncompressed size in a small header at the start of the
 * compressed blob and read it here, or compute it from the page count.
 */
static int32_t my_decompress_get_size(void *pv_ctx) {
    my_decompress_ctx_t *ctx = (my_decompress_ctx_t *)pv_ctx;
    int32_t raw_size = ctx->p_raw->get_size(ctx->p_raw->pv_ctx);
    if (raw_size < 0) return -1;
    // Example: raw_size is an exact multiple of compressed_page_size
    uint32_t num_pages = (uint32_t)raw_size / ctx->compressed_page_size;
    return (int32_t)(num_pages * ctx->decompressed_page_size);
}

// ---------------------------------------------------------------------------
// Example 2: Per-page ECC (error-correcting code) wrapper
//
// Use case: Module image is stored in a lossy medium (LoRa frames, NAND
// flash with bit flips).  Every 256-byte logical page is followed by a
// 4-byte XOR parity in the physical stream.  The wrapper reads page+parity,
// verifies/corrects, and returns the clean logical page.
// ---------------------------------------------------------------------------

typedef struct {
    const udynlink_io_t *p_raw;
    uint32_t logical_page_size;   // e.g. 256
    uint32_t parity_size;         // e.g. 4
    uint8_t page_buf[256];
    uint32_t cached_page;
} my_ecc_ctx_t;

/**
 * @brief Dummy ECC check-and-correct routine.
 *
 * In a real implementation this would run a Hamming code, RS code, or XOR
 * parity check over the page.  Returns 0 on success, -1 if uncorrectable.
 */
static int my_ecc_correct(uint8_t *page, uint32_t page_size, const uint8_t *parity) {
    (void)parity;
    // Placeholder: always succeeds
    // Real code: verify parity and flip bits if a single-bit error is detected.
    (void)page_size;
    (void)page;
    return 0;
}

static int32_t my_ecc_read(void *pv_ctx, void *buf, size_t num_bytes, size_t offset) {
    my_ecc_ctx_t *ctx = (my_ecc_ctx_t *)pv_ctx;

    int32_t total_size = ctx->p_raw->get_size(ctx->p_raw->pv_ctx);
    if (total_size < 0) return -1;

    // Compute logical image size (without parity bytes)
    uint32_t phys_page_size = ctx->logical_page_size + ctx->parity_size;
    uint32_t num_pages      = (uint32_t)total_size / phys_page_size;
    size_t   logical_size   = (size_t)num_pages * ctx->logical_page_size;

    if (offset >= logical_size) return -1;
    if (num_bytes > logical_size - offset)
        num_bytes = logical_size - offset;

    uint8_t *out = (uint8_t *)buf;
    size_t copied = 0;

    while (copied < num_bytes) {
        uint32_t page_idx = (uint32_t)((offset + copied) / ctx->logical_page_size);
        uint32_t page_off = (uint32_t)((offset + copied) % ctx->logical_page_size);
        uint32_t chunk    = ctx->logical_page_size - page_off;
        if (chunk > num_bytes - copied) chunk = (uint32_t)(num_bytes - copied);

        if (page_idx != ctx->cached_page) {
            uint32_t phys_offset = page_idx * phys_page_size;
            uint8_t scratch[260]; // logical_page_size + parity_size
            if (phys_page_size > sizeof(scratch)) return -1;

            int32_t n = ctx->p_raw->read(ctx->p_raw->pv_ctx, scratch, phys_page_size, phys_offset);
            if (n < 0 || (uint32_t)n != phys_page_size) return -1;

            if (my_ecc_correct(scratch, ctx->logical_page_size,
                               scratch + ctx->logical_page_size) != 0) {
                return -1; // uncorrectable error
            }

            memcpy(ctx->page_buf, scratch, ctx->logical_page_size);
            ctx->cached_page = page_idx;
        }

        memcpy(out + copied, ctx->page_buf + page_off, chunk);
        copied += chunk;
    }

    return (int32_t)copied;
}

static int32_t my_ecc_get_size(void *pv_ctx) {
    my_ecc_ctx_t *ctx = (my_ecc_ctx_t *)pv_ctx;
    int32_t raw_size = ctx->p_raw->get_size(ctx->p_raw->pv_ctx);
    if (raw_size < 0) return -1;
    uint32_t phys_page_size = ctx->logical_page_size + ctx->parity_size;
    uint32_t num_pages = (uint32_t)raw_size / phys_page_size;
    return (int32_t)(num_pages * ctx->logical_page_size);
}

// ---------------------------------------------------------------------------
// Example 3: Combining wrappers (decompression + ECC)
//
// Because each wrapper presents a clean udynlink_io_t interface, you can
// chain them.  Pass the ECC wrapper as the "raw" I/O to the decompressor,
// or vice-versa, depending on your physical layout.
//
//   physical flash  -->  ECC wrapper  -->  decompress wrapper  -->  loader
//
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Example 4: Usage in firmware
//
// static my_ecc_ctx_t ecc_ctx = {
//     .p_raw = &flash_io,
//     .logical_page_size = 256,
//     .parity_size = 4,
//     .cached_page = 0xFFFFFFFF,
// };
//
// static udynlink_io_t ecc_io = {
//     .read     = my_ecc_read,
//     .get_size = my_ecc_get_size,
//     .pv_ctx   = &ecc_ctx,
// };
//
// udynlink_module_t mod;
// uint8_t scratch[512];
// udynlink_error_t err = udynlink_load_module_from_stream(
//     &mod, &ecc_io, NULL, 0,
//     UDYNLINK_LOAD_MODE_COPY_ALL, scratch, sizeof(scratch));
//
// ---------------------------------------------------------------------------

#endif // __UDYNLINK_IO_EXAMPLES_H__
