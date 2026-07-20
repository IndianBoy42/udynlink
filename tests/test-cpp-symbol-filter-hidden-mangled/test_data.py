test_data = {
    "desc": "--strip-hidden-syms and --strip-mangled-syms combined: hidden mangled, public mangled, and weak mangled symbols all demoted",
    "mkmodule_args": "--strip-hidden-syms --strip-mangled-syms",
    "modules": [["mod_all.cpp"]],
    "required": [r"^ctor1$", r"^ctor2$", r"^test ok mode \d+$"],
}
