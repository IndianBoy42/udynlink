/* Protobuf module test: 1 struct == 1 module.
 *
 * The module (built from sensor_mod.c + sensor.pb.c via scripts/proto2module)
 * exports sensor_parse()/sensor_write() for the SensorReading message — the
 * default export prefix is the .proto basename. The nanopb runtime
 * (pb_common.c, pb_encode.c, pb_decode.c) lives in the host firmware; the
 * module binds to it through test_resolve_symbol() at load time.
 */
#include "udynlink.h"
#include "sensor_mod_module_data.h"
#include "test_utils.h"
#include "sensor.pb.h"
#include <pb_decode.h>
#include <pb_encode.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Host-side nanopb runtime symbols the module references. */
uintptr_t test_resolve_symbol(const char *name)
{
    if (!strcmp(name, "pb_decode"))
        return (uintptr_t)(uintptr_t)&pb_decode;
    if (!strcmp(name, "pb_encode"))
        return (uintptr_t)(uintptr_t)&pb_encode;
    if (!strcmp(name, "pb_istream_from_buffer"))
        return (uintptr_t)(uintptr_t)&pb_istream_from_buffer;
    if (!strcmp(name, "pb_ostream_from_buffer"))
        return (uintptr_t)(uintptr_t)&pb_ostream_from_buffer;
    return 0;
}

typedef int (*parse_fn_t)(const unsigned char *, size_t, void *);
typedef int (*write_fn_t)(const void *, unsigned char *, size_t *);

static int roundtrip(const udynlink_module_t *p_mod)
{
    const char *exported_syms[] = {"sensor_parse", "sensor_write", NULL};
    const char *extern_syms[] = {"pb_decode", "pb_encode",
                                 "pb_istream_from_buffer",
                                 "pb_ostream_from_buffer", NULL};
    udynlink_sym_t sym_parse, sym_write;
    unsigned char buf[128];
    size_t len;
    SensorReading msg, out;

    if (!check_exported_symbols(p_mod, exported_syms))
        return 0;
    if (!check_extern_symbols(p_mod, extern_syms))
        return 0;
    if (!udynlink_lookup_symbol(p_mod, "sensor_parse", &sym_parse))
        return 0;
    if (!udynlink_lookup_symbol(p_mod, "sensor_write", &sym_write))
        return 0;

    memset(&msg, 0, sizeof(msg));
    msg.id = 42;
    msg.temperature = -1234;
    msg.humidity = 750;
    strcpy(msg.tag, "hall");
    msg.samples_count = 3;
    msg.samples[0] = 10;
    msg.samples[1] = 20;
    msg.samples[2] = 30;

    len = sizeof(buf);
    UDYNLINK_PREPARE_CALL(p_mod);
    if (!((write_fn_t)sym_write.val)(&msg, buf, &len)) {
        printf("encode failed\n");
        return 0;
    }

    memset(&out, 0, sizeof(out));
    if (!((parse_fn_t)sym_parse.val)(buf, len, &out)) {
        printf("decode failed\n");
        return 0;
    }

    if (out.id != 42 || out.temperature != -1234 || out.humidity != 750 ||
        strcmp(out.tag, "hall") != 0 || out.samples_count != 3 ||
        out.samples[0] != 10 || out.samples[1] != 20 || out.samples[2] != 30) {
        printf("round-trip mismatch\n");
        return 0;
    }
    printf("protobuf round-trip OK (%u bytes)\n", (unsigned int)len);
    return 1;
}

int test_qemu(void)
{
    udynlink_module_t mod;
    int res = 0;

    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL;
         i <= (int)UDYNLINK_LOAD_MODE_XIP; i++) {
        if (test_load_module(&mod, sensor_mod_module_data, NULL, 0,
                             (udynlink_load_mode_t)i))
            return 0;
        if (!roundtrip(&mod))
            goto exit;
        test_unload_module(&mod);
    }
    res = 1;
exit:
    test_unload_module(&mod);
    return res;
}
