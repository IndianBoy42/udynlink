"""Contract tests for the standalone udynlink module image parser.

These tests defend the on-disk binary format contract that is defined once in
``scripts/udynlink_parser.py`` (mirroring ``udynlink/udynlink.c`` and the
mkmodule writer).  They must fail if any format constant, bit layout, or offset
computation drifts from the C loader's expectations.

Format constants here are *not* redeclared — they are imported from the parser
under test, so a regression in the parser's own constants is caught by the
behavioral assertions (round-trip, runtime layout) rather than by tautology.
"""

import glob
import os
import struct
import sys

import pytest

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.abspath(os.path.join(_THIS_DIR, "..", ".."))
sys.path.insert(0, os.path.join(_REPO_ROOT, "scripts"))

import udynlink_parser as p  # noqa: E402

# Any in-tree sample module is a valid fixture; the parser must handle every
# shape the toolchain emits (C, C++, weak, no-prologue, deps, wasm2c).
_SAMPLE_BINS = sorted(glob.glob(os.path.join(_REPO_ROOT, "tests", "build_*_src", "mod_*.bin")))


@pytest.fixture(scope="module")
def sample_bin():
    """Pick one well-known sample (helloworld) for targeted assertions."""
    for b in _SAMPLE_BINS:
        if os.path.basename(b) == "mod_hello.bin":
            with open(b, "rb") as f:
                return f.read()
    pytest.skip("no mod_hello.bin sample found")


# ─────────────────────────────────────────────────────────────────────────────
# Signature / version helpers
# ─────────────────────────────────────────────────────────────────────────────

def test_signature_is_ascii_udlm():
    assert p.MODULE_SIGNATURE == b"UDLM"
    # On disk the bytes are literally 'U','D','L','M'; the uint32 value packs
    # them in the order the C loader compares ((M<<24)|(L<<16)|(D<<8)|U).
    assert struct.pack("<I", p.MODULE_SIGN_VALUE) == b"UDLM"


def test_pack_version_round_trip():
    assert p.pack_version(3, 0) == 0x0300
    assert p.get_major_version(p.pack_version(1, 7)) == 1
    assert p.get_minor_version(p.pack_version(1, 7)) == 7
    assert p.pack_version(3, 0) == p.LOADER_ABI_VERSION


# ─────────────────────────────────────────────────────────────────────────────
# Header layout
# ─────────────────────────────────────────────────────────────────────────────

def test_header_struct_size_is_32_bytes():
    # struct.calcsize includes the 4-byte signature; must equal the C struct.
    assert struct.calcsize(p.HEADER_FORMAT) == p.HEADER_SIZE == 32


def test_header_offsets_match_c_loader(sample_bin):
    """Field offsets and derived offsets must match udynlink.c's helpers."""
    img = p.parse_module(sample_bin)
    h = img.header
    # get_header_size / get_code_offset_from_header / get_sym_table_pointer
    assert h.header_size == 32
    assert h.relocs_offset == 32                       # header_size
    assert h.symtab_offset == 32 + h.num_rels * 8      # + relocs
    assert h.code_offset == p.align4(h.symtab_offset + h.symt_size)
    assert h.data_offset == h.code_offset + h.code_size
    assert h.image_size == h.data_offset + h.data_size == len(sample_bin)
    assert h.metadata_size == h.code_offset            # udynlink_get_image_metadata_size


def test_header_pack_round_trips(sample_bin):
    h = p.parse_module(sample_bin).header
    assert h.pack() == sample_bin[:p.HEADER_SIZE]


# ─────────────────────────────────────────────────────────────────────────────
# Symbol table bit packing
# ─────────────────────────────────────────────────────────────────────────────

def test_symbol_type_constants_match_c_enum():
    # Mirrors udynlink.h UDYNLINK_SYM_TYPE_*
    assert (p.SYM_TYPE_INTERNAL, p.SYM_TYPE_EXPORTED, p.SYM_TYPE_EXTERN,
            p.SYM_TYPE_MODULE_NAME, p.SYM_TYPE_WEAK) == (0, 1, 2, 3, 4)


def test_symbol_info_bits_match_c_masks():
    # udynlink.c: OFFSET_MASK=0x07FFFFFF, INFO_SHIFT=27, TYPE_MASK=0x07, CODE=0x08
    assert p.SYM_NAME_OFFSET_MASK == 0x07FFFFFF
    assert p.SYM_INFO_SHIFT == 27
    assert p.SYM_INFO_TYPE_MASK == 0x07
    assert p.SYM_INFO_CODE_MASK == 0x08


