#include "udynlink.h"
#include "mod_hello_module_data.h"
#include "test_utils.h"
#include <stdio.h>
#include <string.h>

static const uint8_t *g_stream_data;
static uint32_t g_stream_size;

static int32_t mock_read(void *pv_ctx, void *buf, uint32_t num_bytes, uint32_t offset) {
    (void)pv_ctx;
    if (offset >= g_stream_size) return -1;
    uint32_t avail = g_stream_size - offset;
    if (num_bytes > avail) num_bytes = avail;
    memcpy(buf, g_stream_data + offset, num_bytes);
    return (int32_t)num_bytes;
}

static int32_t mock_get_size(void *pv_ctx) {
    (void)pv_ctx;
    return (int32_t)g_stream_size;
}

static int test_streaming_load(uint32_t work_buf_size, udynlink_load_mode_t mode) {
    uint8_t work_buf[512];
    udynlink_io_t io = { mock_read, mock_get_size, NULL };
    udynlink_module_t mod;
    int res = 0;

    g_stream_data = mod_hello_module_data;
    g_stream_size = sizeof(mod_hello_module_data);

    if (work_buf_size > sizeof(work_buf)) work_buf_size = sizeof(work_buf);

    udynlink_error_t err = udynlink_load_module_stream(&mod, &io, NULL, 0, mode, work_buf, work_buf_size);
    if (err != UDYNLINK_OK) {
        printf("Streaming load failed: err=%d mode=%d wbsz=%u\n", err, (int)mode, work_buf_size);
        return 0;
    }

    {
        uint32_t* mod_base = (uint32_t*)UDYNLINK_LOT_BASE_ADDR;
        *mod_base = mod.ram_base;

        udynlink_sym_t sym;
        if (udynlink_lookup_symbol(&mod, "test", &sym) == NULL) {
            printf("lookup_symbol 'test' failed (mode=%d wbsz=%u)\n", (int)mode, work_buf_size);
            goto exit;
        }
        int (*p_func)(void) = (int (*)(void))sym.val;
        if (!p_func()) {
            printf("Module 'test' function returned 0 (mode=%d wbsz=%u)\n", (int)mode, work_buf_size);
            goto exit;
        }
    }

    res = 1;
exit:
    udynlink_unload_module(&mod);
    return res;
}

static int test_xip_rejected(void) {
    uint8_t work_buf[128];
    udynlink_io_t io = { mock_read, mock_get_size, NULL };
    udynlink_module_t mod;

    g_stream_data = mod_hello_module_data;
    g_stream_size = sizeof(mod_hello_module_data);

    if (udynlink_load_module_stream(&mod, &io, NULL, 0,
                                     UDYNLINK_LOAD_MODE_XIP, work_buf, sizeof(work_buf))
        != UDYNLINK_ERR_LOAD_UNABLE_TO_XIP) {
        printf("XIP streaming should have been rejected\n");
        return 0;
    }
    return 1;
}

static int test_ram_requirements_stream(void) {
    udynlink_io_t io = { mock_read, mock_get_size, NULL };

    g_stream_data = mod_hello_module_data;
    g_stream_size = sizeof(mod_hello_module_data);

    uint32_t ram_copy_all = udynlink_get_ram_requirements_stream(&io, UDYNLINK_LOAD_MODE_COPY_ALL);
    uint32_t ram_copy_code = udynlink_get_ram_requirements_stream(&io, UDYNLINK_LOAD_MODE_COPY_CODE);

    if (ram_copy_all == 0) {
        printf("ram_requirements_stream COPY_ALL returned 0\n");
        return 0;
    }
    if (ram_copy_code == 0) {
        printf("ram_requirements_stream COPY_CODE returned 0\n");
        return 0;
    }
    printf("ram COPY_ALL=%u COPY_CODE=%u\n", ram_copy_all, ram_copy_code);
    return 1;
}

static int test_stream_metadata_size(void) {
    udynlink_io_t io = { mock_read, mock_get_size, NULL };

    g_stream_data = mod_hello_module_data;
    g_stream_size = sizeof(mod_hello_module_data);

    uint32_t wbsz = udynlink_get_stream_metadata_size(&io);
    if (wbsz == 0) {
        printf("get_stream_metadata_size returned 0\n");
        return 0;
    }
    printf("stream metadata_size=%u\n", wbsz);
    return 1;
}

static int test_ram_requirements_compat(void) {
    uint32_t ram_mmap = udynlink_get_ram_requirements(mod_hello_module_data, UDYNLINK_LOAD_MODE_COPY_ALL);
    udynlink_io_t io = { mock_read, mock_get_size, NULL };

    g_stream_data = mod_hello_module_data;
    g_stream_size = sizeof(mod_hello_module_data);

    uint32_t ram_stream = udynlink_get_ram_requirements_stream(&io, UDYNLINK_LOAD_MODE_COPY_ALL);
    if (ram_mmap != ram_stream) {
        printf("ram_requirements mismatch: mmap=%u stream=%u\n", ram_mmap, ram_stream);
        return 0;
    }
    return 1;
}

int test_qemu(void) {
    int ok = 1;

    printf("== Streaming: COPY_ALL, 512B work_buf ==\n");
    ok = test_streaming_load(512, UDYNLINK_LOAD_MODE_COPY_ALL) && ok;

    printf("== Streaming: COPY_ALL, 64B work_buf ==\n");
    ok = test_streaming_load(64, UDYNLINK_LOAD_MODE_COPY_ALL) && ok;

    printf("== Streaming: COPY_ALL, 128B work_buf ==\n");
    ok = test_streaming_load(128, UDYNLINK_LOAD_MODE_COPY_ALL) && ok;

    printf("== Streaming: COPY_CODE, 512B work_buf ==\n");
    ok = test_streaming_load(512, UDYNLINK_LOAD_MODE_COPY_CODE) && ok;

    printf("== Streaming: COPY_CODE, 64B work_buf ==\n");
    ok = test_streaming_load(64, UDYNLINK_LOAD_MODE_COPY_CODE) && ok;

    printf("== Streaming: XIP rejection ==\n");
    ok = test_xip_rejected() && ok;

    printf("== Streaming: ram_requirements_stream ==\n");
    ok = test_ram_requirements_stream() && ok;

    printf("== Streaming: stream_work_buf_size ==\n");
    ok = test_stream_metadata_size() && ok;

    printf("== Streaming: ram_requirements compat ==\n");
    ok = test_ram_requirements_compat() && ok;

    return ok;
}
