/* State-chart module test (see scripts/sm2module, the codegen-integration
 * example).
 *
 * The module (built from door_mod.c) exports door_step()/door_state_name()/
 * door_event_name()/door_event_id() and references two host-side action
 * hooks, door_action_lock()/door_action_unlock(), which are bound through
 * test_resolve_symbol() at load time. The chart: closed --push--> open,
 * open --lock(action)--> locked, locked --unlock(action)--> open.
 */
#include "udynlink.h"
#include "door_mod_module_data.h"
#include "test_utils.h"
#include "door_contract.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Action counters, threaded through step()'s ctx pointer. */
typedef struct {
    unsigned locks;
    unsigned unlocks;
} action_counts_t;

/* Host-side action hooks the module calls through the LOT; non-static —
 * the contract header declares them with external linkage. */
void door_action_lock(void *ctx)
{
    ((action_counts_t *)ctx)->locks++;
}

void door_action_unlock(void *ctx)
{
    ((action_counts_t *)ctx)->unlocks++;
}

/* Module extern symbols the host must provide. */
uintptr_t test_resolve_symbol(const char *name)
{
    if (!strcmp(name, "door_action_lock"))
        return (uintptr_t)&door_action_lock;
    if (!strcmp(name, "door_action_unlock"))
        return (uintptr_t)&door_action_unlock;
    return 0;
}

static int roundtrip(const udynlink_module_t *p_mod)
{
    const char *exported_syms[] = {"door_step", "door_state_name",
                                   "door_event_name", "door_event_id", NULL};
    const char *extern_syms[] = {"door_action_lock", "door_action_unlock",
                                 NULL};
    udynlink_sym_t sym;
    uint8_t (*step)(uint8_t, uint8_t, void *);
    const char *(*state_name)(uint8_t);
    uint8_t (*event_id)(const char *);
    action_counts_t counts = {0, 0};

    if (!check_exported_symbols(p_mod, exported_syms))
        return 0;
    if (!check_extern_symbols(p_mod, extern_syms))
        return 0;
    if (!udynlink_lookup_symbol(p_mod, "door_step", &sym))
        return 0;
    step = (uint8_t (*)(uint8_t, uint8_t, void *))sym.val;
    if (!udynlink_lookup_symbol(p_mod, "door_state_name", &sym))
        return 0;
    state_name = (const char *(*)(uint8_t))sym.val;
    if (!udynlink_lookup_symbol(p_mod, "door_event_id", &sym))
        return 0;
    event_id = (uint8_t (*)(const char *))sym.val;

    /* walk the chart from the initial state.
     * r9 is caller-owned PIC state: any host call in between (lookup,
     * printf) may clobber it, so PREPARE_CALL sits immediately before
     * every module invocation. */
    uint8_t s = DOOR_STATE_CLOSED;
    UDYNLINK_PREPARE_CALL(p_mod);
    if (step(s, DOOR_EVENT_LOCK, &counts) != s) { /* no transition */
        printf("expected no closed+lock transition\n");
        return 0;
    }
    UDYNLINK_PREPARE_CALL(p_mod);
    if (step(s, DOOR_EVENT_PUSH, &counts) != DOOR_STATE_OPEN) {
        printf("closed+push != open\n");
        return 0;
    }
    s = DOOR_STATE_OPEN;
    UDYNLINK_PREPARE_CALL(p_mod);
    if (step(s, DOOR_EVENT_LOCK, &counts) != DOOR_STATE_LOCKED ||
        counts.locks != 1) {
        printf("open+lock != locked or lock action not fired\n");
        return 0;
    }
    s = DOOR_STATE_LOCKED;
    UDYNLINK_PREPARE_CALL(p_mod);
    if (step(s, DOOR_EVENT_UNLOCK, &counts) != DOOR_STATE_OPEN ||
        counts.unlocks != 1) {
        printf("locked+unlock != open or unlock action not fired\n");
        return 0;
    }
    s = DOOR_STATE_OPEN;

    /* name tables and event_id round-trip */
    UDYNLINK_PREPARE_CALL(p_mod);
    if (!state_name(DOOR_STATE_LOCKED) ||
        strcmp(state_name(DOOR_STATE_LOCKED), "locked") != 0) {
        printf("state_name(locked) mismatch\n");
        return 0;
    }
    UDYNLINK_PREPARE_CALL(p_mod);
    if (state_name(99) != NULL) {
        printf("state_name(out of range) != NULL\n");
        return 0;
    }
    UDYNLINK_PREPARE_CALL(p_mod);
    if (event_id("unlock") != DOOR_EVENT_UNLOCK ||
        event_id("nope") != 0xFFu || event_id(NULL) != 0xFFu) {
        printf("event_id mismatch\n");
        return 0;
    }
    printf("statechart round-trip OK (%u actions)\n",
           counts.locks + counts.unlocks);
    return 1;
}

int test_qemu(void)
{
    udynlink_module_t mod;
    int res = 0;

    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL;
         i <= (int)UDYNLINK_LOAD_MODE_XIP; i++) {
        if (test_load_module(&mod, door_mod_module_data, NULL, 0,
                             (udynlink_load_mode_t)i))
            return 0;
        if (!roundtrip(&mod))
            goto exit;
        test_unload_module(&mod);
    }
    res = 1;
exit:
    test_unload_module(&mod);
    return res;
}
