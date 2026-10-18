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
        # The message states the 4-byte arena floor the loader provides; the
        # exact phrasing is not the contract, so match only the figure.
        assert "4-byte" in warnings[0]


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


_ALIGNED_DATA_SRC = (
    '#include <stdint.h>\n'
    '#include "udynlink_section.h"\n'
    '/* aligned(8) in .data forces a .text->.data gap (the linker script\n'
    ' * starts .data with ALIGN(4)). */\n'
    '__attribute__((aligned(8))) uint64_t pair[2] = {1, 2};\n'
    'static uint64_t *data_ptr = &pair[1];\n'
    'UDYNLINK_SECTION("alt") volatile uint32_t marker;\n'
    'uint64_t *get_ptr(void) { return data_ptr; }\n'
)


@skip_no_arm_gcc
class TestMkmoduleSectionedGap:
    """A sectioned image whose `.data` needs more alignment than the linker
    script gives it starts after a `.text`->`.data` gap.

    The gap must be folded into the CODE payload: the loader resolves
    relocation targets through `code_size + 4*(lot - num_lot)` and writes at
    `va - entry.va`, so a `.data` payload shifted by the gap applies every
    data relocation one word off (and the loader rejects the image)."""

    def _build(self, tmp_path, *extra):
        # -O3 reliably ends .text at 4 mod 8 for this source, which is what
        # makes the .text->.data gap deterministic across toolchains.
        return _run_source(tmp_path, _ALIGNED_DATA_SRC, "-I",
                           os.path.join(_REPO_ROOT, "udynlink"),
                           "--section", "alt", "-O", "3", *extra)

    def test_gap_is_folded_into_the_code_payload(self, tmp_path):
        import struct
        res = self._build(tmp_path)
        assert res.returncode == 0, res.stdout + res.stderr
        img = (tmp_path / "mod.bin").read_bytes()
        (_, _, _, _, _, num_rels, flags, symt_size,
         code_size, data_size, bss_size) = struct.unpack_from("<4sHHHHHHIIII", img, 0)

        num_secs = (flags >> 1) & 0x3F
        sectab_off = (32 + 8 * num_rels + symt_size + 3) & ~3
        entries = [struct.unpack_from("<IIIIII", img, sectab_off + 24 * i)
                   for i in range(num_secs)]
        code_va, code_tbl_size = entries[0][1], entries[0][2]
        data_va, data_tbl_size = entries[1][1], entries[1][2]

        # The linker really did leave a gap for this module.
        assert data_va > code_size, "test module no longer produces a gap"
        # .text's table entry covers code + gap, so .data starts exactly at
        # .text's end; header code_size stays the code's real length.
        assert code_va == 0
        assert code_tbl_size == data_va
        assert data_tbl_size == data_size

        # Payloads are stored at their VA offset: [text(+gap)][data][tagged].
        payload_off = sectab_off + 24 * num_secs
        code_payload = img[payload_off:payload_off + code_tbl_size]
        data_payload = img[payload_off + code_tbl_size:
                           payload_off + code_tbl_size + data_tbl_size]
        # The gap bytes are zeros and .data's real content starts at .data's VA.
        assert code_payload[code_size:] == bytes(data_va - code_size)
        assert data_payload[:8] == struct.pack("<Q", 1), "data payload not at .data's VA"
        # The gap bytes belong to no symbol, so the image ends where the
        # payload concat says (no hole, no double count).
        expected = payload_off + code_tbl_size + data_tbl_size
        for name_off, va, size, _align, cls, _flg in entries[2:]:
            if cls != 2:                      # BSS has no payload
                expected += size
        assert expected == len(img)


_IMPORTS_SRC = (
    "#include <stdio.h>\n"
    "extern int host_scale(int);\n"
    "extern int host_offset;\n"
    "int module_entry(int v) { printf(\"v=%d\\n\", v); return host_scale(v) + host_offset; }\n"
)


