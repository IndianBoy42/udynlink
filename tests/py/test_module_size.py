"""Tests for scripts/module-size.

The tool reports numbers the image format defines, so the assertions compare
its output against the image bytes and against udynlink_parser (the format's
single source of truth): what a host pays in flash, in main-block RAM per
load mode, and in host-allocated tagged sections. Built images come from
mkmodule, so the tests skip without the ARM toolchain.
"""

import json
import os
import shutil
import subprocess
import sys

import pytest

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.abspath(os.path.join(_THIS_DIR, "..", ".."))
sys.path.insert(0, os.path.join(_REPO_ROOT, "scripts"))

import udynlink_parser as p  # noqa: E402

_PY = sys.executable
_SIZE = os.path.join(_REPO_ROOT, "scripts", "module-size")
_MKMODULE = os.path.join(_REPO_ROOT, "scripts", "mkmodule")

skip_no_arm_gcc = pytest.mark.skipif(
    shutil.which("arm-none-eabi-gcc") is None,
    reason="arm-none-eabi-gcc not available",
)

_SRC = (
    "#include <stdio.h>\n"
    "extern int host_only(int);\n"
    "static int table[4] = {1, 2, 3, 4};\n"
    'int module_entry(int v) { printf("%d\\n", v); return host_only(v) + table[v & 3]; }\n'
)

_SEC_SRC = (
    '#include "udynlink_section.h"\n'
    'UDYNLINK_SECTION_ALIGNED("dma", 32) volatile unsigned char dma_buf[64];\n'
    "int module_entry(int v) { dma_buf[0] = (unsigned char)v; return dma_buf[0]; }\n"
)


def _build(workdir, src_text, *args):
    src = workdir / "mod.c"
    src.write_text(src_text)
    cmd = ([_PY, _MKMODULE, "--workdir", str(workdir),
            "--bin-name", str(workdir / "mod.bin")] + list(args) + [str(src)])
    res = subprocess.run(cmd, capture_output=True, text=True)
    assert res.returncode == 0, res.stdout + res.stderr
    return workdir / "mod.bin"


@pytest.fixture(scope="module")
def mod_bin(tmp_path_factory):
    if shutil.which("arm-none-eabi-gcc") is None:
        pytest.skip("arm-none-eabi-gcc not available")
    return _build(tmp_path_factory.mktemp("module-size"), _SRC)


def _run(*args):
    return subprocess.run([_PY, _SIZE] + list(args), capture_output=True,
                          text=True)


@skip_no_arm_gcc
def test_reports_the_image_and_its_imports(mod_bin):
    res = _run(str(mod_bin))
    assert res.returncode == 0, res.stderr
    assert "mod — UDLM image" in res.stdout
    assert "%d B" % mod_bin.stat().st_size in res.stdout
    # The RAM columns must grow with what each load mode copies: the main
    # block is LOT+data+bss under XIP, plus code under COPY_TEXT_DATA, plus
    # the metadata under COPY_ALL.
    img = p.parse_module(mod_bin.read_bytes())
    report = json.loads(_run("--json", str(mod_bin)).stdout)["modules"][0]
    ram = report["ram"]
    assert ram["copy_all"] > ram["copy_text_data"] > ram["xip"]
    assert ram["xip"] == img.runtime_layout(p.LoadMode.XIP).ram_size
    assert report["relocations"] == len(img.relocations)
    assert report["named_symbols"] == img.num_named_symbols
    assert report["name"] == img.module_name


@skip_no_arm_gcc
def test_tagged_sections_are_reported_separately(tmp_path):
    """A sectioned image's per-section host allocations are not part of the
    main block, so they get their own accounting."""
    bin_path = _build(tmp_path, _SEC_SRC, "-I",
                      os.path.join(_REPO_ROOT, "udynlink"),
                      "--section", "dma:align=32")
    report = json.loads(_run("--json", str(bin_path)).stdout)["modules"][0]
    tagged = [s for s in report["sections"] if not s["main"]]
    assert [s["name"] for s in tagged] == ["dma"]
    assert tagged[0]["size"] == 64 and tagged[0]["align"] == 32
    assert report["tagged_ram"] == 64
    text = _run(str(bin_path)).stdout
    assert "tagged sections" in text and "dma" in text


def test_a_non_image_is_an_error(tmp_path):
    junk = tmp_path / "not-a-module.bin"
    junk.write_bytes(b"nope")
    res = _run(str(junk))
    assert res.returncode == 1
    assert "not-a-module.bin" in res.stderr
    assert res.stdout == ""
