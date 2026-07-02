/*
 * test_main.c — host test driver.
 *
 * Calls module functions directly (no udynlink loader involvement) and
 * asserts on the results. Exits non-zero on failure.
 */
#include <stdio.h>


#include "mock_host.h"

extern int regulate(int target);
extern int get_threshold(void);

#define ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        return 1; \
    } \
} while (0)

int main(void) {
    /* Test 1: normal regulation — reading below threshold */
    mock_sensor_set(50);
    mock_actuator_reset();
    ASSERT(regulate(100) == 0);
    ASSERT(mock_actuator_get() == 50);   /* 100 - 50 */
    ASSERT(mock_actuator_calls() == 1);

    /* Test 2: sensor above threshold — should clamp */
    mock_sensor_set(150);
    mock_actuator_reset();
    ASSERT(regulate(100) == -1);
    ASSERT(mock_actuator_get() == 0);
    ASSERT(mock_actuator_calls() == 1);

    /* Test 3: exact threshold boundary */
    mock_sensor_set(100);   /* == threshold, not > threshold */
    mock_actuator_reset();
    ASSERT(regulate(100) == 0);
    ASSERT(mock_actuator_get() == 0);    /* 100 - 100 */

    /* Test 4: threshold accessor */
    ASSERT(get_threshold() == 100);

    printf("All host tests passed.\n");
    return 0;
}
