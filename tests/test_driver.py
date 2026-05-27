#!/usr/bin/env python3

import os
import subprocess
import sys
import shutil
import re
import time

os.chdir(os.path.dirname(os.path.realpath(__file__)))

# ---------------------------------------------------------------------------
# Safe print for parallel execution under just(1).
#
# When `just` runs recipes with [parallel], stdout is a shared pipe in
# non-blocking mode.  With multiple suites writing simultaneously the pipe
# buffer fills up and plain print() raises BlockingIOError.  We stream bytes
# to the raw buffer and retry on EAGAIN until space is available.
# ---------------------------------------------------------------------------
def safe_print(text, end='\n'):
    data = (text + end).encode('utf-8', errors='replace')
    while True:
        try:
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()
            break
        except BlockingIOError:
            time.sleep(0.005)

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

default_qemu_timeout = int(os.environ.get("UDYNLINK_QEMU_TIMEOUT", "5"))
module_target = os.environ.get("UDYNLINK_MODULE_TARGET", "")
module_target_flag = " --target %s " % module_target if module_target else " "

# Repo root, computed from tests/ directory (where this script lives)
repo_root = os.path.abspath("..")

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
def run_cmd(cmd, show_output=False, timeout=None, quiet_on_error=False):
    safe_print("Executing '%s' " % cmd)
    child = subprocess.Popen(cmd.split(' '), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        out, _ = child.communicate(timeout = timeout)
    except subprocess.TimeoutExpired:
        safe_print("-" * 80 + "\nTimeout running '%s'" % cmd)
        child.terminate()
        return (False, None)
    if child.returncode != 0:
        if not quiet_on_error:
            safe_print("-" * 80 + "\nError running '%s' (exit code %d)" % (cmd, child.returncode))
            sys.stdout.buffer.write(out)
            sys.stdout.buffer.flush()
        return (False, out)
    if show_output:
        sys.stdout.buffer.write(out)
        sys.stdout.buffer.flush()
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
    test_name = os.path.basename(full_path)
    aopt = "Os" if opt else "O3"
    safe_print("--- Running test '%s' in '%s' with opt %s ---" % (test_data["desc"], test_name, "-Os" if opt else "-O3"))
    os.chdir(full_path)

    # Isolated working directories so tests can run in parallel.
    # Include the platform name so different platform suites (e.g. mps2 vs an500)
    # do not collide when running concurrently via `just ci`.
    platform = os.environ.get("UDYNLINK_PLATFORM", "default")
    build_dir = os.path.abspath(os.path.join("..", "build_%s_%s_%s" % (platform, test_name, aopt)))
    src_dir = os.path.abspath(os.path.join("..", "build_%s_%s_%s_src" % (platform, test_name, aopt)))
    # Only clean the build dir if explicitly requested.  Each (platform, test, opt)
    # combination already has a unique directory, so stale object files are not a
    # problem across different tests.  Keeping the build dir lets cmake/ninja do
    # incremental compilation when the same test is rerun.
    if os.environ.get("UDYNLINK_TEST_CLEAN") or "--clean" in sys.argv:
        shutil.rmtree(build_dir, ignore_errors=True)
        sys.argv.remove("--clean") if "--clean" in sys.argv else None
    # Always clean src_dir because it holds per-test files (test_qemu.c, module headers).
    shutil.rmtree(src_dir, ignore_errors=True)
    os.makedirs(build_dir, exist_ok=True)
    os.makedirs(src_dir, exist_ok=True)

    # Copy ALL source files from the test directory into the isolated src
    # directory so that mkmodule compilation artifacts (.o, .elf, .bin) are
    # fully isolated and do not race with parallel test invocations.
    for f in os.listdir(full_path):
        src_path = os.path.join(full_path, f)
        if os.path.isfile(src_path):
            shutil.copy2(src_path, src_dir)

    # Compile modules in the isolated src directory
    os.chdir(src_dir)
    if not "modules" in test_data:
        return False, "No modules!"
    for m in test_data["modules"]:
        srcs = " ".join(m)
        compile_cmd = '%s ../../scripts/mkmodule --disasm --gen-c-header --header-path .%s%%s%%s' % (sys.executable, module_target_flag)
        cmd = compile_cmd % ("" if opt else "-O3 ", srcs)
        res, out = run_cmd(cmd, show_output=False)
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

    # Build qemu test in isolated build directory
    cmake_flags = os.environ.get("UDYNLINK_CMAKE_FLAGS", "")
    platform = os.environ.get("UDYNLINK_PLATFORM", "")
    if platform:
        cmake_flags += " -DUDYNLINK_PLATFORM=%s" % platform
    if os.environ.get("UDYNLINK_TEST_DEBUG"):
        cmake_flags += " -DUDYNLINK_TEST_DEBUG_LEVEL=UDYNLINK_DEBUG_INFO"
    cmake_cmd = "cmake -B %s -S ../qemu_host -DUDYNLINK_TEST_SRC_DIR=%s -DUDYNLINK_SOURCE_DIR=%s %s" % (build_dir, src_dir, repo_root, cmake_flags)
    res, out = run_cmd(cmake_cmd, show_output=False)
    if not res:
        return False, "Unable to configure test"
    res, out = run_cmd("cmake --build %s --target test1.elf" % build_dir, show_output=False)
    if not res:
        return False, "Unable to build test"

    # Run QEMU with the freshly compiled test
    safe_print("--- Running QEMU ---")
    qemu_cmd = build_qemu_cmd(os.path.join(build_dir, "test1.elf"))
    res, out = run_cmd(qemu_cmd, timeout=default_qemu_timeout, quiet_on_error=True)
    if out is None:
        return False, "**** Unable to run QEMU or timeout running ****"
    out = out.decode()
    with open(full_path + "/output_test_%s.txt" % aopt, 'w') as fout:
        fout.write(out)
    # Check result (accept non-zero QEMU exit if output shows success)
    #
    # Legacy qemu-system-gnuarmeclipse (QEMU 2.8.0) sometimes prefixes
    # semihosting output lines with the monitor prompt "(qemu) ".  Strip
    # that prefix before regex matching so "^...$" required patterns work.
    clean_out = re.sub(r'^\(qemu\) ', '', out, flags=re.MULTILINE)
    if "*** TEST OK ***" in out:
        for t in test_data.get("required", []):
            finds = re.findall(t, clean_out, re.MULTILINE)
            if len(finds) < test_data.get("total_loads", 3): # consider each load mode in turn
                return False, "**** Can't find '%s' in output ****" % t + out
        return True, out
    return False, "**** Can't find the test OK indicator in the output ****\n" + out

total, failed = 0, 0
tests = sys.argv[1:] if len(sys.argv) > 1 else os.listdir(".")
for l in tests:
    # Look through all dirs that begin with "test-" and have a test_data.py file
    if l.startswith("test-") and os.path.isdir(l):
        for opt in [False, True]:
            res, out = test_one(os.path.abspath(l), opt)
            total += 1
            if not res:
                safe_print("--- TEST FAILED! ---")
                safe_print(out + "\n")
                failed += 1
            else:
                safe_print("--- TEST OK ---\n")

safe_print('*' * 20)
safe_print("Total:  %d" % total)
safe_print("Passed: %d" % (total - failed))
safe_print("Failed: %d" % failed)
safe_print('*' * 20)

sys.stdout.buffer.flush()
os._exit(failed)
