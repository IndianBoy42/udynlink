#!/usr/bin/env python3

import os
import subprocess
import sys
import shutil
import re

os.chdir(os.path.dirname(os.path.realpath(__file__)))

default_qemu_timeout = 5
compile_cmd = '../../scripts/mkmodule --disasm --gen-c-header --header-path ../qemu_host/src %s%s'
cleaned = False

# Simple decorator that keeps the curent directory unchanged after running
# a function.
def keep_current_dir(func):
    def wrap_func(*args, **kwargs):
        cwd = os.getcwd()
        res = func(*args, **kwargs)
        os.chdir(cwd)
        return res
    return wrap_func

# Run a command, capturing output
# Return the output and the exit code
def run_cmd(cmd, show_output=False, timeout=None):
    print("Executing '%s' " % cmd)
    child = subprocess.Popen(cmd.split(' '), stdout=subprocess.PIPE)
    try:
        out, _ = child.communicate(timeout = timeout)
    except subprocess.TimeoutExpired:
        print("-" * 80 + "\nTimeout running '%s'" % cmd)
        child.terminate()
        return (False, None)
    if child.returncode != 0:
        print("-" * 80 + "\nError running '%s'" % cmd)
        print(out)
        return (False, out)
    if show_output:
        print(out)
    return (True, out)

# Run a single test
@keep_current_dir
def test_one(full_path, opt):
    sys.path.append(full_path)
    if "test_data" in sys.modules:
        del sys.modules["test_data"]
    test_data = {}
    if os.path.isfile(os.path.join(full_path, "test_data.py")):
        from test_data import test_data
    else:
        test_data = {
            "desc": "",
            "modules": [[a for a in os.listdir(full_path) if a.endswith(".cpp")]],
            "required": []
        }
    
    sys.path.remove(full_path)
    aopt = "Os" if opt else "O3"
    print("--- Running test '%s' in '%s' with opt %s ---" % (test_data["desc"], os.path.basename(full_path), "-Os" if opt else "-O0"))
    os.chdir(full_path)
    # Compile first
    if not "modules" in test_data:
        return False, "No modules!"
    for m in test_data["modules"]:
        srcs = " ".join(m)
        cmd = compile_cmd % ("" if opt else "--no-opt ", srcs)
        res, out = run_cmd(cmd)
        out = out.decode() 
        if not res:
            return False, "Unable to compile module(s) " + srcs
        with open(full_path + "/output_build_%s.txt" % aopt, 'w') as fout:
            fout.write(out)
        cmd = "arm-none-eabi-objdump -Dztr --source ./%s.elf" % os.path.splitext(srcs)[0]
        res, out = run_cmd(cmd)
        out = out.decode() 
        with open(full_path + "/output_objdump_%s.txt" % aopt, 'w') as fout:
            fout.write(out)
 
    # Copy qemu test in its directory
    shutil.copyfile("test_qemu.c", os.path.join("../qemu_host/src", "test_qemu.c"))
    # Build qemu test
    os.chdir("../qemu_host/Debug")
    global cleaned
    if not cleaned:
        if not run_cmd("make clean")[0]:
            return False
        cleaned = True
    if not run_cmd("make test1.elf")[0]:
        return False, "Unable to build test"
    # Run QEMU with the freshly compiled test
    print("--- Running QEMU ---")
    res, out = run_cmd("qemu-system-gnuarmeclipse -board STM32F429I-Discovery -image test1.elf -nographic", timeout=default_qemu_timeout)
    out = out.decode() 
    if not res:
        return False, "**** Unable to run QEMU or timeout running ****"
    with open(full_path + "/output_test_%s.txt" % aopt, 'w') as fout:
        fout.write(out)
    # Check result
    if out.find("*** TEST OK ***") == -1:
        return False, "**** Can't find the test OK indicator in the output ****\n" + out
    for t in test_data.get("required", []):
        finds = re.findall(t, out, re.MULTILINE)
        if len(finds) < test_data.get("total_loads", 3): # consider each load mode in turn
            return False, "**** Can't find '%s' in output ****" % t + out
    return True, out

total, failed = 0, 0
tests = sys.argv[1:] if len(sys.argv) > 1 else os.listdir(".")
for l in tests:
    # Look through all dirs that begin with "test-" and have a test_data.py file
    if l.startswith("test-") and os.path.isdir(l):
        for opt in [False, True]:
            res, out = test_one(os.path.abspath(l), opt)
            total += 1
            if not res:
                print("--- TEST FAILED! ---")
                print(out + "\n")
                failed += 1
            else:
                print("--- TEST OK ---\n")

print('*' * 20)
print("Total:  %d" % total)
print("Passed: %d" % (total - failed))
print("Failed: %d" % failed)
print('*' * 20)

os._exit(failed)