def test_module_name_is_index_zero(sample_bin):
    img = p.parse_module(sample_bin)
    assert img.symbols, "symtab must be non-empty"
    name_sym = img.symbols[0]
    assert name_sym.type == p.SYM_TYPE_MODULE_NAME
    assert name_sym.name is not None
    assert name_sym.is_in_code is False          # module name has no location
    # name field's low bits point into the symtab; for the module name the
    # value word is always 0.
    assert name_sym.val_raw == 0


def test_symbol_pack_round_trips(sample_bin):
    img = p.parse_module(sample_bin)
    for s in img.symbols:
        assert s.pack() == struct.pack("<II", s.name_off_raw, s.val_raw)


# ─────────────────────────────────────────────────────────────────────────────
# Relocation bit layout
# ─────────────────────────────────────────────────────────────────────────────

def test_reloc_flag_bits_match_c_loader():
    # udynlink_load_apply_relocations: bit31 = R_ARM_ABS32, bit30 = code-base.
    assert p.RELOC_FLAG_DATA_BASE == (1 << 31)
    assert p.RELOC_FLAG_CODE_BASE == (1 << 30)
    assert p.RELOC_VALUE_MASK == 0x7FFFFFFF


def test_reloc_kind_classification():
    sym = p.Relocation(0, 5)
    assert sym.kind == p.RelocKind.SYMBOL and sym.symt_index == 5
    data = p.Relocation(0, (1 << 31) | 0x1234)
    assert data.kind == p.RelocKind.DATA_BASE and data.data_addend == 0x1234
    code = p.Relocation(0, (1 << 30) | 0x56)
    assert code.kind == p.RelocKind.CODE_BASE


# ─────────────────────────────────────────────────────────────────────────────
# Round-trip losslessness (the core parser contract)
# ─────────────────────────────────────────────────────────────────────────────

@pytest.mark.parametrize("bin_path", _SAMPLE_BINS,
                         ids=[os.path.basename(os.path.dirname(b)) + "/" + os.path.basename(b)
                              for b in _SAMPLE_BINS])
def test_parse_then_serialize_is_byte_identical(bin_path):
    """parse_module(...).to_bytes() must reproduce every byte of the source.

    This is the single strongest guard of the format: any drift in header
    field order, symtab bit packing, reloc layout, or section placement breaks
    it immediately. Covers C, C++, weak, no-prologue, dep, and wasm2c modules.
    """
    with open(bin_path, "rb") as f:
        data = f.read()
    img = p.parse_module(data)
    # Structural invariants the loader relies on.
    assert img.header.image_size == len(data), "image_size must cover whole blob"
    assert img.header.signature_ok
    if img.symbols:
        assert img.symbols[0].type == p.SYM_TYPE_MODULE_NAME
    # Lossless reserialization.
    assert img.to_bytes() == data, "round-trip altered bytes"


def test_parse_rejects_bad_signature():
    bad = bytearray(b"XXXX" + b"\x00" * 60)
    with pytest.raises(ValueError, match="bad signature"):
        p.parse_module(bytes(bad))


def test_parse_rejects_truncated_buffer():
    with pytest.raises(ValueError, match="too short"):
        p.parse_module(b"UDLM" + b"\x00" * 10)


# ─────────────────────────────────────────────────────────────────────────────
# Runtime RAM layout (mirrors udynlink.c get_code_pointer / get_data_pointer /
# get_ram_size_for_header and the COPY_ALL copy path)
# ─────────────────────────────────────────────────────────────────────────────

def test_ram_size_matches_c_formula(sample_bin):
    img = p.parse_module(sample_bin)
    h = img.header
    lot = h.num_lot * 4
    # XIP: LOT + data + bss (code stays in flash)
    assert img.runtime_layout(p.LoadMode.XIP).ram_size == lot + h.data_size + h.bss_size
    # COPY_TEXT_DATA: LOT + code + data + bss
    assert img.runtime_layout(p.LoadMode.COPY_TEXT_DATA).ram_size == lot + h.code_size + h.data_size + h.bss_size
    # COPY_ALL: LOT + metadata + code + data + bss
    assert img.runtime_layout(p.LoadMode.COPY_ALL).ram_size == lot + h.code_offset + h.code_size + h.data_size + h.bss_size


