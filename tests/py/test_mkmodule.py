"""Argument-handling regression tests for scripts/mkmodule.

mkmodule compiles every non-option argument as a module source. These tests
pin that contract: unrecognized dash-prefixed tokens are rejected with a
pointed error from mkmodule itself (they used to be handed to gcc as input
files, surfacing as a confusing "unrecognized command-line option"), while
the documented forms — positional sources, leading ``-D`` definitions, and a
``--`` separator — keep working. Skipped when arm-none-eabi-gcc is missing.
"""

import os
import shutil
import subprocess
import sys

import pytest

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.abspath(os.path.join(_THIS_DIR, "..", ".."))
_MKMODULE = os.path.join(_REPO_ROOT, "scripts", "mkmodule")
_PY = sys.executable

_SRC = "int module_entry(void) { return 42; }\n"

skip_no_arm_gcc = pytest.mark.skipif(
    shutil.which("arm-none-eabi-gcc") is None,
    reason="arm-none-eabi-gcc not available",
)


def _run_mkmodule(workdir, *args):
    src = workdir / "mod.c"
    src.write_text(_SRC)
    cmd = ([_PY, _MKMODULE, "--workdir", str(workdir)]
           + list(args) + [str(src)])
    return subprocess.run(cmd, capture_output=True, text=True)


@skip_no_arm_gcc
class TestMkmoduleArgs:
    def test_unknown_flag_rejected_not_compiled(self, tmp_path):
        """A leaked wrapper flag (real case: a stray '--extra-source' made
        gcc try to compile an input file literally named '--extra-source')
        fails inside mkmodule with a pointed error."""
        res = _run_mkmodule(tmp_path, "--extra-source", "whatever.c")
        assert res.returncode != 0
        assert "unrecognized option" in res.stderr
        assert "--extra-source" in res.stderr
        # The error must come from mkmodule's parser, not from gcc choking
        # on the flag after it leaked into the compile command.
        assert "unrecognized command-line option" not in res.stderr

    def test_double_dash_before_sources(self, tmp_path):
        """One leading '--' separates forwarded wrapper args from sources."""
        res = _run_mkmodule(tmp_path, "--")
        assert res.returncode == 0, res.stdout + res.stderr
        assert (tmp_path / "mod.bin").is_file()

    def test_leading_defs_still_supported(self, tmp_path):
        """Leading -D macros precede the source list ('first definitions,
        then files') and still compile."""
        res = _run_mkmodule(tmp_path, "-DMODULE_LEVEL=2")
        assert res.returncode == 0, res.stdout + res.stderr
        assert (tmp_path / "mod.bin").is_file()

    def test_dash_token_after_defs_rejected(self, tmp_path):
        """-D macros are only honored BEFORE the first source; a leaked flag
        after them is named and rejected, not compiled. (--wrapper-prefix is
        a mkwasm2c-module flag mkmodule does not define; -O would be a poor
        probe — mkmodule defines -O itself and argparse consumes it.)"""
        res = _run_mkmodule(tmp_path, "-DMODULE_LEVEL=2", "--wrapper-prefix", "w2c_")
        assert res.returncode != 0
        assert "unrecognized option" in res.stderr
        assert "--wrapper-prefix" in res.stderr