@skip_no_arm_gcc
class TestMkmoduleImportsHeader:
    """--gen-imports-header: the host-side symbol contract. The import list is
    read back from the built image, so the header describes what the loader
    will actually have to resolve (not what the sources mentioned)."""

    def _run(self, tmp_path, src_text, *args):
        return _run_source(tmp_path, src_text, "--header-path", str(tmp_path), *args)

    def test_lists_undefined_symbols_only(self, tmp_path):
        res = self._run(tmp_path, _IMPORTS_SRC, "--gen-imports-header")
        assert res.returncode == 0, res.stdout + res.stderr
        header = (tmp_path / "mod_imports.h").read_text()
        for name in ("host_scale", "host_offset", "printf"):
            assert '"%s",' % name in header
        # Defined exports and locals are not imports.
        assert '"module_entry"' not in header
        assert "#ifndef MOD_IMPORTS_H" in header
        assert "static inline const char *const *mod_imports(size_t *count)" in header

    def test_self_contained_module_reports_no_imports(self, tmp_path):
        res = self._run(tmp_path, _SRC, "--gen-imports-header")
        assert res.returncode == 0, res.stdout + res.stderr
        header = (tmp_path / "mod_imports.h").read_text()
        assert "No imports" in header
        assert "names[]" not in header
        assert "return NULL;" in header

    def test_gen_c_header_does_not_claim_imports_header(self, tmp_path):
        """--gen-c-header writes the image header only. The <bin>_imports.h
        name belongs to the wrapper pipelines' own contract headers
        (mkwasm2c-module writes prototypes there), so mkmodule must not
        implicitly overwrite it."""
        res = self._run(tmp_path, _IMPORTS_SRC, "--gen-c-header")
        assert res.returncode == 0, res.stdout + res.stderr
        assert (tmp_path / "mod_module_data.h").is_file()
        assert not (tmp_path / "mod_imports.h").exists()


_CPP_ALIGNED_CTOR_SRC = """
struct G { int v; G() : v(7) {} };
static G g_value;
/* alignas(32) puts .bss on a 32-byte alignment, so the loader places its
 * runtime base past .data's end by an alignment pad. */
alignas(32) static char g_buf[64];
extern "C" int module_entry(void) { g_buf[0] = 1; return g_value.v + g_buf[0]; }
"""


@skip_no_arm_gcc
def test_init_array_end_stays_inside_data(tmp_path):
    """__init_array_end must not sit on the .data/.bss boundary.

    The section table describes a flat link VA space; the loader places each
    main section at its own aligned base. A boundary symbol that also equals
    .data's end is then classified as the first .bss address, which the
    loader aligns up — so cpp_init_fini walks past the real array and calls
    the alignment padding (zeros). The linker script keeps the end symbol
    strictly inside .data; a global ctor plus an over-aligned .bss is the
    shape that exposed it.
    """
    sys.path.insert(0, os.path.join(_REPO_ROOT, "scripts"))
    import udynlink_parser as up

    src = tmp_path / "mod.cpp"
    src.write_text(_CPP_ALIGNED_CTOR_SRC)
    cmd = [_PY, _MKMODULE, "--workdir", str(tmp_path), "--module-name", "modx",
           "--bin-name", str(tmp_path / "mod.bin"), "--tag-on-align", str(src)]
    res = subprocess.run(cmd, capture_output=True, text=True)
    assert res.returncode == 0, res.stdout + res.stderr

    img = up.parse_module((tmp_path / "mod.bin").read_bytes())
    main = [s for s in img.sections if s.name is None]
    data = next(s for s in main if s.class_name == "DATA")
    bss = next(s for s in main if s.class_name == "BSS")
    start = img.lookup_symbol("__init_array_start")
    end = img.lookup_symbol("__init_array_end")
    assert start is not None and end is not None
    assert end.val_raw > start.val_raw, "the fixture must produce a ctor"
    assert end.val_raw < data.va + data.size, (
        "__init_array_end at %#x is not inside .data [%#x,%#x)"
        % (end.val_raw, data.va, data.va + data.size))
    assert bss.va == (data.va + data.size + bss.align - 1) & ~(bss.align - 1), (
        "the loader validates bss.va == align_up(data end, bss align)")
