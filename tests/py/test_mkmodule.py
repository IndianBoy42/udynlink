"""Regression tests for scripts/mkmodule.

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

_FASTDATA_SRC = (
    '__attribute__((section(".fastdata"))) int x = 1;\n'
    "int module_entry(void) { return x; }\n"
)

_ALIGNED_SRC = (
    "__attribute__((aligned(32))) int y = 1;\n"
    "__attribute__((aligned(16))) int z;\n"
    "int module_entry(void) { return y + z; }\n"
)


def _run_source(workdir, src_text, *args):
    src_path = workdir / "mod.c"
    src_path.write_text(src_text)
    cmd = ([_PY, _MKMODULE, "--workdir", str(workdir)]
           + list(args) + [str(src_path)])
    return subprocess.run(cmd, capture_output=True, text=True)


@skip_no_arm_gcc
class TestMkmoduleDiagnostics:
    def test_unknown_section_names_section_and_symbol(self, tmp_path):
        """A custom section attribute has no slot in the packed image; mkmodule
        must name the offending section and symbol instead of dying with a
        KeyError from its internal section-index map."""
        res = _run_source(tmp_path, _FASTDATA_SRC)
        assert res.returncode != 0
        assert ".fastdata" in res.stderr
        assert "'x'" in res.stderr
        assert "Traceback" not in res.stderr

    def test_aligned_data_warns_once_and_still_builds(self, tmp_path):
        """The loader guarantees only 4-byte alignment for module data, so
        aligned(32)/aligned(16) variables must produce exactly one build-time
        warning naming the alignment — and the module must still build: the
        .text->.data alignment gap is packed into the image the same way the
        .data->.bss gap always was."""
        res = _run_source(tmp_path, _ALIGNED_SRC)
        assert res.returncode == 0, res.stdout + res.stderr
        assert (tmp_path / "mod.bin").is_file()
        warnings = [l for l in res.stderr.splitlines() if l.startswith("WARNING:")]
        assert len(warnings) == 1
        assert "32-byte alignment" in warnings[0]
        assert "4-byte alignment" in warnings[0]


@skip_no_arm_gcc
class TestMkmoduleSections:
    """--section: CLI guards, spec validation, and the sectioned image
    shape (header flag bits + entries-only table + interned names)."""

    _SEC_SRC = (
        '#include "udynlink_section.h"\n'
        'UDYNLINK_SECTION("dtcm") volatile int dtcm_counter = 7;\n'
        'UDYNLINK_SECTION_ALIGNED("dma", 32) volatile unsigned char dma_buf[64];\n'
        'int module_entry(int v) { dtcm_counter += 1; dma_buf[0] = (unsigned char)v; return dtcm_counter + dma_buf[0]; }\n'
    )

    def _run_sec(self, tmp_path, *args):
        return _run_source(tmp_path, self._SEC_SRC, "-I",
                           os.path.join(_REPO_ROOT, "udynlink"), *args)

    def test_pc_rel_rejected(self, tmp_path):
        res = self._run_sec(tmp_path, "--pc-rel", "--section", "dtcm")
        assert res.returncode != 0
        assert "--pc-rel" in res.stderr and "--section" in res.stderr

    def test_no_long_calls_rejected(self, tmp_path):
        res = self._run_sec(tmp_path, "--no-long-calls", "--section", "dtcm")
        assert res.returncode != 0
        assert "--no-long-calls" in res.stderr

    def test_explicit_old_version_rejected(self, tmp_path):
        res = self._run_sec(tmp_path, "--section", "dtcm", "--udynlink-version", "3.0")
        assert res.returncode != 0
        assert "3.1" in res.stderr

    def test_unknown_flag_names_valid_set(self, tmp_path):
        res = self._run_sec(tmp_path, "--section", "dtcm:flags=BOGUS")
        assert res.returncode != 0
        assert "NOCACHE" in res.stderr and "DMA" in res.stderr and "SHARED" in res.stderr

    def test_bad_align_rejected(self, tmp_path):
        for bad in ("3", "0", "48"):
            res = self._run_sec(tmp_path, "--section", "dtcm:align=" + bad)
            assert res.returncode != 0
            assert "power of two" in res.stderr

    def test_reserved_name_rejected(self, tmp_path):
        res = self._run_sec(tmp_path, "--section", "main")
        assert res.returncode != 0
        assert "main" in res.stderr

    def test_undeclared_tag_rejected(self, tmp_path):
        src = self._SEC_SRC.replace('UDYNLINK_SECTION("dtcm")', 'UDYNLINK_SECTION("other")')
        res = _run_source(tmp_path, src, "-I", os.path.join(_REPO_ROOT, "udynlink"),
                          "--section", "dtcm", "--section", "dma")
        assert res.returncode != 0
        assert "--section" in res.stderr and "other" in res.stderr

    def test_sectioned_image_shape(self, tmp_path):
        """A sectioned build declares the flag+count bits, version >= 3.1,
        and carries a VA-sorted entries-only table whose payloads follow in
        ascending-VA order skipping BSS (struct-parse; no loader involved)."""
        import struct
        res = self._run_sec(tmp_path, "--section", "dtcm",
                            "--section", "dma:align=32:flags=DMA,NOCACHE")
        assert res.returncode == 0, res.stdout + res.stderr
        img = (tmp_path / "mod.bin").read_bytes()
        sign, _, uv, _, _, num_rels, flags, symt_size, code_size, data_size, bss_size = \
            struct.unpack_from("<4sHHHHHHIIII", img, 0)
        assert sign == b"UDLM"
        assert flags & 1 and 1 <= (flags >> 1) & 0x3F <= 63
        assert uv >= (3 << 8 | 1)
        num_secs = (flags >> 1) & 0x3F
        sectab_off = (32 + 8 * num_rels + symt_size + 3) & ~3
        entries = [struct.unpack_from("<IIIIII", img, sectab_off + 24 * i)
                   for i in range(num_secs)]
        vas = [e[1] for e in entries]
        assert vas == sorted(vas)
        # main sections first: code/data/bss, MAIN-flagged, unnamed
        assert [e[4] for e in entries[:3]] == [0, 1, 2]
        assert all(e[5] & 0x80000000 for e in entries[:3])
        assert all(e[0] == 0 for e in entries[:3])
        # tagged names interned in the symtab pool
        pool = img[32 + 8 * num_rels:32 + 8 * num_rels + symt_size]
        names = []
        for name_off, va, size, align, cls, flg in entries[3:]:
            assert not (flg & 0x80000000)
            names.append(pool[name_off:pool.index(b"\0", name_off)].decode())
        assert names == ["dtcm", "dma"]
        # payloads: ascending VA, skipping BSS, image fully consumed
        off = sectab_off + 24 * num_secs
        for e in entries:
            if e[4] == 2:
                continue
            off += e[2]
        assert off == len(img)
