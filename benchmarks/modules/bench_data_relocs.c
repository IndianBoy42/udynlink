#include <stdint.h>

extern int32_t ext_a;
extern int32_t ext_b;
extern int32_t ext_c;
extern int32_t ext_d;
extern void (*ext_callback_a)(int);
extern void (*ext_callback_b)(int);
extern void (*ext_callback_c)(int);
extern int32_t (*ext_compute_a)(int32_t);
extern int32_t (*ext_compute_b)(int32_t);
extern int32_t (*ext_compute_c)(int32_t);

typedef void (*void_fn)(int);
typedef int32_t (*int_fn)(int32_t);

static void_fn handlers[6] = {
    (void_fn)0,
    (void_fn)0,
    (void_fn)0,
    (void_fn)0,
    (void_fn)0,
    (void_fn)0,
};

static int_fn computefs[3] = {
    (int_fn)0,
    (int_fn)0,
    (int_fn)0,
};

void bench_data_relocs_init(void) {
    handlers[0] = ext_callback_a;
    handlers[1] = ext_callback_b;
    handlers[2] = ext_callback_c;
    computefs[0] = ext_compute_a;
    computefs[1] = ext_compute_b;
    computefs[2] = ext_compute_c;
}

int32_t bench_data_relocs_run(int32_t val) {
    int32_t sum = ext_a + ext_b + ext_c + ext_d;
    if (ext_compute_a) sum += ext_compute_a(val);
    if (ext_compute_b) sum += ext_compute_b(val);
    if (ext_compute_c) sum += ext_compute_c(val);
    return sum;
}
