#include "udynlink.h"
#include "mod_hello_module_data.h"
#include "test_utils.h"
#include <stdio.h>
#include <string.h>

static const uint8_t *g_stream_data;
static size_t g_stream_size;

static int32_t mock_read(void *pv_ctx, void *buf, size_t num_bytes, size_t offset) {
    (void)pv_ctx;
    if (offset >= g_stream_size) return -1;
    size_t avail = g_stream_size - offset;
    if (num_bytes > avail) num_bytes = avail;
    memcpy(buf, g_stream_data + offset, num_bytes);
    return (int32_t)num_bytes;
}

static int32_t mock_get_size(void *pv_ctx) {
    (void)pv_ctx;
    return (int32_t)g_stream_size;
}

// --- Test: NULL hooks (backward compatible) ---

static int test_null_hooks(void) {
    uint8_t scratch_buf[512];
    udynlink_io_t io = { mock_read, mock_get_size, NULL };
    udynlink_module_t mod;

    memset(&mod, 0, sizeof(mod));
    g_stream_data = mod_hello_module_data;
    g_stream_size = sizeof(mod_hello_module_data);

    udynlink_error_t err = udynlink_load_module_from_stream(&mod, &io, NULL, 0,
        UDYNLINK_LOAD_MODE_COPY_ALL, scratch_buf, sizeof(scratch_buf));
    if (err != UDYNLINK_OK) {
        printf("NULL hooks load failed: err=%d\n", err);
        return 0;
    }

    udynlink_sym_t sym;
    if (udynlink_lookup_symbol(&mod, "test", &sym) == NULL) {
        printf("lookup_symbol 'test' failed with NULL hooks\n");
        udynlink_unload_module(&mod);
        return 0;
    }

    udynlink_unload_module(&mod);
    printf("NULL hooks OK\n");
    return 1;
}

// --- Test: All hooks succeed, record stage order and p_mod state ---

typedef struct {
    udynlink_hook_stage_t stages[4];
    int stage_count;
    int header_parsed_ram_null;
    int deps_resolved_ram_set;
    int sections_loaded_code_valid;
    int relocs_applied_sym_resolved;
} hook_ctx_t;

static udynlink_error_t all_stages_hook(udynlink_hook_stage_t stage, udynlink_module_t *p_mod, void *pv_hook_ctx) {
    hook_ctx_t *ctx = (hook_ctx_t *)pv_hook_ctx;
    if (ctx->stage_count < 4) {
        ctx->stages[ctx->stage_count++] = stage;
    }

    switch (stage) {
        case UDYNLINK_HOOK_HEADER_PARSED:
            ctx->header_parsed_ram_null = (p_mod->p_ram == NULL);
            break;
        case UDYNLINK_HOOK_DEPS_RESOLVED:
            ctx->deps_resolved_ram_set = (p_mod->p_ram != NULL);
            break;
        case UDYNLINK_HOOK_SECTIONS_LOADED:
            // Verify code pointer is valid by checking it's non-NULL
            ctx->sections_loaded_code_valid = (udynlink_get_text_pointer(p_mod) != NULL);
            break;
        case UDYNLINK_HOOK_RELOCS_APPLIED:
            // Verify symbol resolution works post-reloc
            {
                udynlink_sym_t sym;
                ctx->relocs_applied_sym_resolved = (udynlink_lookup_symbol(p_mod, "test", &sym) != NULL);
            }
            break;
    }
    return UDYNLINK_OK;
}

static int test_all_stages(void) {
    uint8_t scratch_buf[512];
    udynlink_io_t io = { mock_read, mock_get_size, NULL };
    udynlink_module_t mod;
    hook_ctx_t ctx = {0};
    udynlink_load_hooks_t hooks = { all_stages_hook, &ctx };

    memset(&mod, 0, sizeof(mod));
    g_stream_data = mod_hello_module_data;
    g_stream_size = sizeof(mod_hello_module_data);

    udynlink_error_t err = udynlink_load_module_from_stream_ex(&mod, &io, NULL, 0,
        UDYNLINK_LOAD_MODE_COPY_ALL, scratch_buf, sizeof(scratch_buf), &hooks);
    if (err != UDYNLINK_OK) {
        printf("All-stages load failed: err=%d\n", err);
        return 0;
    }

    if (ctx.stage_count != 4) {
        printf("Expected 4 hook stages, got %d\n", ctx.stage_count);
        udynlink_unload_module(&mod);
        return 0;
    }
    if (ctx.stages[0] != UDYNLINK_HOOK_HEADER_PARSED ||
        ctx.stages[1] != UDYNLINK_HOOK_DEPS_RESOLVED ||
        ctx.stages[2] != UDYNLINK_HOOK_SECTIONS_LOADED ||
        ctx.stages[3] != UDYNLINK_HOOK_RELOCS_APPLIED) {
        printf("Hook stages out of order\n");
        udynlink_unload_module(&mod);
        return 0;
    }
    if (!ctx.header_parsed_ram_null) {
        printf("HEADER_PARSED: expected p_ram == NULL\n");
        udynlink_unload_module(&mod);
        return 0;
    }
    if (!ctx.deps_resolved_ram_set) {
        printf("DEPS_RESOLVED: expected p_ram != NULL\n");
        udynlink_unload_module(&mod);
        return 0;
    }
    if (!ctx.sections_loaded_code_valid) {
        printf("SECTIONS_LOADED: expected valid code pointer\n");
        udynlink_unload_module(&mod);
        return 0;
    }
    if (!ctx.relocs_applied_sym_resolved) {
        printf("RELOCS_APPLIED: expected symbol resolved\n");
        udynlink_unload_module(&mod);
        return 0;
    }

    udynlink_unload_module(&mod);
    printf("All stages OK (order, state checks passed)\n");
    return 1;
}

