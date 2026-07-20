test_data = {
    "desc": "--strip-hidden-syms and --strip-weak-sym-names combined: hidden and weak symbols demoted, extern C survives",
    "mkmodule_args": "--strip-hidden-syms --strip-weak-sym-names",
    "modules": [["mod_all.cpp"]],
    "required": [r"^ctor1$", r"^ctor2$", r"^test ok mode \d+$"],
}
