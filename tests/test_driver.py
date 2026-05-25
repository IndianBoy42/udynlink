#!/usr/bin/env python3

import os
import subprocess
import sys
import shutil
import re

os.chdir(os.path.dirname(os.path.realpath(__file__)))

# ---------------------------------------------------------------------------
# Configuration: QEMU binary, target board, CPU, and extra flags.
#
# Defaults point to the legacy xPack QEMU (qemu-system-gnuarmeclipse) and
# the STM32F429I-Discovery board because that is what the current test
# firmware is compiled/linked for.  Override via environment variables to
# experiment with other QEMU binaries or target boards.
# ---------------------------------------------------------------------------

def find_qemu_in_path():
    for path_dir in os.environ.get("PATH", "").split(os.pathsep):
        candidate = os.path.join(path_dir, "qemu-system-gnuarmeclipse")
        if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    return None

# QEMU binary path.
#   Default: search PATH for the legacy QEMU, fall back to bare name.
qemu_from_path = find_qemu_in_path()
default_qemu_bin = qemu_from_path or "qemu-system-gnuarmeclipse"

qemu_bin = os.environ.get("UDYNLINK_QEMU_BIN", default_qemu_bin)

# Target board / machine name.
#   Legacy QEMU uses  -board <name>
#   Upstream QEMU uses -machine <name>
qemu_machine = os.environ.get("UDYNLINK_QEMU_MACHINE", "STM32F429I-Discovery")

# CPU type (mainly for upstream QEMU -cpu flag).
#   e.g. cortex-m4, cortex-m3, cortex-m0
default_cpu = "cortex-m4"  # STM32F429 is a Cortex-M4
qemu_cpu = os.environ.get("UDYNLINK_QEMU_CPU", default_cpu)

# Extra QEMU flags (space-separated).
#   e.g. "-semihosting -d unimp,guest_errors"
qemu_extra_flags = os.environ.get("UDYNLINK_QEMU_EXTRA_FLAGS", "")

# Whether the binary is the legacy qemu-system-gnuarmeclipse fork.
is_legacy = os.path.basename(qemu_bin) == "qemu-system-gnuarmeclipse"

default_qemu_timeout = 5
compile_cmd = '%s ../../scripts/mkmodule --disasm --gen-c-header --header-path ../qemu_host/src %%s%%s' % sys.executable
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

# Build the QEMU command line for running test1.elf.
def build_qemu_cmd(elf_path="test1.elf"):
    parts = [qemu_bin]
    if is_legacy:
        parts += ["-board", qemu_machine, "-image", elf_path, "-nographic"]
    else:
        # Upstream qemu-system-arm / qemu-system-aarch64 syntax
        parts += ["-machine", qemu_machine, "-kernel", elf_path, "-nographic"]
        if qemu_cpu:
            parts += ["-cpu", qemu_cpu]
        if qemu_extra_flags:
            parts += qemu_extra_flags.split()
    return " ".join(parts)

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
        objdump = f"{os.environ.get('UDYNLINK_CC_PREFIX', 'arm-none-eabi-')}objdump"
        cmd = f"{objdump} -Dztr --source ./{os.path.splitext(m[0])[0]}.elf"
        res, out = run_cmd(cmd)
        out = out.decode() 
        with open(full_path + "/output_objdump_%s.txt" % aopt, 'w') as fout:
            fout.write(out)
 
    # Copy qemu test in its directory
    shutil.copyfile("test_qemu.c", os.path.join("../qemu_host/src", "test_qemu.c"))
    # Build qemu test
    cmake_build_dir = "../build"
    global cleaned
    if not cleaned:
        # Remove the build directory to force a clean rebuild
        shutil.rmtree(cmake_build_dir, ignore_errors=True)
        cleaned = True
    if not run_cmd("cmake -B %s -S ../qemu_host" % cmake_build_dir)[0]:
        return False, "Unable to configure test"
    if not run_cmd("cmake --build %s --target test1.elf" % cmake_build_dir)[0]:
        return False, "Unable to build test"
    # Run QEMU with the freshly compiled test
    print("--- Running QEMU ---")
    qemu_cmd = build_qemu_cmd(os.path.join(cmake_build_dir, "test1.elf"))
    res, out = run_cmd(qemu_cmd, timeout=default_qemu_timeout)
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
