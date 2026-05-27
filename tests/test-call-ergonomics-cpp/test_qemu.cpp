#include "udynlink.hpp"
#include "mod_callergo_cpp_module_data.h"
#include "test_utils.h"
#include <stdio.h>
#include <utility>

extern "C" int test_qemu(void) {
    int res = 0;

    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i++) {
        // Test 1: Module RAII + resolve + call
        {
            udynlink::Module mod;
            udynlink_error_t err = mod.load(
                mod_callergo_cpp_module_data, NULL, 0,
                (udynlink_load_mode_t)i
            );
            if (err != UDYNLINK_OK) {
                printf("Module::load failed: %d\n", (int)err);
                goto exit;
            }
            if (!mod.is_loaded()) {
                printf("Module::is_loaded() false after load\n");
                goto exit;
            }
            CHECK_RAM_SIZE(mod.handle(), 0);

            auto h_add = mod.resolve<int(int, int)>("add");
            if (!h_add) {
                printf("resolve add failed\n");
                goto exit;
            }
            auto h_magic = mod.resolve<int(void)>("get_magic");
            if (!h_magic) {
                printf("resolve get_magic failed\n");
                goto exit;
            }

            int r1 = h_add(2, 3);
            if (r1 != 5) {
                printf("add(2,3) returned %d, expected 5\n", r1);
                goto exit;
            }

            int r2 = h_magic();
            if (r2 != 0xCAFE) {
                printf("get_magic() returned 0x%04X, expected 0xCAFE\n", r2);
                goto exit;
            }

            // Test 2: error path - unknown symbol
            auto h_missing = mod.resolve<int(void)>("nonexistent");
            if (h_missing) {
                printf("Expected resolve to fail for missing symbol\n");
                goto exit;
            }
            if (h_missing.address() != 0) {
                printf("Expected address 0 for missing symbol\n");
                goto exit;
            }

            // Verify calling a missing Func returns default-constructed value
            int r_missing = h_missing();
            if (r_missing != 0) {
                printf("Expected 0 from unresolved Func, got %d\n", r_missing);
                goto exit;
            }

            // Test 3: run the module's own test via C++ wrapper
            auto h_test = mod.resolve<int(void)>("test");
            if (!h_test) {
                printf("resolve test failed\n");
                goto exit;
            }
            if (!h_test()) {
                printf("Module self-test failed\n");
                goto exit;
            }

            // Test 4: Context RAII for batch calls
            {
                udynlink::Context ctx(mod.handle());
                int r3 = h_add(10, 20);
                if (r3 != 30) {
                    printf("Context add(10,20) returned %d, expected 30\n", r3);
                    goto exit;
                }
                int r4 = h_magic();
                if (r4 != 0xCAFE) {
                    printf("Context get_magic() returned 0x%04X, expected 0xCAFE\n", r4);
                    goto exit;
                }
            }

            // Test 5: Context::rebind to a second module instance
            {
                udynlink::Module mod2;
                err = mod2.load(
                    mod_callergo_cpp_module_data, NULL, 0,
                    (udynlink_load_mode_t)i
                );
                if (err != UDYNLINK_OK) {
                    printf("mod2::load failed: %d\n", (int)err);
                    goto exit;
                }

                udynlink::Context ctx(mod.handle());
                auto h2 = mod2.resolve<int(int, int)>("add");
                if (!h2) {
                    printf("mod2 resolve add failed\n");
                    goto exit;
                }

                // rebind to mod2
                ctx.rebind(mod2.handle());
                int r5 = h2(7, 8);
                if (r5 != 15) {
                    printf("rebind add(7,8) returned %d, expected 15\n", r5);
                    goto exit;
                }

                // mod unloads automatically on scope exit (RAII)
            }

            // Test 6: move semantics
            {
                udynlink::Module mod_src;
                err = mod_src.load(
                    mod_callergo_cpp_module_data, NULL, 0,
                    (udynlink_load_mode_t)i
                );
                if (err != UDYNLINK_OK) {
                    printf("mod_src::load failed: %d\n", (int)err);
                    goto exit;
                }

                udynlink::Module mod_dst(std::move(mod_src));
                if (!mod_dst.is_loaded()) {
                    printf("Move-constructed module not loaded\n");
                    goto exit;
                }
                if (mod_src.is_loaded()) {
                    printf("Move-source module still loaded\n");
                    goto exit;
                }

                auto h_move = mod_dst.resolve<int(int, int)>("add");
                int r6 = h_move(100, 1);
                if (r6 != 101) {
                    printf("move add(100,1) returned %d, expected 101\n", r6);
                    goto exit;
                }

                udynlink::Module mod_assign;
                mod_assign = std::move(mod_dst);
                if (!mod_assign.is_loaded()) {
                    printf("Move-assigned module not loaded\n");
                    goto exit;
                }
                if (mod_dst.is_loaded()) {
                    printf("Move-assign source still loaded\n");
                    goto exit;
                }

                auto h_assign = mod_assign.resolve<int(int, int)>("add");
                int r7 = h_assign(50, 7);
                if (r7 != 57) {
                    printf("move-assign add(50,7) returned %d, expected 57\n", r7);
                    goto exit;
                }
            }

            // mod unloads automatically on scope exit
        }
    }

    res = 1;
exit:
    return res;
}
