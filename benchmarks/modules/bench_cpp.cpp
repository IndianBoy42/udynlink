#include <stdint.h>
#include <stdio.h>

class Counter {
    int32_t val_;
public:
    Counter(int32_t v) : val_(v) { printf("Counter(%d)\n", v); }
    ~Counter() { printf("~Counter()\n"); }
    int32_t get() const { return val_; }
    void add(int32_t v) { val_ += v; }
};

class Accumulator {
    int32_t sum_;
public:
    Accumulator() : sum_(0) {}
    void push(int32_t v) { sum_ += v; }
    int32_t total() const { return sum_; }
};

static Counter g_counter(42);
static Accumulator g_acc;

class Transformer {
public:
    virtual int32_t apply(int32_t v) const = 0;
    virtual ~Transformer() {}
};

class AddN : public Transformer {
    int32_t n_;
public:
    AddN(int32_t n) : n_(n) {}
    int32_t apply(int32_t v) const override { return v + n_; }
};

class MulN : public Transformer {
    int32_t n_;
public:
    MulN(int32_t n) : n_(n) {}
    int32_t apply(int32_t v) const override { return v * n_; }
};

extern "C" int bench_cpp(void) {
    g_counter.add(8);
    for (int i = 0; i < 10; i++) g_acc.push(i);

    AddN add10(10);
    MulN mul3(3);

    int32_t r1 = add10.apply(g_counter.get());
    int32_t r2 = mul3.apply(g_acc.total());

    return (r1 == 60) && (r2 == 135);
}
