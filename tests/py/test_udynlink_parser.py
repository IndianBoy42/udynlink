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
import re
import shutil
import struct
import subprocess
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
    assert p.pack_version(3, 1) == 0x0301
    assert p.get_major_version(p.pack_version(1, 7)) == 1
    assert p.get_minor_version(p.pack_version(1, 7)) == 7
    # The tree's loader is ABI 3.1 (section table); untagged images keep
    # declaring 3.0 so old loaders still accept them.
    assert p.pack_version(3, 1) == p.LOADER_ABI_VERSION


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
    # Structural invariants the loader relies on. Sectioned images derive
    # image_size from the parsed table (header.image_size is untagged-only);
    # both must cover the whole blob.
    assert img.image_size == len(data), "image_size must cover whole blob"
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


# ─────────────────────────────────────────────────────────────────────────────
# Sectioned images (loader ABI 3.1): table decode, placement, resolution
#
# The tests below build synthetic images from parts so every layout number is
# hand-computable; the builder mirrors the mkmodule pack sequence via the
# parser's own constants.
# ─────────────────────────────────────────────────────────────────────────────

_TAG_VA = p.TAGGED_REGION_VA_STRIDE          # 0x02000000: first tagged region


def _sym_word(name_off: int, val: int, sym_type: int, is_code: bool) -> bytes:
    info = sym_type | (p.SYM_INFO_CODE_MASK if is_code else 0)
    return struct.pack("<II", (info << p.SYM_INFO_SHIFT) | name_off, val)


def _sectioned_specs(code_size, data_size, bss_size=0, tagged=(), main_aligns=(4, 4, 4)):
    """Full table specs: the three main sections (indices 0/1/2) plus tagged
    ones, each in its own region at 0x02000000*(k+1) per the mkmodule linker
    template. Tagged entry: (name, size, align, cls, flags)."""
    specs = [
        (".text", 0, code_size, main_aligns[0], p.SEC_CLASS_CODE, p.SEC_FLAG_MAIN),
        (".data", code_size, data_size, main_aligns[1], p.SEC_CLASS_DATA, p.SEC_FLAG_MAIN),
        (".bss", code_size + data_size, bss_size, main_aligns[2], p.SEC_CLASS_BSS, p.SEC_FLAG_MAIN),
    ]
    for k, (name, size, align, cls, flags) in enumerate(tagged):
        specs.append((name, _TAG_VA * (k + 1), size, align, cls, flags))
    return specs


