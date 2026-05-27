#include <stdio.h>

class Test
{
public:
    Test() {
        printf("cpp constructor\n");
    }
};

static Test constructor_test;

extern "C" int test(void) {
    printf("strip-init-array cpp ok\n");
    return 1;
}