// --- Test: Abort at each stage ---

typedef struct {
    udynlink_hook_stage_t abort_stage;
    int hit;
} abort_ctx_t;

static udynlink_error_t abort_hook(udynlink_hook_stage_t stage, udynlink_module_t *p_mod, void *pv_hook_ctx) {
    (void)p_mod;
    abort_ctx_t *ctx = (abort_ctx_t *)pv_hook_ctx;
    if (stage == ctx->abort_stage) {
        ctx->hit = 1;
        return UDYNLINK_ERR_INVALID_MODULE; // arbitrary non-OK
    }
    return UDYNLINK_OK;
}

static int test_abort_at_stage(udynlink_hook_stage_t stage) {
    uint8_t scratch_buf[512];
    udynlink_io_t io = { mock_read, mock_get_size, NULL };
    udynlink_module_t mod;
    abort_ctx_t ctx = { stage, 0 };
    udynlink_load_hooks_t hooks = { abort_hook, &ctx };

    memset(&mod, 0, sizeof(mod));
    g_stream_data = mod_hello_module_data;
    g_stream_size = sizeof(mod_hello_module_data);

    udynlink_error_t err = udynlink_load_module_from_stream_ex(&mod, &io, NULL, 0,
        UDYNLINK_LOAD_MODE_COPY_ALL, scratch_buf, sizeof(scratch_buf), &hooks);
    if (err != UDYNLINK_ERR_LOAD_HOOK_ABORTED) {
        printf("Abort at stage %d: expected ERR_LOAD_HOOK_ABORTED, got %d\n", (int)stage, err);
        return 0;
    }
    if (!ctx.hit) {
        printf("Abort at stage %d: hook was not invoked\n", (int)stage);
        return 0;
    }
    // Verify module handle was cleaned up (zeroed)
    if (mod.p_header != NULL || mod.p_ram != NULL) {
        printf("Abort at stage %d: module handle not zeroed\n", (int)stage);
        return 0;
    }
    printf("Abort at stage %d OK\n", (int)stage);
    return 1;
}

// --- Test: Hook with COPY_TEXT_DATA mode ---

static int test_hooks_copy_text_data(void) {
    uint8_t scratch_buf[512];
    udynlink_io_t io = { mock_read, mock_get_size, NULL };
    udynlink_module_t mod;
    hook_ctx_t ctx = {0};
    udynlink_load_hooks_t hooks = { all_stages_hook, &ctx };

    memset(&mod, 0, sizeof(mod));
    g_stream_data = mod_hello_module_data;
    g_stream_size = sizeof(mod_hello_module_data);

    udynlink_error_t err = udynlink_load_module_from_stream_ex(&mod, &io, NULL, 0,
        UDYNLINK_LOAD_MODE_COPY_TEXT_DATA, scratch_buf, sizeof(scratch_buf), &hooks);
    if (err != UDYNLINK_OK) {
        printf("Hooks COPY_TEXT_DATA load failed: err=%d\n", err);
        return 0;
    }
    if (ctx.stage_count != 4) {
        printf("Expected 4 hook stages for COPY_TEXT_DATA, got %d\n", ctx.stage_count);
        udynlink_unload_module(&mod);
        return 0;
    }

    udynlink_unload_module(&mod);
    printf("Hooks COPY_TEXT_DATA OK\n");
    return 1;
}

int test_qemu(void) {
    int ok = 1;

    printf("== Streaming hooks: NULL hooks (backward compat) ==\n");
    ok = test_null_hooks() && ok;

    printf("== Streaming hooks: all stages succeed ==\n");
    ok = test_all_stages() && ok;

    printf("== Streaming hooks: abort at HEADER_PARSED ==\n");
    ok = test_abort_at_stage(UDYNLINK_HOOK_HEADER_PARSED) && ok;

    printf("== Streaming hooks: abort at DEPS_RESOLVED ==\n");
    ok = test_abort_at_stage(UDYNLINK_HOOK_DEPS_RESOLVED) && ok;

    printf("== Streaming hooks: abort at SECTIONS_LOADED ==\n");
    ok = test_abort_at_stage(UDYNLINK_HOOK_SECTIONS_LOADED) && ok;

    printf("== Streaming hooks: abort at RELOCS_APPLIED ==\n");
    ok = test_abort_at_stage(UDYNLINK_HOOK_RELOCS_APPLIED) && ok;

    printf("== Streaming hooks: COPY_TEXT_DATA with hooks ==\n");
    ok = test_hooks_copy_text_data() && ok;

    return ok;
}