def _build_image(*, version=(3, 0), num_lot=2, symbols=(), relocs=(),
                 sectioned=None, payloads=None, bss_size=0):
    """Assemble a UDLM image from parts (mirrors the mkmodule pack order).

    sectioned=None builds an untagged legacy image sized from payloads
    ({0: code, 1: data}); otherwise ``sectioned`` is the full spec list and
    ``payloads`` maps section index -> payload bytes. ``symbols`` entries are
    (name|None, val, sym_type, is_code); symbol and section names share the
    symbol-table string pool.
    """
    payloads = dict(payloads or {})
    tagged = sectioned is not None
    specs = sectioned
    if specs is None:
        code, data = payloads.get(0, b""), payloads.get(1, b"")
        specs = [
            (None, 0, len(code), 4, p.SEC_CLASS_CODE, p.SEC_FLAG_MAIN),
            (None, len(code), len(data), 4, p.SEC_CLASS_DATA, p.SEC_FLAG_MAIN),
            (None, len(code) + len(data), bss_size, 4, p.SEC_CLASS_BSS, p.SEC_FLAG_MAIN),
        ]
    entries_start = 4 + 8 * len(symbols)
    pool = bytearray()
    name_offs = {}

    def intern(name):
        if name is None:
            return 0
        if name not in name_offs:
            name_offs[name] = entries_start + len(pool)
            pool.extend(name.encode() + b"\x00")
        return name_offs[name]

    for (name, _val, _typ, _code) in symbols:
        intern(name)
    sec_name_offs = [intern(spec[0]) for spec in specs]

    symtab = struct.pack("<I", len(symbols))
    for (name, val, typ, is_code) in symbols:
        symtab += _sym_word(intern(name), val, typ, is_code)
    symtab += bytes(pool)

    relocs_bytes = b"".join(struct.pack("<II", lo, v) for lo, v in relocs)
    symtab_off = p.HEADER_SIZE + len(relocs_bytes)
    sectab = b""
    if tagged:
        # Entries only — the count rides in the header's flags bits 7:1.
        sectab = b"".join(
            struct.pack(p.SECTION_ENTRY_FORMAT, noff, va, size, align, cls, flags)
            for (name, va, size, align, cls, flags), noff in zip(specs, sec_name_offs))
        sectab_off = p.align4(symtab_off + len(symtab))
        code_off = p.align4(sectab_off + len(sectab))
    else:
        code_off = p.align4(symtab_off + len(symtab))

    # Payloads concatenated in ascending VA order, skipping BSS (contract 1.3).
    body = bytearray()
    for idx, spec in enumerate(specs):
        (_name, _va, size, _align, cls, _flags) = spec
        if cls == p.SEC_CLASS_BSS:
            continue
        payload = payloads.get(idx, b"")
        assert len(payload) <= size, "payload %d exceeds declared size" % idx
        body += payload + b"\x00" * (size - len(payload))

    flags_word = (p.UDYNLINK_HDR_FLAG_SECTIONS | (len(specs) << p.UDYNLINK_HDR_SECTION_COUNT_SHIFT)) if tagged else 0
    header = struct.pack(p.HEADER_FORMAT, p.MODULE_SIGNATURE, 1, p.pack_version(*version),
                         p.ARCH_TAG_CORTEX_M4, num_lot, len(relocs), flags_word,
                         len(symtab), specs[0][2], specs[1][2], specs[2][2])
    img = bytearray(header) + relocs_bytes + symtab
    if sectab:
        img += b"\x00" * (sectab_off - len(img)) + sectab
    img += b"\x00" * (code_off - len(img))
    img += body
    return bytes(img)


# ─────────────────────────────────────────────────────────────────────────────
# Untagged images must behave exactly like the pre-3.1 parser
# ─────────────────────────────────────────────────────────────────────────────

def test_untagged_synthetic_matches_today():
    """Synthetic untagged image: geometry, layout, resolution, relocations and
    the single (ram_size, None, 4, 0) allocation call are today's exactly."""
    # In-place .data words hold the link-time values: word0 carries the ABS32
    # addend (4), word1 is the code-base offset the TARGET1 reloc adds to.
    code = bytes(range(0x10))
    data = struct.pack("<I", 4) + struct.pack("<I", 0x1234)
    symbols = [
        ("mod", 0, p.SYM_TYPE_MODULE_NAME, False),
        ("func", 4, p.SYM_TYPE_EXPORTED, True),
        ("var", 0, p.SYM_TYPE_EXPORTED, False),
    ]
    relocs = [(0, 2), (2, (1 << 31) | 4), (3, 1 << 30)]
    raw = _build_image(version=(3, 0), symbols=symbols, relocs=relocs,
                       payloads={0: code, 1: data})
    img = p.parse_module(raw)
    h = img.header
    lot = h.num_lot * 4
    assert not h.has_sections and h.flags == 0
    assert h.code_offset == p.align4(h.symtab_offset + h.symt_size)
    assert h.data_offset == h.code_offset + 16 and h.image_size == len(raw)
    # Synthesized reporting view only: three MAIN entries, align 4, overlapping
    # code/data VA ranges — never consulted for resolution.
    assert [(s.va, s.size, s.cls, s.align) for s in img.sections] == [
        (0, 16, p.SEC_CLASS_CODE, 4), (16, 8, p.SEC_CLASS_DATA, 4),
        (24, 0, p.SEC_CLASS_BSS, 4)]
    assert all(s.is_main and s.synthetic and s.name is None for s in img.sections)

    base = 0x20000000
    for mode, size in ((p.LoadMode.XIP, lot + 8),
                       (p.LoadMode.COPY_TEXT_DATA, lot + 16 + 8),
                       (p.LoadMode.COPY_ALL, lot + h.code_offset + 16 + 8)):
        lay = img.runtime_layout(mode, ram_base=base)
        assert lay.ram_size == size, mode
        assert lay.allocation_plan() == [p.AllocationCall(size, None, 4, 0)]
    ctd = img.runtime_layout(p.LoadMode.COPY_TEXT_DATA, ram_base=base)
    assert (ctd.code_base, ctd.data_base, ctd.bss_base) == \
        (base + lot, base + lot + 16, base + lot + 24)
    ca = img.runtime_layout(p.LoadMode.COPY_ALL, ram_base=base)
    assert ca.header_base == base + lot
    assert (ca.code_base, ca.data_base) == (base + lot + h.code_offset,
                                            base + lot + h.code_offset + 16)

    # Resolution keeps the SYM_INFO_CODE_MASK branch (val is section-relative).
    lay = img.runtime_layout(p.LoadMode.COPY_TEXT_DATA, ram_base=base)
    assert lay.resolve_symbol(img.symbols[1]).address == base + lot + 4   # code
    assert lay.resolve_symbol(img.symbols[2]).address == base + lot + 16  # data

    # Relocation semantics verbatim: symbol write, *p += data_base - addend,
    # *p = code_base + old.
    buf, reports = lay.apply_relocations()
    lot_word0 = struct.unpack_from("<I", buf, 0)[0]
    data_off = lay.data_base - base
    word0, word1 = struct.unpack_from("<II", buf, data_off)
    assert lot_word0 == base + lot + 16            # 'var' -> data_base + 0
    assert word0 == (4 + (lay.data_base - 4)) & 0xFFFFFFFF
    assert word1 == (lay.code_base + 0x1234) & 0xFFFFFFFF
    assert [r.written for r in reports] == [lot_word0, word0, word1]
    assert all(r.target in ("LOT", "DATA") for r in reports)


