test_data = {
    "desc": "C++ inline, static inline, and weak template instantiations survive --strip-mangled-syms",
    "mkmodule_args": "--strip-mangled-syms --strip-weak-sym-names",
    "modules": [["mod_inlines.cpp"]],
    "required": [],
}
