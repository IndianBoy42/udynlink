#include <stdio.h>

class MagicHolder
{
public:
    MagicHolder() {}
    int get() const { return 0xCAFE; }
};

static MagicHolder s_magic;
int s_side_effect = 0;

extern "C" int add(int a, int b) {
    return a + b;
}

extern "C" int get_magic(void) {
    return s_magic.get();
}

extern "C" void set_value(int v) {
    s_side_effect = v;
}

extern "C" int get_value(void) {
    return s_side_effect;
}

extern "C" int test(void) {
    set_value(42);
    return (add(2, 3) == 5 && add(-1, 1) == 0 && get_magic() == 0xCAFE && get_value() == 42);
}