# ─────────────────────────────────────────────────────────────────────────────
# Sectioned images: aligned main-block placement + allocator simulation
# ─────────────────────────────────────────────────────────────────────────────

def _aligned_image():
    """6-entry table: .text@4, .data@16, .bss@8 main; tagged 'slow' DATA@32,
    'fast' BSS@8, 'xipc' CODE@4 in their own regions. Hand-computable geometry."""
    code, data = bytes(range(32)), b"\x11" * 16
    specs = _sectioned_specs(32, 16, 8, main_aligns=(4, 16, 8), tagged=[
        ("slow", 64, 32, p.SEC_CLASS_DATA, p.SEC_FLAG_DMA),
        ("fast", 16, 8, p.SEC_CLASS_BSS, 0),
        ("xipc", 16, 4, p.SEC_CLASS_CODE, 0),
    ])
    raw = _build_image(version=(3, 1),
                       symbols=[("mod", 0, p.SYM_TYPE_MODULE_NAME, False)],
                       sectioned=specs,
                       payloads={0: code, 1: data, 3: b"\x55" * 64, 5: b"\x77" * 16})
    return p.parse_module(raw), code, data


def test_sectioned_main_block_alignment():
    img, code, _data = _aligned_image()
    base = 0x20000000
    lay = img.runtime_layout(p.LoadMode.COPY_ALL, ram_base=base,
                             section_bases={3: 0x30000000, 4: 0x30001000, 5: 0x30002000})
    # Walk: LOT(8) + bases(12) + metadata(224) = 244 -> text@244, data@288
    # (aligned 16), bss@304 (aligned 8); padding included in ram_size = 312.
    assert lay.main_align == 16
    assert lay.code_base == base + 244
    assert lay.data_base == base + 288 and lay.data_base % 16 == 0
    assert lay.bss_base == base + 304 and lay.bss_base % 8 == 0
    assert lay.ram_size == 312
    assert lay.allocation_plan() == [
        p.AllocationCall(312, None, 16, 0),
        p.AllocationCall(64, "slow", 32, p.SEC_FLAG_DMA),
        p.AllocationCall(16, "fast", 8, 0),
        p.AllocationCall(16, "xipc", 4, 0),
    ]
    # A clean placement raises no issues; misaligned/missing/NULL bases do.
    assert lay.placement_issues() == []
    bad = img.runtime_layout(p.LoadMode.COPY_ALL, ram_base=base,
                             section_bases={3: 0x30000004, 5: 0x30002000})
    issues = bad.placement_issues()
    assert any("SECTION_UNALIGNED" in s and "'slow'" in s for s in issues)
    assert any("SECTION_UNRESOLVED" in s and "'fast'" in s for s in issues)
    misaligned_block = img.runtime_layout(p.LoadMode.COPY_ALL, ram_base=base + 8,
                                          section_bases={3: 0x30000000, 4: 0x30001000, 5: 0x30002000})
    assert any("RAM_UNALIGNED" in s for s in misaligned_block.placement_issues())


