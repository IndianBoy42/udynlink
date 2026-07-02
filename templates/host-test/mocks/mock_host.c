/*
 * mock_host.c — mock implementations of host symbols the module consumes.
 *
 * Each function here matches an `extern` declaration in the module source.
 * The mock maintains internal state that the test driver can configure
 * (via mock_* helpers) and inspect after calling module functions.
 */
#include "mock_host.h"

#include <stdint.h>

static int s_sensor_value = 0;
static int s_actuator_value = 0;
static int s_actuator_calls = 0;

/* --- Mock control API (called by test driver, not by module) --- */

void mock_sensor_set(int value) { s_sensor_value = value; }
void mock_actuator_reset(void) { s_actuator_value = 0; s_actuator_calls = 0; }
int  mock_actuator_get(void)   { return s_actuator_value; }
int  mock_actuator_calls(void) { return s_actuator_calls; }

/* --- Host symbols consumed by module/mod_example.c --- */

int sensor_read(int channel) {
    (void)channel;
    return s_sensor_value;
}

void actuator_set(int value) {
    s_actuator_value = value;
    s_actuator_calls++;
}
