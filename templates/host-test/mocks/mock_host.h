#ifndef MOCK_HOST_H
#define MOCK_HOST_H

/*
 * Mock control API — called by the test driver to set up and inspect
 * mock state. These functions are NOT consumed by the module; they exist
 * so tests can configure what the host symbols return.
 */

void mock_sensor_set(int value);
void mock_actuator_reset(void);
int  mock_actuator_get(void);
int  mock_actuator_calls(void);

#endif /* MOCK_HOST_H */