def test_sectioned_xip_copies_tagged_code_and_skips_main_text():
    img, _code, _data = _aligned_image()
    base = 0x20000000
    lay = img.runtime_layout(p.LoadMode.XIP, ram_base=base,
                             section_bases={3: 0x30000000, 4: 0x30001000, 5: 0x30002000})
    assert lay.code_in_ram is False
    assert lay.code_in_image_offset == img.code_offset   # main text stays in flash
    assert lay.data_base == base + 32 and lay.bss_base == base + 48
    assert lay.ram_size == 56
    # Tagged sections are allocated in every mode — the host asked for them.
    assert [c.section for c in lay.allocation_plan()[1:]] == ["slow", "fast", "xipc"]


def test_sectioned_build_ram_copies_base_array_metadata_and_payloads():
    img, code, data = _aligned_image()
    base = 0x20000000
    lay = img.runtime_layout(p.LoadMode.COPY_ALL, ram_base=base,
                             section_bases={3: 0x30000000, 4: 0x30001000, 5: 0x30002000})
    buf = lay.build_ram()
    assert len(buf) == lay.ram_size
    # Base array right after the LOT, ascending index among non-main sections.
    assert struct.unpack_from("<III", buf, 8) == (0x30000000, 0x30001000, 0x30002000)
    # Metadata block (header .. sectab) follows the array.
    assert bytes(buf[lay.header_base - base:lay.header_base - base + 32]) == img.header.pack()
    sectab_at = lay.header_base - base + img.sectab_offset
    assert bytes(buf[sectab_at:sectab_at + len(img._sectab_bytes)]) == img._sectab_bytes
    # MAIN payloads at their aligned bases.
    assert bytes(buf[lay.code_base - base:lay.code_base - base + 32]) == code
    assert bytes(buf[lay.data_base - base:lay.data_base - base + 16]) == data


def test_simulate_load_drives_allocator_callback():
    img, _code, _data = _aligned_image()
    calls = []

    def host_malloc(size, section, align, flags):
        calls.append((size, section, align, flags))
        if section == "slow":
            return 0x30000044          # deliberately misaligned (needs 32)
        if section is None:
            return 0x20000000
        return 0x30010000

    sim = img.runtime_layout(p.LoadMode.COPY_ALL).simulate_load(host_malloc=host_malloc)
    assert calls == [(len(sim.ram), None, 16, 0),
                     (64, "slow", 32, p.SEC_FLAG_DMA),
                     (16, "fast", 8, 0),
                     (16, "xipc", 4, 0)]
    assert any("SECTION_UNALIGNED" in s and "'slow'" in s for s in sim.issues)
    # The NULL return is the SECTION_UNRESOLVED path.
    sim2 = img.runtime_layout(p.LoadMode.COPY_ALL).simulate_load(
        host_malloc=lambda size, section, align, flags: None)
    # Every call's NULL is recorded (4x), and the placement stays unresolved
    # for each section afterwards.
    assert sum(1 for s in sim2.issues if "returned NULL" in s) == 4
    assert all("SECTION_UNRESOLVED" in s for s in sim2.issues if "returned NULL" not in s)
    # Section blocks: payload copied, BSS zeroed, in every load mode.
    sim3 = img.runtime_layout(p.LoadMode.XIP).simulate_load(
        section_bases={3: 0x30000000, 4: 0x30001000, 5: 0x30002000})
    assert bytes(sim3.section_blocks[3].data) == b"\x55" * 64
    assert bytes(sim3.section_blocks[5].data) == b"\x77" * 16
    assert bytes(sim3.section_blocks[4].data) == b"\x00" * 16
    assert sim3.issues == []



