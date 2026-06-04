#include <stdint.h>

extern int32_t vec_dot(const int32_t *a, const int32_t *b, int n);
extern int32_t vec_sum(const int32_t *v, int n);
extern void    vec_add(int32_t *dst, const int32_t *a, const int32_t *b, int n);
extern void    vec_scale(int32_t *dst, const int32_t *v, int32_t s, int n);
extern int32_t vec_max(const int32_t *v, int n);
extern int32_t vec_min(const int32_t *v, int n);

int bench_multi_file(void) {
    int32_t a[8] = {1,2,3,4,5,6,7,8};
    int32_t b[8] = {8,7,6,5,4,3,2,1};
    int32_t c[8];

    int32_t dot  = vec_dot(a, b, 8);
    int32_t sum  = vec_sum(a, 8);
    vec_add(c, a, b, 8);
    vec_scale(c, c, 2, 8);
    int32_t mx = vec_max(c, 8);
    int32_t mn = vec_min(c, 8);

    return (dot > 0) && (sum > 0) && (mx > mn);
}
