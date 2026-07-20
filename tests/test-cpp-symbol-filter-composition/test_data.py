test_data = {
    "desc": "All four --strip-* flags combined: module loads, ctors run, test() functions correctly",
    "mkmodule_args": "--public-symbols=add,sub,mul,test,call_mangled_through_got,call_via_templated,c_ctor_fn --strip-hidden-syms --strip-non-public-syms --strip-mangled-syms --strip-weak-sym-names",
    "modules": [["mod_all.cpp"]],
    "required": [r"^ctor1$", r"^ctor2$", r"^test ok mode \d+$"],
}