# ─────────────────────────────────────────────────────────────────────────────
# Sectioned images: symbol resolution and relocation semantics
# ─────────────────────────────────────────────────────────────────────────────

def _dma_image(relocs=()):
    specs = _sectioned_specs(32, 16, 0, [("dma", 32, 32, p.SEC_CLASS_DATA, 0)])
    raw = _build_image(version=(3, 1), symbols=_SEC_SYMS, relocs=relocs,
                       sectioned=specs,
                       payloads={0: b"\x00" * 32, 1: b"\x00" * 16, 3: b"\x00" * 32})
    return p.parse_module(raw)


def test_sectioned_symbol_resolves_into_host_base():
    """A symbol living in a tagged section resolves through the host-supplied
    base: runtime(va) = base + (va - section.va)."""
    img = _dma_image()
    lay = img.runtime_layout(p.LoadMode.COPY_ALL, ram_base=0x20000000,
                             section_bases={3: 0x30000000})
    res = lay.resolve_symbol(img.lookup_symbol("dma_buf"))
    assert res.address == 0x30000008 and res.resolved_by == "module"
    assert lay.runtime_address(_TAG_VA + 12) == 0x3000000C


def test_sectioned_reloc_writes_host_base_address():
    """Relocations against a tagged-section symbol write the host-resolved
    address into LOT slots and main .data words alike."""
    img = _dma_image(relocs=[(0, 1), (2, 1)])   # symt index 1 = dma_buf
    lay = img.runtime_layout(p.LoadMode.COPY_ALL, ram_base=0x20000000,
                             section_bases={3: 0x30000000})
    buf, reports = lay.apply_relocations()
    lot0 = struct.unpack_from("<I", buf, 0)[0]
    data0 = struct.unpack_from("<I", buf, lay.data_base - lay.ram_base)[0]
    assert lot0 == data0 == 0x30000008
    assert [r.written for r in reports] == [0x30000008, 0x30000008]


def test_sectioned_reloc_target_inside_tagged_section():
    """Both base-additive forms collapse to *p = runtime(*p); a target word
    inside a tagged section is patched in the section's own block."""
    # Words at dma offsets 12/16 hold link VAs into .text (fn@4, fn2@8).
    dma = bytearray(32)
    struct.pack_into("<II", dma, 12, 4, 8)
    specs = _sectioned_specs(32, 16, 0, [("dma", 32, 32, p.SEC_CLASS_DATA, 0)])
    lot_dma = 2 + (_TAG_VA + 12 - 32) // 4      # target word inside 'dma'
    lot_dma2 = lot_dma + 1
    lot_gap = 2 + (64 - 32) // 4                # VA 64: the gap before 'dma'
    raw = _build_image(version=(3, 1), symbols=_SEC_SYMS,
                       relocs=[(lot_dma, 1 << 31), (lot_dma2, 1 << 30),
                               (lot_gap, (1 << 31) | 4)],
                       sectioned=specs,
                       payloads={0: b"\x00" * 32, 1: b"\x00" * 16, 3: bytes(dma)})
    img = p.parse_module(raw)
    lay = img.runtime_layout(p.LoadMode.COPY_ALL, ram_base=0x20000000,
                             section_bases={3: 0x30000000})
    sim = lay.simulate_load()
    blk = sim.section_blocks[3].data
    # runtime(4) and runtime(8) resolve into main .text via its table entry.
    assert struct.unpack_from("<II", blk, 12) == (lay.code_base + 4, lay.code_base + 8)
    assert sim.reports[0].target == "dma" and sim.reports[0].written == lay.code_base + 4
    assert sim.reports[1].target == "dma" and sim.reports[1].written == lay.code_base + 8
    # The main .data word this image owns was never touched.
    assert struct.unpack_from("<I", sim.ram, lay.data_base - lay.ram_base)[0] == 0
    # A target VA outside every CODE/DATA window (the VA gap) is rejected the
    # same way the loader's BAD_RELOCATION_TABLE path rejects it.
    assert sim.reports[2].kind == "BAD" and "BAD_RELOCATION_TABLE" in sim.reports[2].note
    assert sim.reports[2].written is None


