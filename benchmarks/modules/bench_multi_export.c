#include <stdint.h>

int fn_add(int a, int b) { return a + b; }
int fn_sub(int a, int b) { return a - b; }
int fn_mul(int a, int b) { return a * b; }
int fn_div(int a, int b) { return b ? a / b : 0; }
int fn_mod(int a, int b) { return b ? a % b : 0; }
int fn_and(int a, int b) { return a & b; }
int fn_or(int a, int b) { return a | b; }
int fn_xor(int a, int b) { return a ^ b; }
int fn_shl(int a, int b) { return a << b; }
int fn_shr(int a, int b) { return (unsigned)a >> b; }

int16_t fn_add16(int16_t a, int16_t b) { return a + b; }
int16_t fn_mul16(int16_t a, int16_t b) { return a * b; }

int32_t fn_clz32(uint32_t v) {
    int c = 0;
    while (v) { v >>= 1; c++; }
    return c;
}

int32_t fn_popcount32(uint32_t v) {
    int c = 0;
    while (v) { c += v & 1; v >>= 1; }
    return c;
}

int fn_abs(int x) { return x < 0 ? -x : x; }
int fn_max(int a, int b) { return a > b ? a : b; }
int fn_min(int a, int b) { return a < b ? a : b; }
int fn_clamp(int lo, int hi, int v) { return v < lo ? lo : (v > hi ? hi : v); }
int fn_sign(int x) { return (x > 0) - (x < 0); }

int fn_isqrt(int n) {
    int x = n, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + n / x) / 2; }
    return x;
}
