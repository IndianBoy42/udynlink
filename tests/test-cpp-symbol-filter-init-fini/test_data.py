test_data = {
    "desc": "C++ ctors and C __attribute__((constructor)) run in the right order under --strip-mangled-syms",
    "mkmodule_args": "--strip-mangled-syms",
    "modules": [["mod_init_fini.cpp"]],
    "required": [
        r"^c_ctor first$",
        r"^ctor a 1$",
        r"^ctor b 2$",
        r"^ctor c 3$",
        r"^c_ctor last$",
    ],
}