# ─────────────────────────────────────────────────────────────────────────────
# Cross-check against real artifacts (activates once mkmodule --section lands)
# ─────────────────────────────────────────────────────────────────────────────

def _sectioned_sample_bins():
    out = []
    for b in _SAMPLE_BINS:
        with open(b, "rb") as f:
            head = f.read(p.HEADER_SIZE)
        if len(head) < p.HEADER_SIZE:
            continue
        h = p.ModuleHeader.unpack(head, 0)
        if h.signature_ok and h.has_sections:
            out.append(b)
    return out


@pytest.mark.skipif(not _sectioned_sample_bins(),
                    reason="no --section module built yet (toolchain slice pending)")
def test_sectioned_sample_table_matches_elf():
    """The decoded table must equal the ELF-derived expectation: section
    VAs/sizes from readelf, plus the flat-space main-section convention."""
    readelf = shutil.which("arm-none-eabi-readelf")
    if readelf is None:
        pytest.skip("arm-none-eabi-readelf not available")
    for bin_path in _sectioned_sample_bins():
        with open(bin_path, "rb") as f:
            data = f.read()
        img = p.parse_module(data)
        assert img.has_sections
        assert img.to_bytes() == data
        for prev, cur in zip(img.sections, img.sections[1:]):
            assert cur.va >= prev.va + prev.size
        # Main sections follow the flat link space: .text@0, .data@code_size.
        assert (img.sections[0].va, img.sections[0].size) == (0, img.header.code_size)
        assert (img.sections[1].va, img.sections[1].size) == \
            (img.header.code_size, img.header.data_size)
        # Tagged entries must match the ELF's .udynlink.sec.* sections.
        elf = bin_path[:-4] + ".elf"
        out = subprocess.run([readelf, "-SW", elf], capture_output=True, text=True,
                             check=True).stdout
        elf_secs = {}
        for m in re.finditer(
                r"\[\s*\d+\]\s+(\S+)\s+\S+\s+([0-9a-f]{8})\s+[0-9a-f]+\s+([0-9a-f]+)"
                r"\s+\S+\s+\S*\s+\d+\s+\d+\s+(\d+)", out):
            elf_secs[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16), int(m.group(4)))
        for sec in img.sections:
            if sec.is_main:
                # The main sections' ELF counterparts are .text/.data/.bss in
                # the same flat link space.
                elf_name = (".text", ".data", ".bss")[sec.index]
            else:
                elf_name = ".udynlink.sec." + (sec.name or "")
            assert elf_name in elf_secs, "table entry '%s' missing from ELF" % sec.name
            addr, size, elf_align = elf_secs[elf_name]
            assert (sec.va, sec.size) == (addr, size)
            if not sec.is_main:
                # Table align is the host placement promise: mkmodule emits
                # max(declared --section align, ELF sh_addralign), so it may
                # exceed the ELF's. Below it would be an encoder bug.
                assert sec.align >= elf_align
        # End-to-end: symbols whose val lives in a tagged section resolve
        # through the host-supplied base, and their LOT writes carry it.
        bases = {s.index: 0x30000000 + 0x01000000 * i
                 for i, s in enumerate(img.sections) if not s.is_main}
        lay = img.runtime_layout(p.LoadMode.COPY_ALL, ram_base=0x20000000,
                                 section_bases=bases)
        assert lay.placement_issues() == []
        _ram, reports = lay.apply_relocations()
        lot_words = struct.unpack_from("<%dI" % img.header.num_lot, _ram, 0)
        for sym in img.symbols:
            if sym.type in (p.SYM_TYPE_INTERNAL, p.SYM_TYPE_EXPORTED):
                sec = img.find_section_by_va(sym.val_raw)
                if sec is not None and not sec.is_main:
                    expected = bases[sec.index] + (sym.val_raw - sec.va)
                    assert lay.resolve_symbol(sym).address == expected
                    # Its relocation (if any) landed in the LOT with that value.
                    for i, r in enumerate(img.relocations):
                        if r.kind == p.RelocKind.SYMBOL and r.symt_index == sym.index \
                                and r.lot_offset < img.header.num_lot:
                            assert lot_words[r.lot_offset] == expected


