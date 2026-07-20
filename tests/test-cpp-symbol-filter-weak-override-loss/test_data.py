test_data = {
    "desc": "--strip-weak-sym-names demotes weak symbols but module still loads and runs",
    "mkmodule_args": "--strip-weak-sym-names",
    "modules": [["mod_weak_loss.cpp"]],
    "required": [],
}
