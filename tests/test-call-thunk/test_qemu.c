#include "udynlink.h"
#include "udynlink_thunk.h"
#include "mod_math_module_data.h"
#include "test_utils.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define THUNK_POOL_SIZE 512

static uint8_t g_thunk_buf[THUNK_POOL_SIZE];
static udynlink_thunk_pool_t g_thunk_pool;

static int test_call_thunk_single(udynlink_load_mode_t mode) {
    udynlink_module_t mod_math;
    int ok = 0;

    memset(&mod_math, 0, sizeof(mod_math));
    udynlink_thunk_pool_init(&g_thunk_pool, g_thunk_buf, THUNK_POOL_SIZE);

    if (test_load_module(&mod_math, mod_math_module_data, NULL, 0, mode) != UDYNLINK_OK) {
        printf("mod_math load failed\n");
        return 0;
    }

    {
        uintptr_t thunk = udynlink_thunk_make_call(&g_thunk_pool, &mod_math, "math_add");
        if (thunk == 0) {
            printf("thunk for math_add failed\n");
            goto cleanup;
        }
        int (*p_add)(int, int) = (int (*)(int, int))thunk;
        int r = p_add(1, 2);
        if (r != 3) {
            printf("thunked math_add(1,2) = %d, expected 3\n", r);
            goto cleanup;
        }
        printf("thunked math_add(1, 2) = %d\n", r);
    }

    {
        uintptr_t thunk = udynlink_thunk_make_call(&g_thunk_pool, &mod_math, "math_mul");
        if (thunk == 0) {
            printf("thunk for math_mul failed\n");
            goto cleanup;
        }
        int (*p_mul)(int, int) = (int (*)(int, int))thunk;
        int r = p_mul(3, 4);
        if (r != 12) {
            printf("thunked math_mul(3,4) = %d, expected 12\n", r);
            goto cleanup;
        }
        printf("thunked math_mul(3, 4) = %d\n", r);
    }

    {
        size_t expected_used = 2 * UDYNLINK_STUB_SIZE;
        if (g_thunk_pool.used != expected_used) {
            printf("pool used = %u, expected %u\n", g_thunk_pool.used, expected_used);
            goto cleanup;
        }
        printf("pool used = %u (2 stubs, 1 gateway)\n", g_thunk_pool.used);
    }

    {
        size_t expected_gateway_top = THUNK_POOL_SIZE - UDYNLINK_GATEWAY_SIZE;
        if (g_thunk_pool.gateway_top != expected_gateway_top) {
            printf("pool gateway_top = %u, expected %u\n", g_thunk_pool.gateway_top, expected_gateway_top);
            goto cleanup;
        }
        printf("pool gateway_top = %u (1 gateway)\n", g_thunk_pool.gateway_top);
    }

    {
        uintptr_t thunk2 = udynlink_thunk_make_call(&g_thunk_pool, &mod_math, "math_add");
        if (thunk2 == 0) {
            printf("2nd thunk for math_add failed\n");
            goto cleanup;
        }
        int (*p_add)(int, int) = (int (*)(int, int))thunk2;
        int r = p_add(5, 6);
        if (r != 11) {
            printf("thunked math_add(5,6) = %d, expected 11\n", r);
            goto cleanup;
        }
        size_t expected_used = 2 * UDYNLINK_STUB_SIZE;
        if (g_thunk_pool.used != expected_used) {
            printf("stub dedup failed: pool used = %u, expected %u\n", g_thunk_pool.used, expected_used);
            goto cleanup;
        }
        printf("stub dedup: math_add thunk reused, pool used = %u\n", g_thunk_pool.used);
    }

    printf("call thunk: OK\n");
    ok = 1;

cleanup:
    test_unload_module(&mod_math);
    return ok;
}

int test_qemu(void) {
    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i++) {
        if (!test_call_thunk_single((udynlink_load_mode_t)i))
            return 0;
    }
    return 1;
}