# ─────────────────────────────────────────────────────────────────────────────
# Sectioned images: table decode, geometry, round-trip, validation
# ─────────────────────────────────────────────────────────────────────────────

_SEC_SYMS = [
    ("mod", 0, p.SYM_TYPE_MODULE_NAME, False),
    ("dma_buf", _TAG_VA + 8, p.SYM_TYPE_EXPORTED, False),   # lives in 'dma'
]


def _sectioned_image():
    code, data = bytes(range(0x20)), b"\xcd" * 16
    specs = _sectioned_specs(32, 16, 8, [("dma", 32, 32, p.SEC_CLASS_DATA,
                                          p.SEC_FLAG_DMA | p.SEC_FLAG_NOCACHE)])
    raw = _build_image(version=(3, 1), symbols=_SEC_SYMS, sectioned=specs,
                       payloads={0: code, 1: data, 3: b"\x55" * 32})
    return p.parse_module(raw), raw


def test_sectioned_table_decode():
    img, _raw = _sectioned_image()
    h = img.header
    assert h.has_sections and h.flags == p.UDYNLINK_HDR_FLAG_SECTIONS | (4 << p.UDYNLINK_HDR_SECTION_COUNT_SHIFT)
    assert h.num_sections == 4 and h.udynlink_version == p.pack_version(3, 1)
    # Entries-only table after the symtab; the count itself rides in the
    # header, so code_offset is header-only.
    sectab_size = 4 * p.SECTION_ENTRY_SIZE
    assert h.sectab_size == sectab_size
    assert img.sectab_offset == p.align4(h.symtab_offset + h.symt_size)
    assert img.code_offset == p.align4(img.sectab_offset + sectab_size) == h.code_offset
    assert img.data_offset == img.code_offset + 32
    # image_size accounts for the table and every non-BSS payload (BSS skipped).
    assert img.image_size == img.code_offset + 32 + 16 + 32
    # Entries decoded in ascending-VA order with names from the symbol pool.
    assert [(s.name, s.va, s.size, s.align) for s in img.sections] == [
        (".text", 0, 32, 4), (".data", 32, 16, 4),
        (".bss", 48, 8, 4), ("dma", _TAG_VA, 32, 32)]
    assert [s.cls for s in img.sections] == [
        p.SEC_CLASS_CODE, p.SEC_CLASS_DATA, p.SEC_CLASS_BSS, p.SEC_CLASS_DATA]
    assert all(s.is_main for s in img.sections[:3])
    assert not img.sections[3].is_main and not img.sections[3].synthetic
    assert img.sections[3].flags == p.SEC_FLAG_DMA | p.SEC_FLAG_NOCACHE
    assert img.sections[3].host_flags == p.SEC_FLAG_DMA | p.SEC_FLAG_NOCACHE
    assert img.sections[2].is_bss and not img.sections[2].contains_payload


def test_sectioned_payload_order_skips_bss():
    img, _raw = _sectioned_image()
    # Ascending VA order, BSS occupies no payload bytes.
    assert img.payload_image_offset(0) == img.code_offset
    assert img.payload_image_offset(1) == img.code_offset + 32
    assert img.payload_image_offset(2) == img.code_offset + 48   # BSS: next slot
    assert img.payload_image_offset(3) == img.code_offset + 48
    assert img.section_payload(3) == b"\x55" * 32
    assert img.section_payload(2) == b""
    assert img.code == bytes(range(0x20)) and img.data == b"\xcd" * 16


def test_sectioned_round_trip_is_byte_identical():
    _img, raw = _sectioned_image()
    assert p.parse_module(raw).to_bytes() == raw


