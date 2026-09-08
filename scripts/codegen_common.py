"""Shared machinery for udynlink "codegen integration" pipelines.

A codegen integration compiles a domain artifact (a .proto, a .wasm, a state
chart, ...) into a self-contained UDLM module: data/schema tables ship inside
the module, a thin generated wrapper exposes a fixed prefixed C ABI, and a
small closed set of runtime functions either stays host-side (bound by name at
load time) or is absent entirely (fully self-contained modules, zero externs).

This module is the supported surface for writing your own integration script;
the recipe and the contract rules are documented in
docs/codegen-integrations.md.  scripts/proto2module and
scripts/mkwasm2c-module are both built on it — read them as reference.
"""

import os
import re
import subprocess
import sys

SCRIPTS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPTS_DIR)
MKMODULE = os.path.join(SCRIPTS_DIR, "mkmodule")

_C_IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def find_tool(value, env_name, candidates, hint):
    """Resolve an external tool: explicit value, then env var, then candidates.

    Candidates may be bare names (looked up on PATH) or paths (used verbatim
    when they exist, e.g. a repo-local pinned install like tools/wabt/bin/wasm2c).
    """
    if value:
        return value
    if os.environ.get(env_name):
        return os.environ[env_name]
    from shutil import which
    for c in candidates:
        if os.path.basename(c) != c:
            # path-like candidate (e.g. a repo-local pinned install):
            # use verbatim when it exists
            if os.path.isfile(c):
                return c
        else:
            found = which(c)
            if found:
                return found
    sys.exit("error: %s" % hint)


def sanitize_identifier(name):
    """Sanitize an arbitrary string into a C identifier chunk (`my-sensor` ->
    `my_sensor`).  Deterministic, so export names stay reproducible."""
    return re.sub(r"[^A-Za-z0-9_]", "_", name)


def check_identifiers(names, what="name"):
    """Exit loudly if any name is not a valid C identifier."""
    for n in names:
        if not _C_IDENT.match(n):
            sys.exit("error: %s '%s' is not a valid C identifier" % (what, n))


def api_header_guard(base):
    """Include guard for a generated contract header (`my-sensor` ->
    `MY_SENSOR`)."""
    return sanitize_identifier(base).upper()


def run_tool(cmd, what, verbose=True):
    """Run an external tool with argv-list semantics (no shell quoting or
    injection); output passes through, failures are loud."""
    if verbose:
        print("[Executing] %s" % " ".join(cmd))
    res = subprocess.run(cmd)
    if res.returncode != 0:
        sys.exit("error: %s failed (exit code %d)" % (what, res.returncode))


def build_module(sources, *, public_symbols=None, strip_non_public=False,
                 build_flags="", target=None, mcpu=None, opt_level=None,
                 bin_name=None, module_name=None, workdir=None,
                 gen_c_header=False, header_path=None, no_prologue=False,
                 mod_version=None, udynlink_version=None, extra_args=(),
                 verbose=True):
    """Compile + package `sources` into a UDLM image via scripts/mkmodule.

    Returns the path of the produced .bin: `bin_name` if given (including a
    --bin-name that arrived inside `extra_args`), otherwise
    `<workdir>/<module_name>.bin` where module_name defaults to the first
    source's basename (mkmodule's own naming rule).

    `public_symbols` is sorted before passing (reproducible symtabs).
    `strip_non_public` demotes every other defined symbol to a nameless
    internal entry — the wrappers are the module's API; internals stay out
    of the symtab (relocations resolve by offset, so addressing is
    unaffected).  `extra_args` are appended verbatim (e.g. user-provided
    flags collected after `--`).
    """
    cmd = [sys.executable, MKMODULE]
    if target:
        cmd += ["--target", target]
    if mcpu:
        cmd += ["--mcpu", mcpu]
    if opt_level is not None:
        cmd += ["-O", opt_level]
    if bin_name:
        cmd += ["--bin-name", bin_name]
    if module_name:
        cmd += ["--module-name", module_name]
    if workdir:
        cmd += ["--workdir", workdir]
    if gen_c_header:
        cmd += ["--gen-c-header", "--header-path", header_path or "."]
    if public_symbols:
        cmd += ["--public-symbols", ",".join(sorted(public_symbols))]
        if strip_non_public:
            cmd.append("--strip-non-public-syms")
    if build_flags:
        # '=' form: the value typically starts with '-I...' and argparse
        # would otherwise treat it as an option
        cmd += ["--build-flags=%s" % build_flags]
    if no_prologue:
        cmd.append("--no-prologue")
    if mod_version:
        cmd += ["--mod-version", mod_version]
    if udynlink_version:
        cmd += ["--udynlink-version", udynlink_version]
    if not verbose:
        cmd.append("--no-verbose")
    cmd += list(extra_args)
    cmd += list(sources)
    run_tool(cmd, "mkmodule", verbose=verbose)

    # a --bin-name arriving via extra_args overrides the default naming
    if bin_name is None:
        for i, a in enumerate(extra_args):
            if a == "--bin-name" and i + 1 < len(extra_args):
                bin_name = extra_args[i + 1]
            elif a.startswith("--bin-name="):
                bin_name = a.split("=", 1)[1]
    if bin_name:
        return bin_name
    name = module_name or os.path.splitext(os.path.basename(sources[0]))[0]
    return os.path.join(workdir or os.path.dirname(os.path.abspath(sources[0]))
                        or ".", name + ".bin")


def report_size(bin_path, module_name=None):
    """Print an image-bytes + RAM-per-load-mode summary.  Informational only:
    any failure to parse is silently ignored so pipelines never break on it."""
    try:
        sys.path.insert(0, SCRIPTS_DIR)
        from udynlink_parser import parse_module  # noqa: E402
        mod = parse_module(open(bin_path, "rb").read())
        h = mod.header
        lot = h.num_lot * 4
        header_off = (32 + h.num_rels * 8 + h.symt_size + 3) & ~3
        print("module %s: %d B image | RAM: XIP %d, COPY_TEXT_DATA %d, "
              "COPY_ALL %d" % (
                  module_name or os.path.basename(bin_path),
                  h.code_size + h.data_size + header_off,
                  lot + h.data_size + h.bss_size,
                  lot + h.code_size + h.data_size + h.bss_size,
                  lot + header_off + h.code_size + h.data_size + h.bss_size))
    except Exception:
        pass
