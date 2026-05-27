#include <stdio.h>

class MagicHolder
{
public:
    MagicHolder() {}
    int get() const { return 0xCAFE; }
};

static MagicHolder s_magic;

extern "C" int add(int a, int b) {
    return a + b;
}

extern "C" int get_magic(void) {
    return s_magic.get();
}

extern "C" int test(void) {
    return (add(2, 3) == 5 && add(-1, 1) == 0 && get_magic() == 0xCAFE);
}