def test_sectioned_validation_rejects_malformed_tables():
    good = _sectioned_image()[1]
    # The version fence: a sectioned image declaring 3.0 is malformed.
    specs = _sectioned_specs(32, 16, 8, [("dma", 32, 32, p.SEC_CLASS_DATA, 0)])
    with pytest.raises(ValueError, match="3\\.1"):
        p.parse_module(_build_image(version=(3, 0), sectioned=specs, symbols=_SEC_SYMS))
    # Unknown header flag bits (15:8 must be 0).
    bad_flags = bytearray(good)
    struct.pack_into("<H", bad_flags, 14,
                     p.UDYNLINK_HDR_FLAG_SECTIONS | (4 << p.UDYNLINK_HDR_SECTION_COUNT_SHIFT) | 0x0100)
    with pytest.raises(ValueError, match="unknown header flag bits"):
        p.parse_module(bytes(bad_flags))
    # Count/flag consistency lives in the header (the BAD_SECTION_TABLE set).
    count0 = bytearray(good)
    struct.pack_into("<H", count0, 14, p.UDYNLINK_HDR_FLAG_SECTIONS)
    with pytest.raises(ValueError, match="section count 0 out of range"):
        p.parse_module(bytes(count0))
    count64 = bytearray(good)
    struct.pack_into("<H", count64, 14,
                     p.UDYNLINK_HDR_FLAG_SECTIONS | (64 << p.UDYNLINK_HDR_SECTION_COUNT_SHIFT))
    with pytest.raises(ValueError, match="section count 64 out of range"):
        p.parse_module(bytes(count64))
    count_noflag = bytearray(good)
    struct.pack_into("<H", count_noflag, 14, 3 << p.UDYNLINK_HDR_SECTION_COUNT_SHIFT)
    with pytest.raises(ValueError, match="without UDYNLINK_HDR_FLAG_SECTIONS"):
        p.parse_module(bytes(count_noflag))
    # Table entries must be sorted by va (the loader relies on it).
    unsorted = _build_image(version=(3, 1), symbols=_SEC_SYMS, sectioned=[
        (".text", 0, 32, 4, p.SEC_CLASS_CODE, p.SEC_FLAG_MAIN),
        (".data", 64, 16, 4, p.SEC_CLASS_DATA, p.SEC_FLAG_MAIN),   # overlaps .bss slot
        (".bss", 48, 8, 4, p.SEC_CLASS_BSS, p.SEC_FLAG_MAIN),
    ], payloads={0: b"\x00" * 32, 1: b"\x00" * 16})
    with pytest.raises(ValueError, match="sorted by va"):
        p.parse_module(unsorted)
    # align must be a power of two >= 4.
    bad_align = _build_image(version=(3, 1), symbols=_SEC_SYMS, sectioned=[
        (".text", 0, 32, 4, p.SEC_CLASS_CODE, p.SEC_FLAG_MAIN),
        (".data", 32, 16, 3, p.SEC_CLASS_DATA, p.SEC_FLAG_MAIN),
        (".bss", 48, 8, 4, p.SEC_CLASS_BSS, p.SEC_FLAG_MAIN),
    ], payloads={0: b"\x00" * 32, 1: b"\x00" * 16})
    with pytest.raises(ValueError, match="power of two"):
        p.parse_module(bad_align)
    # Reserved flag bits 23:16 must be 0 in v1.
    bad_rsvd = _build_image(version=(3, 1), symbols=_SEC_SYMS, sectioned=[
        (".text", 0, 32, 4, p.SEC_CLASS_CODE, p.SEC_FLAG_MAIN),
        (".data", 32, 16, 4, p.SEC_CLASS_DATA, p.SEC_FLAG_MAIN | 0x00010000),
        (".bss", 48, 8, 4, p.SEC_CLASS_BSS, p.SEC_FLAG_MAIN),
    ], payloads={0: b"\x00" * 32, 1: b"\x00" * 16})
    with pytest.raises(ValueError, match="reserved flag bits"):
        p.parse_module(bad_rsvd)