def test_section_placement_per_load_mode(sample_bin):
    img = p.parse_module(sample_bin)
    h = img.header
    base = 0x20000000

    # XIP: LOT then data then bss in RAM; code lives in the source image.
    xip = img.runtime_layout(p.LoadMode.XIP, ram_base=base)
    assert xip.code_in_ram is False
    assert xip.lot_base == base
    assert xip.data_base == base + h.num_lot * 4
    assert xip.bss_base == xip.data_base + h.data_size
    assert xip.code_in_image_offset == h.code_offset

    # COPY_TEXT_DATA: LOT, code, data, bss in RAM; header stays in image.
    ctd = img.runtime_layout(p.LoadMode.COPY_TEXT_DATA, ram_base=base)
    assert ctd.header_in_ram is False
    assert ctd.code_base == base + h.num_lot * 4
    assert ctd.data_base == ctd.code_base + h.code_size
    assert ctd.bss_base == ctd.data_base + h.data_size

    # COPY_ALL: LOT, then a full image copy (header+relocs+symt+code+data), bss.
    ca = img.runtime_layout(p.LoadMode.COPY_ALL, ram_base=base)
    assert ca.header_in_ram is True
    assert ca.header_base == base + h.num_lot * 4
    assert ca.code_base == ca.header_base + h.code_offset
    assert ca.data_base == ca.code_base + h.code_size
    assert ca.bss_base == ca.data_base + h.data_size


def test_build_ram_copies_sections_for_copy_all(sample_bin):
    img = p.parse_module(sample_bin)
    lay = img.runtime_layout(p.LoadMode.COPY_ALL, ram_base=0x20000000)
    buf = lay.build_ram()
    assert len(buf) == lay.ram_size
    # The code bytes must appear at code_base - ram_base in the COPY_ALL buffer.
    code_off = lay.code_base - lay.ram_base
    assert bytes(buf[code_off:code_off + img.header.code_size]) == img.code
    data_off = lay.data_base - lay.ram_base
    assert bytes(buf[data_off:data_off + img.header.data_size]) == img.data


def test_apply_relocations_uses_full_ram_buffer(sample_bin):
    """apply_relocations must return a buffer of exactly ram_size and one
    report per relocation."""
    img = p.parse_module(sample_bin)
    for mode in p.LoadMode:
        lay = img.runtime_layout(mode, ram_base=0x20000000)
        buf, reports = lay.apply_relocations()
        assert len(buf) == lay.ram_size
        assert len(reports) == len(img.relocations)


# ─────────────────────────────────────────────────────────────────────────────
# Architecture tag decoding
# ─────────────────────────────────────────────────────────────────────────────

def test_decode_arch_tag_fields():
    # cortex-m4: family 4, no FPU, soft float.
    d = p.decode_arch_tag(p.ARCH_TAG_CORTEX_M4)
    assert d["family"] == "cortex-m4" and d["fpu"] is False and d["float_abi"] == "soft"
    # cortex-m4f: family 4 + FPU bit + hard float.
    d = p.decode_arch_tag(p.ARCH_TAG_CORTEX_M4F)
    assert d["family"] == "cortex-m4" and d["fpu"] is True and d["float_abi"] == "hard"
    # no-prologue flag is the high bit.
    assert p.decode_arch_tag(p.ARCH_TAG_CORTEX_M4 | p.ARCH_FLAG_NO_PROLOGUE)["no_prologue"] is True


# ─────────────────────────────────────────────────────────────────────────────
# Dedupe invariant: test_symbol_filter's parser now delegates to udynlink_parser
# ─────────────────────────────────────────────────────────────────────────────

def test_symbol_filter_parse_symtab_uses_parser(sample_bin, tmp_path):
    """The refactored test helper must return the same shape as the parser."""
    from test_symbol_filter import _parse_symtab  # noqa: E402  (after sys.path insert above)
    bin_path = tmp_path / "sample.bin"
    bin_path.write_bytes(sample_bin)
    entries = _parse_symtab(str(bin_path))
    img = p.parse_module(sample_bin)
    assert len(entries) == len(img.symbols)
    for e, s in zip(entries, img.symbols):
        assert e["name"] == s.name
        assert e["type"] == s.type
        assert e["value"] == s.val_raw
