#include <stdint.h>

int32_t vec_dot(const int32_t *a, const int32_t *b, int n) {
    int32_t s = 0;
    for (int i = 0; i < n; i++) s += a[i] * b[i];
    return s;
}

int32_t vec_sum(const int32_t *v, int n) {
    int32_t s = 0;
    for (int i = 0; i < n; i++) s += v[i];
    return s;
}

void vec_add(int32_t *dst, const int32_t *a, const int32_t *b, int n) {
    for (int i = 0; i < n; i++) dst[i] = a[i] + b[i];
}

void vec_scale(int32_t *dst, const int32_t *v, int32_t s, int n) {
    for (int i = 0; i < n; i++) dst[i] = v[i] * s;
}

int32_t vec_max(const int32_t *v, int n) {
    int32_t m = v[0];
    for (int i = 1; i < n; i++) if (v[i] > m) m = v[i];
    return m;
}

int32_t vec_min(const int32_t *v, int n) {
    int32_t m = v[0];
    for (int i = 1; i < n; i++) if (v[i] < m) m = v[i];
    return m;
}
