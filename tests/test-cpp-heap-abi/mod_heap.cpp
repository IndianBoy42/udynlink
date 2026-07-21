// Module that actually exercises operator new / delete / new[] / delete[] so
// the host stubs in udynlink_cpp_abi.h are called at runtime, not merely
// bound. Each allocation round-trips through udynlink_external_malloc/_free.
#include <stdio.h>

struct Widget {
    int v;
    Widget(int x) : v(x) {}
    virtual ~Widget() = default;   // forces _ZdlPvj reference (deleting dtor)
};

extern "C" int test(void) {
    int sum = 0;

    // new / delete — exercises _Znwj + _ZdlPvj.
    Widget *w = new Widget(10);
    sum += w->v;
    delete w;

    // new[] / delete[] — exercises _Znaj + _ZdaPvj.
    Widget *arr = new Widget[3]{1, 2, 3};
    sum += arr[0].v + arr[1].v + arr[2].v;
    delete[] arr;

    return sum == 16 ? 1 : 0;  // 10 + 1+2+3 = 16
}
