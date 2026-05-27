# Weak symbol resolution test
# Tests that weak symbols use the module's own definition by default,
# and are overridden when the host provides them via udynlink_external_resolve_symbol.

test_data = {
    "desc": "Weak symbol host override test",
    "modules": [["mod_weak.c"]],
    "required": []
}
