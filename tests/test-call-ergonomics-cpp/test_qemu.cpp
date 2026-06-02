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
                mod_callergo_cpp_module_data, nullptr, 0,
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

            int r1 = (*h_add)(2, 3);
            if (r1 != 5) {
                printf("add(2,3) returned %d, expected 5\n", r1);
                goto exit;
            }

            int r2 = (*h_magic)();
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

            // Test 3: run the module's own test via C++ wrapper
            auto h_test = mod.resolve<int(void)>("test");
            if (!h_test) {
                printf("resolve test failed\n");
                goto exit;
            }
            if (!(*h_test)()) {
                printf("Module self-test failed\n");
                goto exit;
            }

            // Test 4: Context RAII for batch calls
            {
                udynlink::Context ctx(*mod.handle());
                int r3 = (*h_add)(10, 20);
                if (r3 != 30) {
                    printf("Context add(10,20) returned %d, expected 30\n", r3);
                    goto exit;
                }
                int r4 = (*h_magic)();
                if (r4 != 0xCAFE) {
                    printf("Context get_magic() returned 0x%04X, expected 0xCAFE\n", r4);
                    goto exit;
                }
            }

            // Test 5: Context::rebind to a second module instance
            {
                udynlink::Module mod2;
                err = mod2.load(
                    mod_callergo_cpp_module_data, nullptr, 0,
                    (udynlink_load_mode_t)i
                );
                if (err != UDYNLINK_OK) {
                    printf("mod2::load failed: %d\n", (int)err);
                    goto exit;
                }

                udynlink::Context ctx(*mod.handle());
                auto h2 = mod2.resolve<int(int, int)>("add");
                if (!h2) {
                    printf("mod2 resolve add failed\n");
                    goto exit;
                }

                // rebind to mod2
                ctx.rebind(*mod2.handle());
                int r5 = (*h2)(7, 8);
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
                    mod_callergo_cpp_module_data, nullptr, 0,
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
                int r6 = (*h_move)(100, 1);
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
                int r7 = (*h_assign)(50, 7);
                if (r7 != 57) {
                    printf("move-assign add(50,7) returned %d, expected 57\n", r7);
                    goto exit;
                }
            }

            // Test 7: verify Func<void> and Func<int> side-by-side
            {
                auto h_set = mod.resolve<void(int)>("set_value");
                if (!h_set) {
                    printf("resolve set_value failed\n");
                    goto exit;
                }
                auto h_get = mod.resolve<int(void)>("get_value");
                if (!h_get) {
                    printf("resolve get_value failed\n");
                    goto exit;
                }

                // Verify via C API that set_value+get_value work
                udynlink_func_t h_c_set, h_c_get;
                udynlink_resolve_func(mod.handle(), "set_value", &h_c_set);
                udynlink_resolve_func(mod.handle(), "get_value", &h_c_get);

                UDYNLINK_CALL_VOID(&h_c_set, (77));
                int c_val = UDYNLINK_CALL(&h_c_get, int, ());
                if (c_val != 77) {
                    printf("C API set_value(77)+get_value() = %d, expected 77\n", c_val);
                    goto exit;
                }

                // Now test via C++ Func
                (*h_set)(99);
                int after = (*h_get)();
                if (after != 99) {
                    printf("C++ Func set_value(99)+get_value() = %d, expected 99\n", after);
                    goto exit;
                }
            }

            // Test 8: Func<void> on disengaged handle (null addr early return)
            {
                auto h_bad = mod.resolve<void(int)>("nonexistent");
                if (h_bad) {
                    printf("Expected void Func resolve to fail\n");
                    goto exit;
                }
                // Calling a disengaged optional<Func<void>> must not crash,
                // but optional doesn't have operator() — the Func inside does.
                // We test by constructing a default Func<void(int)> directly.
                udynlink::Func<void(int)> disengaged;
                if (disengaged) {
                    printf("Default-constructed Func should be disengaged\n");
                    goto exit;
                }
                disengaged(42); // must not crash, should return immediately
            }

            // Test 9: Module::load(1-arg) convenience overload
            {
                udynlink::Module mod_conv;
                udynlink_error_t e = mod_conv.load(mod_callergo_cpp_module_data);
                if (e != UDYNLINK_OK) {
                    printf("1-arg load failed: %d\n", (int)e);
                    goto exit;
                }
                if (!mod_conv.is_loaded()) {
                    printf("1-arg load: not loaded\n");
                    goto exit;
                }
            }

            // Test 10: explicit unload + double-unload no-op
            {
                udynlink::Module mod_u;
                (void)mod_u.load(mod_callergo_cpp_module_data, nullptr, 0,
                           (udynlink_load_mode_t)i);
                if (!mod_u.is_loaded()) {
                    printf("unload test: load failed\n");
                    goto exit;
                }
                udynlink_error_t e1 = mod_u.unload();
                if (e1 != UDYNLINK_OK) {
                    printf("first unload failed: %d\n", (int)e1);
                    goto exit;
                }
                if (mod_u.is_loaded()) {
                    printf("still loaded after unload\n");
                    goto exit;
                }
                udynlink_error_t e2 = mod_u.unload();
                if (e2 != UDYNLINK_OK) {
                    printf("double-unload should be no-op (OK), got: %d\n", (int)e2);
                    goto exit;
                }
            }

            // Test 11: reload (load on already-loaded module)
            {
                udynlink::Module mod_rl;
                (void)mod_rl.load(mod_callergo_cpp_module_data, nullptr, 0,
                            (udynlink_load_mode_t)i);
                auto h1 = mod_rl.resolve<int(int, int)>("add");
                if (!h1 || (*h1)(1, 1) != 2) {
                    printf("reload: first load broken\n");
                    goto exit;
                }
                udynlink_error_t e = mod_rl.load(
                    mod_callergo_cpp_module_data, nullptr, 0,
                    (udynlink_load_mode_t)i
                );
                if (e != UDYNLINK_OK) {
                    printf("reload failed: %d\n", (int)e);
                    goto exit;
                }
                auto h2 = mod_rl.resolve<int(int, int)>("add");
                if (!h2 || (*h2)(3, 4) != 7) {
                    printf("reload: second load broken\n");
                    goto exit;
                }
            }

            // Test 12: post-Context r9 restore verification
            {
                uint32_t r9_before;
                __asm volatile ("mov %0, r9" : "=r"(r9_before) : :);

                {
                    udynlink::Context ctx(*mod.handle());
                    // r9 is now mod's ram_base — different from r9_before
                }

                uint32_t r9_after;
                __asm volatile ("mov %0, r9" : "=r"(r9_after) : :);
                if (r9_after != r9_before) {
                    printf("Context dtor failed to restore r9: before=0x%08X after=0x%08X\n",
                           (unsigned)r9_before, (unsigned)r9_after);
                    goto exit;
                }
            }

            // Test 13: Func(mod, addr) pre-resolved ctor + Func::address()
            {
                udynlink_func_t ch;
                if (udynlink_resolve_func(mod.handle(), "add", &ch) != UDYNLINK_OK) {
                    printf("pre-resolved: C resolve_func failed\n");
                    goto exit;
                }
                udynlink::Func<int(int, int)> h_from_addr(*mod.handle(), ch.addr);
                if (!h_from_addr) {
                    printf("pre-resolved Func is disengaged\n");
                    goto exit;
                }
                if (h_from_addr.address() != ch.addr) {
                    printf("Func::address() mismatch: %lu vs %lu\n",
                           (unsigned long)h_from_addr.address(), (unsigned long)ch.addr);
                    goto exit;
                }
                int r = h_from_addr(10, 5);
                if (r != 15) {
                    printf("pre-resolved add(10,5) returned %d, expected 15\n", r);
                    goto exit;
                }
            }

            // Test 14: Module::swap() and ADL swap
            {
                udynlink::Module mod_a;
                (void)mod_a.load(mod_callergo_cpp_module_data, nullptr, 0,
                           (udynlink_load_mode_t)i);
                udynlink::Module mod_b; // empty

                mod_a.swap(mod_b);
                if (mod_a.is_loaded()) {
                    printf("swap: source still loaded\n");
                    goto exit;
                }
                if (!mod_b.is_loaded()) {
                    printf("swap: dest not loaded\n");
                    goto exit;
                }

                auto h = mod_b.resolve<int(int, int)>("add");
                if (!h || (*h)(5, 5) != 10) {
                    printf("swap: dest module broken after swap\n");
                    goto exit;
                }

                // ADL swap back
                using udynlink::swap;
                swap(mod_a, mod_b);
                if (!mod_a.is_loaded() || mod_b.is_loaded()) {
                    printf("ADL swap: states wrong\n");
                    goto exit;
                }
            }
        }
    }

    res = 1;
exit:
    return res;
}
