#include <stdint.h>

int32_t public_add(int32_t a, int32_t b) { return a + b; }
int32_t public_mul(int32_t a, int32_t b) { return a * b; }

static int32_t private_helper(int32_t x) { return x * x + x; }

int32_t public_compute(int32_t x) { return private_helper(x); }

int32_t internal_unused1(int32_t x) { return x + 1; }
int32_t internal_unused2(int32_t x) { return x + 2; }
int32_t internal_unused3(int32_t x) { return x + 3; }
