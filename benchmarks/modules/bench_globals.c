#include <stdint.h>

volatile int32_t g0 = 0;
volatile int32_t g1 = 1;
volatile int32_t g2 = 2;
volatile int32_t g3 = 3;
volatile int32_t g4 = 4;
volatile int32_t g5 = 5;
volatile int32_t g6 = 6;
volatile int32_t g7 = 7;

volatile int32_t g_a = 10;
volatile int32_t g_b = 20;
volatile int32_t g_c = 30;
volatile int32_t g_d = 40;
volatile int32_t g_e = 50;
volatile int32_t g_f = 60;
volatile int32_t g_g = 70;
volatile int32_t g_h = 80;

volatile const int32_t g_const_a = 100;
volatile const int32_t g_const_b = 200;
volatile const int32_t g_const_c = 300;

volatile int32_t *g_ptr_arr[4] = { &g_a, &g_b, &g_c, &g_d };

int bench_globals(void) {
    g0 = g_a + g_b;
    g1 = g_c - g_d;
    g2 = g_e * g_f;
    g3 = g_const_a + g_const_b + g_const_c;
    int sum = 0;
    for (int i = 0; i < 4; i++) sum += *g_ptr_arr[i];
    return g0 + g1 + g2 + g3 + sum;
}
