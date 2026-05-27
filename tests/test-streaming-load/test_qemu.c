#include "udynlink.h"
#include "mod_hello_module_data.h"
#include "test_utils.h"
#include <stdio.h>
#include <string.h>

static int test_load_module_image(udynlink_load_mode_t mode) {
    udynlink_module_image_t image;
    udynlink_image_from_memory(mod_hello_module_data, &image);
    udynlink_module_t mod;
    int res = 0;

    udynlink_error_t err = udynlink_load_module_image(&mod, &image, NULL, 0, mode);
    if (err != UDYNLINK_OK) {
        printf("load_module_image failed: err=%d mode=%d\n", err, (int)mode);
        return 0;
    }

    {
        udynlink_sym_t sym;
        if (udynlink_lookup_symbol(&mod, "test", &sym) == NULL) {
            printf("lookup_symbol 'test' failed (mode=%d)\n", (int)mode);
            goto exit;
        }
        int (*p_func)(void) = (int (*)(void))sym.val;
        UDYNLINK_PREPARE_CALL(&mod);
        if (!p_func()) {
            printf("Module 'test' function returned 0 (mode=%d)\n", (int)mode);
            goto exit;
        }
    }

    res = 1;
exit:
    udynlink_unload_module(&mod);
    return res;
}

static int test_xip(void) {
    udynlink_module_image_t image;
    udynlink_image_from_memory(mod_hello_module_data, &image);
    udynlink_module_t mod;

    udynlink_error_t err = udynlink_load_module_image(&mod, &image, NULL, 0,
        UDYNLINK_LOAD_MODE_XIP);
    if (err != UDYNLINK_OK) {
        printf("XIP load failed: err=%d\n", err);
        return 0;
    }
    udynlink_unload_module(&mod);
    return 1;
}

static int test_ram_requirements_image(void) {
    udynlink_module_header_t *header = (udynlink_module_header_t *)mod_hello_module_data;
    size_t ram_copy_all = udynlink_compute_ram_size(header, UDYNLINK_LOAD_MODE_COPY_ALL);
    size_t ram_copy_code = udynlink_compute_ram_size(header, UDYNLINK_LOAD_MODE_COPY_TEXT_DATA);

    if (ram_copy_all == 0) {
        printf("compute_ram_size COPY_ALL returned 0\n");
        return 0;
    }
    if (ram_copy_code == 0) {
        printf("compute_ram_size COPY_CODE returned 0\n");
        return 0;
    }
    printf("ram COPY_ALL=%zu COPY_CODE=%zu\n", ram_copy_all, ram_copy_code);
    return 1;
}

static int test_image_metadata_size(void) {
    udynlink_module_header_t *header = (udynlink_module_header_t *)mod_hello_module_data;
    size_t wbsz = udynlink_get_image_metadata_size(header);
    if (wbsz == 0) {
        printf("get_image_metadata_size returned 0\n");
        return 0;
    }
    printf("image metadata_size=%zu\n", wbsz);
    return 1;
}

static int test_ram_requirements_compat(void) {
    size_t ram_mmap = udynlink_get_ram_requirements(mod_hello_module_data, UDYNLINK_LOAD_MODE_COPY_ALL);
    udynlink_module_header_t *header = (udynlink_module_header_t *)mod_hello_module_data;
    size_t ram_image = udynlink_compute_ram_size(header, UDYNLINK_LOAD_MODE_COPY_ALL);
    if (ram_mmap != ram_image) {
        printf("ram_requirements mismatch: mmap=%zu image=%zu\n", ram_mmap, ram_image);
        return 0;
    }
    return 1;
}

static int test_validate_header(void) {
    udynlink_module_header_t *header = (udynlink_module_header_t *)mod_hello_module_data;
    if (udynlink_validate_header(header) != UDYNLINK_OK) {
        printf("validate_header failed for valid image\n");
        return 0;
    }

    udynlink_module_header_t bad = *header;
    bad.sign = 0xDEADBEEF;
    if (udynlink_validate_header(&bad) != UDYNLINK_ERR_LOAD_INVALID_SIGN) {
        printf("validate_header did not reject bad sign\n");
        return 0;
    }
    return 1;
}

static int test_image_get_module_name(void) {
    udynlink_module_image_t image;
    udynlink_image_from_memory(mod_hello_module_data, &image);
    const char *name = udynlink_image_get_module_name(image.p_symtab);
    if (!name || strcmp(name, "mod_hello") != 0) {
        printf("image_get_module_name returned wrong name: %s\n", name ? name : "(null)");
        return 0;
    }
    return 1;
}

int test_qemu(void) {
    int ok = 1;

    printf("== Image load: COPY_ALL ==\n");
    ok = test_load_module_image(UDYNLINK_LOAD_MODE_COPY_ALL) && ok;

    printf("== Image load: COPY_TEXT_DATA ==\n");
    ok = test_load_module_image(UDYNLINK_LOAD_MODE_COPY_TEXT_DATA) && ok;

    printf("== Image load: XIP ==\n");
    ok = test_xip() && ok;

    printf("== Image load: compute_ram_size ==\n");
    ok = test_ram_requirements_image() && ok;

    printf("== Image load: metadata_size ==\n");
    ok = test_image_metadata_size() && ok;

    printf("== Image load: ram_requirements compat ==\n");
    ok = test_ram_requirements_compat() && ok;

    printf("== Image load: validate_header ==\n");
    ok = test_validate_header() && ok;

    printf("== Image load: get_module_name ==\n");
    ok = test_image_get_module_name() && ok;

    return ok;
}
