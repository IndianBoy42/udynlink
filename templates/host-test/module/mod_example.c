/*
 * mod_example.c — example udynlink module for host-side testing.
 *
 * This is the exact same source file you would pass to `mkmodule` for
 * on-target compilation. It consumes two host-provided symbols
 * (sensor_read, actuator_set) and exports one function (regulate).
 *
 * On target: udynlink resolves sensor_read / actuator_set at load time
 *            via udynlink_external_resolve_symbol().
 * On host:   the linker resolves them to the mock implementations in
 *            mocks/mock_host.c.
 *
 * No source changes between target and host builds.
 */
#include <stdint.h>

extern int sensor_read(int channel);
extern void actuator_set(int value);

static int threshold = 100;

int regulate(int target) {
    int reading = sensor_read(0);
    if (reading > threshold) {
        actuator_set(0);
        return -1;
    }
    actuator_set(target - reading);
    return 0;
}

int get_threshold(void) {
    return threshold;
}
