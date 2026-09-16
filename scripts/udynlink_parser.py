"""Standalone parser for udynlink module images (``UDLM``) and runtime RAM layouts.

This module is the single source of truth, in Python, for the udynlink binary
module format.  Every constant and layout rule below is extracted verbatim from
the two authoritative definitions of the format and kept in sync with them:

* **Writer** — ``scripts/mkmodule`` (``process()``): how a ``.bin`` image is
  encoded (header field order/struct format, relocation entry layout, symbol
  table bit packing, section table, code/data/tagged-payload placement).
* **C reader** — ``udynlink/udynlink.h`` + ``udynlink/udynlink.c``: how the
  loader decodes the same bytes at runtime (``udynlink_module_header_t``,
  ``get_sym_at_raw``, ``udynlink_load_apply_relocations``,
  ``udynlink_image_from_memory``, ``get_code_pointer``/``get_data_pointer``,
  ``get_ram_size_for_header``).

Both encode and decode an identical on-disk layout; this parser reproduces it
losslessly and also models the *runtime* layout (where each section lands in
RAM once a module is loaded under a given load mode, how the extended
allocator places tagged sections, and how relocations patch that RAM).
Loader ABI 3.1 adds the section table (``UDYNLINK_HDR_FLAG_SECTIONS``):
sectioned images resolve through a flat-ELF-VA section map, while untagged
images keep the 3.0 path byte-for-byte (their synthesized three-entry table is
a reporting view only).

The parser is standalone: it depends only on the Python standard library.
``mkmodule`` and the test suite import the format constants from here so the
format is defined exactly once across the Python toolchain.
"""

import struct
from dataclasses import dataclass, field
from enum import IntEnum
from typing import Callable, Dict, List, Optional, Tuple

__all__ = [
    # Signature / version
    "MODULE_SIGNATURE", "MODULE_SIGN_VALUE",
    "pack_version", "get_major_version", "get_minor_version",
    "LOADER_ABI_VERSION",
    # Header flags (bit 0 = section-table flag; bits 7:1 = section count)
    "UDYNLINK_HDR_FLAG_SECTIONS", "UDYNLINK_HDR_SECTION_COUNT_SHIFT",
    "UDYNLINK_HDR_SECTION_COUNT_MASK", "UDYNLINK_HDR_FLAG_RESERVED_MASK",
    "MAX_SECTION_COUNT",
    "HEADER_FORMAT", "HEADER_SIZE", "HEADER_SIZE_V2",
    "ModuleHeader",
    # Symbol table
    "SYM_TYPE_INTERNAL", "SYM_TYPE_EXPORTED", "SYM_TYPE_EXTERN",
    "SYM_TYPE_MODULE_NAME", "SYM_TYPE_WEAK",
    "SYM_NAME_OFFSET_MASK", "SYM_INFO_SHIFT",
    "SYM_INFO_TYPE_MASK", "SYM_INFO_CODE_MASK",
    "SYM_LOCATION_CODE", "SYM_LOCATION_DATA",
    "Symbol", "sym_type_name", "sym_location_name",
    "RELOC_FLAG_DATA_BASE", "RELOC_FLAG_CODE_BASE", "RELOC_VALUE_MASK",
    "Relocation", "RelocKind",
    # Section table
    "SECTION_ENTRY_FORMAT", "SECTION_ENTRY_SIZE",
    "SEC_CLASS_CODE", "SEC_CLASS_DATA", "SEC_CLASS_BSS", "sec_class_name",
    "SEC_FLAG_NOCACHE", "SEC_FLAG_DMA", "SEC_FLAG_SHARED",
    "SEC_FLAG_HOST_MASK", "SEC_FLAG_RESERVED_MASK", "SEC_FLAG_MAIN",
    "TAGGED_REGION_VA_STRIDE", "ImageSection",
    # Architecture tag
    "ARCH_FAMILY_MASK", "ARCH_FPU_MASK", "ARCH_FLOAT_ABI_MASK",
    "ARCH_FLOAT_ABI_SHIFT", "ARCH_FLAG_NO_PROLOGUE",
    "ARCH_TAG_CORTEX_M0", "ARCH_TAG_CORTEX_M0PLUS", "ARCH_TAG_CORTEX_M3",
    "ARCH_TAG_CORTEX_M4", "ARCH_TAG_CORTEX_M4F", "ARCH_TAG_CORTEX_M7",
    "ARCH_TAG_CORTEX_M33", "ARCH_TAG_CORTEX_M55", "ARCH_TAG_CORTEX_M85",
    "arch_family_name", "arch_float_abi_name", "decode_arch_tag",
    # Load mode
    "LoadMode", "LOAD_MODE_COPY_ALL", "LOAD_MODE_COPY_TEXT_DATA", "LOAD_MODE_XIP",
    # Image + runtime
    "ModuleImage", "RuntimeLayout", "ResolvedSymbol", "RelocationReport",
    "AllocationCall", "SectionBlock", "LoadSimulation",
    "parse_module", "align4", "align_up",
]

# ─────────────────────────────────────────────────────────────────────────────
# Alignment helpers
# ─────────────────────────────────────────────────────────────────────────────

def align_up(n: int, alignment: int) -> int:
    """Round ``n`` up to the next multiple of ``alignment`` (power of two)."""
    return (n + alignment - 1) & ~(alignment - 1)


def align4(n: int) -> int:
    """Round ``n`` up to a multiple of 4. Mirrors ``round_to`` in udynlink_utils."""
    return align_up(n, 4)


# ─────────────────────────────────────────────────────────────────────────────
# Signature and version packing
#
# Exact mirror of udynlink.c:
#   #define UDYNLINK_MODULE_SIGN (((uint32_t)'M'<<24)|((uint32_t)'L'<<16)|((uint32_t)'D'<<8)|(uint32_t)'U')
# i.e. the bytes on disk are the ASCII string "UDLM" (little-endian uint32 value
# 0x4D4C4455). mkmodule writes bytearray(b"UDLM").
# ─────────────────────────────────────────────────────────────────────────────

MODULE_SIGNATURE = b"UDLM"
MODULE_SIGN_VALUE = (ord("M") << 24) | (ord("L") << 16) | (ord("D") << 8) | ord("U")

# From udynlink.h: UDYNLINK_MAKE_VERSION(major, minor) = ((major)<<8)|(minor)
def pack_version(major: int, minor: int) -> int:
    return (major << 8) | (minor & 0xFF)


def get_major_version(v: int) -> int:
    return (v >> 8) & 0xFF


def get_minor_version(v: int) -> int:
    return v & 0xFF


# ABI version of the loader shipping with this tree (udynlink.h:
# UDYNLINK_LOADER_ABI_VERSION = UDYNLINK_MAKE_VERSION(3, 1)).  3.1 adds the
# section table (UDYNLINK_HDR_FLAG_SECTIONS); sectioned images must declare
# udynlink_version >= 3.1 so a 3.0 loader fences them via VERSION_MISMATCH,
# while untagged images keep declaring 3.0 and stay loadable by old loaders.
# Used only for the version-comparison helper; the parser reads any
# udynlink_version field.
LOADER_ABI_VERSION = pack_version(3, 1)


# ─────────────────────────────────────────────────────────────────────────────
# Header flags word (the former `reserved` u16 @ 0x0E; no header growth)
#
#   bit 0   UDYNLINK_HDR_FLAG_SECTIONS — image carries a section table
#   bits7:1 num_sections — 1..63 when bit 0 is set, 0 otherwise
#   bits15:8 reserved, must be 0
#
# The count lives here (not in the table) so every size computation stays
# header-only and deref-free: udynlink_get_image_size() doubles as the fuzz
# harness's pre-load gate and must never touch a header-derived offset before
# rejecting the input. Legacy loaders never read this field — the
# udynlink_version fence is what protects them from sectioned images.
# ─────────────────────────────────────────────────────────────────────────────

UDYNLINK_HDR_FLAG_SECTIONS = 0x0001        # bit 0: image carries a section table
UDYNLINK_HDR_SECTION_COUNT_SHIFT = 1
UDYNLINK_HDR_SECTION_COUNT_MASK = 0x00FE   # bits 7:1: num_sections (1..63)
UDYNLINK_HDR_FLAG_RESERVED_MASK = 0xFF00   # bits 15:8 must be 0
MAX_SECTION_COUNT = 63                     # what the 7-bit field may express


# ─────────────────────────────────────────────────────────────────────────────
# Module image header (32 bytes for ABI v1.0 / v3.0+, 36 bytes for v2.0)
#
#   sign(4) mod_version(2) udynlink_version(2) arch_tag(2) num_lot(2)
#   num_rels(2) flags(2) symt_size(4) code_size(4) data_size(4) bss_size(4)

# little-endian: 4s H H H H H H I I I I  ->  4 + 6*2 + 4*4 = 32 bytes
HEADER_FORMAT = "<4sHHHHHHIIII"
HEADER_SIZE = 32          # ABI v1.0 and v3.0+ (sizeof(udynlink_module_header_t))
HEADER_SIZE_V2 = 36       # ABI v2.0 had 4 extra bytes (deprecated dependency list)

@dataclass
class ModuleHeader:
    """Decoded 32-byte UDLM module header."""
    sign: bytes
    mod_version: int
    udynlink_version: int
    arch_tag: int
    num_lot: int
    num_rels: int
    flags: int
    symt_size: int
    code_size: int
    data_size: int
    bss_size: int

    @classmethod
    def unpack(cls, data: bytes, offset: int = 0) -> "ModuleHeader":
        fields = struct.unpack_from(HEADER_FORMAT, data, offset)
        return cls(*fields)

    def pack(self) -> bytes:
        return struct.pack(HEADER_FORMAT, *[
            self.sign, self.mod_version, self.udynlink_version, self.arch_tag,
            self.num_lot, self.num_rels, self.flags, self.symt_size,
            self.code_size, self.data_size, self.bss_size,
        ])

    @property
    def signature_ok(self) -> bool:
        return self.sign == MODULE_SIGNATURE

    @property
    def header_size(self) -> int:
        # Mirrors get_header_size() in udynlink.c: v3.0+ and v1.0 = 32, v2.0 = 36.
        if self.udynlink_version >= pack_version(3, 0):
            return HEADER_SIZE
        if self.udynlink_version >= pack_version(2, 0):
            return HEADER_SIZE_V2
        return HEADER_SIZE

    @property
    def relocs_size(self) -> int:
        """Size of the relocation table in bytes (num_rels * 2 * uint32)."""
        return self.num_rels * 2 * 4

    @property
    def relocs_offset(self) -> int:
        """Byte offset of the relocation table from the image start."""
        return self.header_size

    @property
    def symtab_offset(self) -> int:
        """Byte offset of the symbol table from the image start."""
        return self.relocs_offset + self.relocs_size

    @property
    def num_sections(self) -> int:
        """Section count carried in flags bits 7:1 (0 = no table)."""
        return (self.flags & UDYNLINK_HDR_SECTION_COUNT_MASK) >> UDYNLINK_HDR_SECTION_COUNT_SHIFT

    @property
    def has_sections(self) -> bool:
        """True when the image carries a section table (header flag bit 0)."""
        return bool(self.flags & UDYNLINK_HDR_FLAG_SECTIONS)

    @property
    def sectab_offset(self) -> int:
        """Byte offset of the section table from the image start.

        Sits at align4(symtab_offset + symt_size), contiguous with the header
        metadata (the existing COPY_TEXT_DATA/XIP constraint). For untagged
        images (count 0) this offset is where the .code payload starts.
        """
        return align4(self.symtab_offset + self.symt_size)

    @property
    def sectab_size(self) -> int:
        """Bytes of section-table entries — no count word, the count is here."""
        return self.num_sections * SECTION_ENTRY_SIZE

    def _require_untagged(self, what: str) -> None:
        # The header cannot know the section *sizes* (they live in the table),
        # so geometry that sums payloads is undefined without parsing. Fail
        # loudly instead of returning a plausible-but-wrong number: the parsed
        # ModuleImage carries the authoritative values.
        if self.has_sections:
            raise ValueError(
                "%s depends on the section sizes; use the parsed ModuleImage "
                "geometry for sectioned images" % what)

    @property
    def code_offset(self) -> int:
        """Byte offset of the .code section from the image start.

        Mirrors get_code_offset_from_header(): header + relocs + symt (+ the
        section table when present), aligned up to 4. Header-only for both
        kinds because the section count travels in the flags word.
        """
        return align4(self.sectab_offset + self.sectab_size)

    @property
    def metadata_size(self) -> int:
        """Bytes from image start to .code start (header + relocs + symt
        (+ sectab) + pad). Mirrors udynlink_get_image_metadata_size()."""
        return self.code_offset

    @property
    def data_offset(self) -> int:
        """Byte offset of the .data section from the image start.

        Payloads are stored in ascending-VA order skipping BSS, so .data
        follows .code in the image for tagged modules too (contract §1.3).
        """
        return self.code_offset + self.code_size


    @property
    def image_size(self) -> int:
        """Total on-disk image size (header + relocs + symt + code + data).

        Mirrors udynlink_get_image_size() (bss is not stored on disk).
        Untagged images only; sectioned images also store the section table
        and any tagged-section payloads.
        """
        self._require_untagged("header.image_size")
        return self.data_offset + self.data_size

    def ram_size(self, mode: "LoadMode") -> int:
        """RAM bytes required to load under ``mode`` (untagged images).

        Mirrors get_ram_size_for_header() in udynlink.c. Untagged images only:
        sectioned main blocks grow by the non-main base array and per-section
        alignment padding — use RuntimeLayout.ram_size for those.
        """
        self._require_untagged("header.ram_size")
        tot = self.num_lot * 4 + self.data_size + self.bss_size
        if mode == LoadMode.COPY_TEXT_DATA:
            tot += self.code_size
        elif mode == LoadMode.COPY_ALL:
            tot += self.code_offset + self.code_size
        return tot


# ─────────────────────────────────────────────────────────────────────────────
# Symbol table bit layout
#
# Each symbol entry is two little-endian uint32 words: (name_off, val).
#   name_off bits [26:0]  = byte offset of the NUL-terminated name within the
#                           symbol table (0 for nameless INTERNAL symbols)
#                [29:27]  = symbol type (SYM_TYPE_*)
#                [30]     = location: 1 = .code, 0 = .data
#                [31]     = last-entry marker (0xFFFFFFFF); written by legacy
#                           toolchains only — never emitted by current mkmodule
# Exact mirror of udynlink.c masks UDYNLINK_SYM_OFFSET_MASK (0x07FFFFFF),
# UDYNLINK_SYM_INFO_SHIFT (27), UDYNLINK_SYM_INFO_TYPE_MASK (0x07),
# UDYNLINK_SYM_INFO_CODE_MASK (0x08). The encoder in mkmodule uses the same
# packing: s_off = name_off | (type_data << 27), type_data = type | (8 if code).
# ─────────────────────────────────────────────────────────────────────────────

SYM_TYPE_INTERNAL    = 0
SYM_TYPE_EXPORTED    = 1
SYM_TYPE_EXTERN      = 2
SYM_TYPE_MODULE_NAME = 3
SYM_TYPE_WEAK        = 4

SYM_NAME_OFFSET_MASK = 0x07FFFFFF
SYM_INFO_SHIFT       = 27
SYM_INFO_TYPE_MASK   = 0x07
SYM_INFO_CODE_MASK   = 0x08   # bit 3 of the info byte (= bit 30 of name_off)

SYM_LOCATION_CODE    = 0
SYM_LOCATION_DATA    = 1

_SYM_TYPE_NAMES = {
    SYM_TYPE_INTERNAL:    "INTERNAL",
    SYM_TYPE_EXPORTED:    "EXPORTED",
    SYM_TYPE_EXTERN:      "EXTERN",
    SYM_TYPE_MODULE_NAME: "MODULE_NAME",
    SYM_TYPE_WEAK:        "WEAK",
}
_SYM_LOC_NAMES = {SYM_LOCATION_CODE: "CODE", SYM_LOCATION_DATA: "DATA"}


def sym_type_name(t: int) -> str:
    return _SYM_TYPE_NAMES.get(t, "TYPE_%d" % t)


def sym_location_name(loc: int) -> str:
    return _SYM_LOC_NAMES.get(loc, "LOC_%d" % loc)


@dataclass
class Symbol:
    """One decoded symbol table entry.

    ``name_off_raw`` / ``val_raw`` preserve the on-disk words losslessly so the
    image can be re-serialized byte-for-byte; ``name`` / ``type`` / ``location``
    are the decoded fields mirroring udynlink.c's udynlink_sym_t.
    """
    index: int
    name_off_raw: int
    val_raw: int
    name: Optional[str]   # None for INTERNAL (nameless) symbols
    type: int
    location: int        # SYM_LOCATION_CODE / SYM_LOCATION_DATA

    @property
    def info(self) -> int:
        """The 5-bit info byte (type | code-flag), as udynlink.c computes it."""
        return (self.name_off_raw >> SYM_INFO_SHIFT)

    @property
    def name_byte_offset(self) -> int:
        return self.name_off_raw & SYM_NAME_OFFSET_MASK

    @property
    def is_last_entry_marker(self) -> bool:
        """True for the legacy 0xFFFFFFFF sentinel (all bits set)."""
        return self.name_off_raw == 0xFFFFFFFF

    @property
    def type_name(self) -> str:
        return sym_type_name(self.type)

    @property
    def location_name(self) -> str:
        return sym_location_name(self.location)

    @property
    def is_local(self) -> bool:
        return self.type == SYM_TYPE_INTERNAL

    @property
    def is_external(self) -> bool:
        return self.type == SYM_TYPE_EXTERN

    @property
    def is_in_code(self) -> bool:
        return self.location == SYM_LOCATION_CODE

    def pack(self) -> bytes:
        return struct.pack("<II", self.name_off_raw, self.val_raw)


# ─────────────────────────────────────────────────────────────────────────────
# Relocation table bit layout
#
# Each relocation entry is two little-endian uint32 words: (lot_offset, value).
# ``lot_offset`` indexes into the runtime LOT (offset < num_lot) or into the
# post-LOT .data section (offset >= num_lot, in uint32 units).
# ``value`` (called symt_offset in udynlink.c) is one of:
#   * bit 31 set  -> R_ARM_ABS32 data relocation (additive). The low 31 bits
#                    are the .data addend; at load: *p += data_base - addend.
#   * bit 30 set  -> R_ARM_TARGET1 / code-base relocation. At load:
#                    *p = code_base + *p  (i.e. patches a .code pointer).
#   * otherwise   -> a symbol-table index; the symbol's resolved runtime
#                    address is written to *p. EXTERN symbols need a host
#                    resolver; failure is fatal at load time.
# Exact mirror of udynlink_load_apply_relocations() in udynlink.c and the
# mkmodule pack loop (value = (1<<30)|v for .text, (1<<31)|v for .data,
# symt_mapping[key] otherwise).
# ─────────────────────────────────────────────────────────────────────────────

RELOC_FLAG_DATA_BASE = 1 << 31   # R_ARM_ABS32: low 31 bits = data addend
RELOC_FLAG_CODE_BASE = 1 << 30   # R_ARM_TARGET1/code-base: p = code_base + old
RELOC_VALUE_MASK     = 0x7FFFFFFF


class RelocKind(IntEnum):
    """Classification of a relocation's value word (the loader's dispatch)."""
    SYMBOL   = 0   # value is a symbol-table index
    DATA_BASE = 1   # R_ARM_ABS32 data relocation (additive)
    CODE_BASE = 2   # R_ARM_TARGET1 / code-base relocation


@dataclass
class Relocation:
    """One decoded relocation entry (lot_offset, value)."""
    lot_offset: int
    value: int

    @property
    def kind(self) -> RelocKind:
        if self.value & RELOC_FLAG_DATA_BASE:
            return RelocKind.DATA_BASE
        if self.value & RELOC_FLAG_CODE_BASE:
            return RelocKind.CODE_BASE
        return RelocKind.SYMBOL

    @property
    def symt_index(self) -> int:
        """Symbol-table index when ``kind == SYMBOL``; else 0."""
        return self.value if self.kind == RelocKind.SYMBOL else 0

    @property
    def data_addend(self) -> int:
        """Low 31 bits of value for R_ARM_ABS32 (DATA_BASE) relocations."""
        return self.value & RELOC_VALUE_MASK

    @property
    def target_is_lot(self) -> bool:
        """True if this relocation patches a LOT slot (vs. a .data word)."""
        # The loader distinguishes by lot_offset < num_lot; that needs the
        # header, so this is a convenience for the SYMBOL/DATA_BASE/CODE_BASE
        # case handled in RuntimeLayout.apply_relocations.
        return False  # resolved in context by RuntimeLayout

    def pack(self) -> bytes:
        return struct.pack("<II", self.lot_offset, self.value)


# ─────────────────────────────────────────────────────────────────────────────
# Architecture tag
#
# uint16_t bit layout (udynlink.h):
#   bits [3:0]  — core family ID
#   bit  4      — FPU present
#   bits [6:5]  — float ABI (00=soft, 01=softfp, 10=hard)
#   bit  7      — UDYNLINK_ARCH_FLAG_NO_PROLOGUE (set by mkmodule --no-prologue)
#   bits [15:8] — reserved
# ─────────────────────────────────────────────────────────────────────────────

ARCH_FAMILY_MASK        = 0x0F
ARCH_FPU_MASK            = 0x10
ARCH_FLOAT_ABI_MASK      = 0x60
ARCH_FLOAT_ABI_SHIFT     = 5
ARCH_FLAG_NO_PROLOGUE    = 0x80

ARCH_FLOAT_ABI_SOFT      = 0
ARCH_FLOAT_ABI_SOFTFP    = 1
ARCH_FLOAT_ABI_HARD      = 2

ARCH_TAG_CORTEX_M0      = 0x01
ARCH_TAG_CORTEX_M0PLUS  = 0x02
ARCH_TAG_CORTEX_M3      = 0x03
ARCH_TAG_CORTEX_M4      = 0x04
ARCH_TAG_CORTEX_M4F     = 0x54
ARCH_TAG_CORTEX_M7      = 0x57
ARCH_TAG_CORTEX_M33     = 0x08
ARCH_TAG_CORTEX_M55     = 0x59
ARCH_TAG_CORTEX_M85     = 0x5A

_ARCH_FAMILY_NAMES = {
    0x01: "cortex-m0", 0x02: "cortex-m0plus", 0x03: "cortex-m3",
    0x04: "cortex-m4", 0x08: "cortex-m33",
    0x05: "cortex-m7", 0x09: "cortex-m55", 0x0A: "cortex-m85",
}
_FLOAT_ABI_NAMES = {
    ARCH_FLOAT_ABI_SOFT: "soft", ARCH_FLOAT_ABI_SOFTFP: "softfp",
    ARCH_FLOAT_ABI_HARD: "hard",
}


def arch_family_name(arch_tag: int) -> str:
    return _ARCH_FAMILY_NAMES.get(arch_tag & ARCH_FAMILY_MASK, "family-0x%X" % (arch_tag & ARCH_FAMILY_MASK))


def arch_float_abi_name(arch_tag: int) -> Optional[str]:
    abi = (arch_tag & ARCH_FLOAT_ABI_MASK) >> ARCH_FLOAT_ABI_SHIFT
    return _FLOAT_ABI_NAMES.get(abi)


def decode_arch_tag(arch_tag: int) -> dict:
    """Decode an architecture tag into its component fields."""
    return {
        "family": arch_family_name(arch_tag),
        "family_id": arch_tag & ARCH_FAMILY_MASK,
        "fpu": bool(arch_tag & ARCH_FPU_MASK),
        "float_abi": arch_float_abi_name(arch_tag),
        "no_prologue": bool(arch_tag & ARCH_FLAG_NO_PROLOGUE),
        "raw": arch_tag,
    }


# ─────────────────────────────────────────────────────────────────────────────
# Load mode (udynlink.h udynlink_load_mode_t)
# ─────────────────────────────────────────────────────────────────────────────

class LoadMode(IntEnum):
    COPY_ALL       = 0
    COPY_TEXT_DATA = 1
    XIP            = 2


# Integer aliases matching the C enum values, for code that prefers ints.
LOAD_MODE_COPY_ALL       = int(LoadMode.COPY_ALL)
LOAD_MODE_COPY_TEXT_DATA = int(LoadMode.COPY_TEXT_DATA)
LOAD_MODE_XIP            = int(LoadMode.XIP)

# ─────────────────────────────────────────────────────────────────────────────
# Section table (loader ABI 3.1, header flag UDYNLINK_HDR_FLAG_SECTIONS)
#
# Present only when the header flag is set, at header.sectab_offset,
# contiguous with the header metadata. The count lives in the header's flags
# bits 7:1 (§1.1) — the table itself is entries only:
#
#   entry[num_sections]:            /* 24 bytes each, sorted by va ascending */
#       u32 name_off;   /* NUL-terminated name inside the symbol-table string
#                          pool (symt covers it); 0 = unnamed */
#       u32 va;         /* link-time VA of the section start (flat ELF link
#                          address space: .text=0, .data=code_size,
#                          .bss=code_size+data_size, tagged=0x02000000*(k+1)) */
#       u32 size;       /* bytes, multiple of 4 */
#       u32 align;      /* bytes, power of two, >= 4 */
#       u32 class;      /* 0 = CODE, 1 = DATA, 2 = BSS */
#       u32 flags;      /* hint bits below; bit 31 = MAIN (loader-internal) */
#
# The three main sections (.text/.data/.bss) always hold the three lowest VAs,
# so they are indices 0/1/2 for tagged and untagged modules alike. Untagged
# modules have no table on disk; the parser synthesizes the same three entries
# as a *reporting view* only — untagged resolution keeps today's
# SYM_INFO_CODE_MASK branch (data/bss values are arena-relative there, so the
# ranges overlap and a VA lookup would be ambiguous).
# ─────────────────────────────────────────────────────────────────────────────

SECTION_ENTRY_FORMAT = "<6I"
SECTION_ENTRY_SIZE = 24

SEC_CLASS_CODE = 0     # UDYNLINK_SEC_CLASS_CODE
SEC_CLASS_DATA = 1     # UDYNLINK_SEC_CLASS_DATA
SEC_CLASS_BSS  = 2     # UDYNLINK_SEC_CLASS_BSS

_SEC_CLASS_NAMES = {SEC_CLASS_CODE: "CODE", SEC_CLASS_DATA: "DATA", SEC_CLASS_BSS: "BSS"}

# Hint flags (contract §1.4): bits 7:0 udynlink vocabulary, 15:8 host
# pass-through (never interpreted), 23:16 reserved (must be 0), 31:24 internal
# (bit 31 MAIN: the section lives in the module's main RAM block).
SEC_FLAG_NOCACHE = 0x01          # host should map non-cacheable (DMA coherency)
SEC_FLAG_DMA     = 0x02          # must be reachable by the DMA controller
SEC_FLAG_SHARED  = 0x04          # may be shared with other modules / host code
SEC_FLAG_HOST_MASK      = 0x0000FF00   # opaque pass-through
SEC_FLAG_RESERVED_MASK  = 0x00FF0000   # must be 0 in v1
SEC_FLAG_MAIN           = 1 << 31      # loader-internal: main RAM block

# Tagged regions start here in the flat link space (mkmodule linker template),
# spaced > 16 MiB apart so an accidental cross-region `bl` fails to link.
TAGGED_REGION_VA_STRIDE = 0x02000000


def sec_class_name(cls: int) -> str:
    return _SEC_CLASS_NAMES.get(cls, "CLASS_%d" % cls)


@dataclass
class ImageSection:
    """One decoded (or synthesized) section-table entry.

    Mirrors the C ``udynlink_section_info_t`` reporting view plus the raw
    on-disk words needed to re-serialize the table byte-for-byte.
    """
    index: int              # position in the table (sorted by va)
    name_off: int           # raw u32; 0 = unnamed
    va: int                 # link-time VA of the section start
    size: int               # bytes, multiple of 4
    align: int              # bytes, power of two, >= 4
    cls: int                # SEC_CLASS_*
    flags: int              # SEC_FLAG_*; bit 31 = MAIN
    name: Optional[str] = None
    synthetic: bool = False  # True for the untagged reporting view

    @property
    def is_main(self) -> bool:
        return bool(self.flags & SEC_FLAG_MAIN)

    @property
    def is_bss(self) -> bool:
        return self.cls == SEC_CLASS_BSS

    @property
    def class_name(self) -> str:
        return sec_class_name(self.cls)

    @property
    def host_flags(self) -> int:
        """Flags as the allocator callback sees them (MAIN + reserved masked)."""
        return self.flags & ~SEC_FLAG_MAIN & ~SEC_FLAG_RESERVED_MASK

    @property
    def contains_payload(self) -> bool:
        """BSS occupies no payload bytes in the image (zeroed at load)."""
        return not self.is_bss

    def contains_va(self, va: int) -> bool:
        return self.va <= va < self.va + self.size

    def pack(self) -> bytes:
        return struct.pack(SECTION_ENTRY_FORMAT,
                           self.name_off, self.va, self.size,
                           self.align, self.cls, self.flags)

# ─────────────────────────────────────────────────────────────────────────────
# Parsed module image (on-disk layout)
# ─────────────────────────────────────────────────────────────────────────────

@dataclass
class ModuleImage:
    """A fully decoded UDLM module image.

    ``parse_module`` populates this; every byte of the source blob is accounted
    for by ``image_size``. For untagged images ``sections`` holds the
    synthesized three-entry reporting view (resolution never uses it); for
    sectioned images it is the decoded on-disk table.
    """
    header: ModuleHeader
    relocations: List[Relocation]
    symbols: List[Symbol]
    code: bytes
    data: bytes
    # Absolute (relative to image start) byte offsets, for tooling/inspection.
    relocs_offset: int = 0
    symtab_offset: int = 0
    code_offset: int = 0
    data_offset: int = 0
    # Section table: decoded entries (sorted by va) + raw bytes + offset.
    # Untagged images carry the synthesized 3-entry view and empty bytes.
    sections: List[ImageSection] = field(default_factory=list)
    payloads: List[bytes] = field(default_factory=list)  # per section; BSS -> b""
    sectab_offset: int = 0

    @property
    def module_name(self) -> Optional[str]:
        """Index-0 entry is always the module name (SYM_TYPE_MODULE_NAME)."""
        return self.symbols[0].name if self.symbols else None

    @property
    def num_named_symbols(self) -> int:
        """Count of named (searchable) entries [1, N], as the loader computes.

        Mirrors compute_num_named_syms_raw(): named entries occupy indices
        1..N lexicographically sorted, locals follow. Returns 0 (meaning
        "fall back to linear search") if the named block is not sorted.
        """
        if len(self.symbols) <= 1:
            return 0
        last_named = 0
        for i in range(1, len(self.symbols)):
            s = self.symbols[i]
            if s.type == SYM_TYPE_INTERNAL:
                break
            last_named = i
        # Verify lexicographic ordering of the named block.
        named = [self.symbols[i] for i in range(1, last_named + 1) if self.symbols[i].is_last_entry_marker is False]
        named_names = [(s.name or "") for s in named]
        if any(named_names[i] > named_names[i + 1] for i in range(len(named_names) - 1)):
            return 0
        return last_named

    def lookup_symbol(self, name: str) -> Optional[Symbol]:
        """Linear search for a named symbol (INTERNAL excluded).

        The runtime uses binary search over [1, num_named_symbols]; this helper
        is a convenience for offline inspection and does not require the named
        block to be sorted.
        """
        for s in self.symbols:
            if not s.is_local and s.name == name:
                return s
        return None

    def runtime_layout(self, mode: LoadMode, ram_base: int = 0,
                       section_bases: Optional[dict] = None) -> "RuntimeLayout":
        """Build a RuntimeLayout; ``section_bases`` maps non-main section
        index -> host-resolved runtime address (tagged images only)."""
        return RuntimeLayout(self, mode, ram_base, section_bases)

    @property
    def has_sections(self) -> bool:
        return self.header.has_sections

    @property
    def image_size(self) -> int:
        """Total on-disk image size; the authoritative geometry for both kinds.

        Sectioned images append the section table to the metadata and store
        every non-BSS payload in ascending-VA order (contract §1.3), so
        ``data_offset == code_offset + code_size`` still holds.
        """
        return self.code_offset + sum(s.size for s in self.sections if s.contains_payload)

    def find_section_by_va(self, va: int) -> Optional[ImageSection]:
        """The section whose [va, va+size) window contains ``va``.

        Sectioned images only: the flat ELF link space keeps the ranges
        non-overlapping, so the answer is unique. The synthesized untagged
        view has overlapping code/data ranges (arena-relative values) and must
        not be queried — untagged resolution uses the SYM_INFO_CODE_MASK bit.
        """
        if not self.has_sections:
            return None
        for s in self.sections:  # N is small; the loader may linear-scan too
            if s.contains_va(va):
                return s
        return None

    def section_payload(self, idx: int) -> bytes:
        """Payload bytes of section ``idx`` as stored in the image (BSS -> b"")."""
        return self.payloads[idx]

    def to_bytes(self) -> bytes:
        """Re-serialize the image byte-for-byte from its decoded parts.

        Lossless round-trip: ``parse_module(img).to_bytes() == img`` for any
        well-formed image. The symbol table, relocation table and section
        table are rebuilt from their raw on-disk words; the name pool and all
        payloads are carried verbatim and re-placed by the same ascending-VA
        offset rule the parser decoded them with.
        """
        out = bytearray(self.header.pack())
        for r in self.relocations:
            out += r.pack()
        # Symtab: we rebuild from the decoded raw entries + the name pool that
        # followed them. The name pool length is symtab_offset-offset_of_names.
        out += self._symtab_bytes
        # Pad to the section table (none for untagged: the empty table makes
        # this a no-op), append it, then pad to the payload start.
        out += b"\x00" * (self.sectab_offset - len(out))
        out += self._sectab_bytes
        out += b"\x00" * (self.code_offset - len(out))
        buf = bytearray(self.image_size)
        buf[:len(out)] = out
        for sec, payload in zip(self.sections, self.payloads):
            if not sec.contains_payload:
                continue
            off = self.payload_image_offset(sec.index)
            buf[off:off + len(payload)] = payload
        return bytes(buf)

    def payload_image_offset(self, idx: int) -> int:
        """On-disk offset of section ``idx``'s payload (contract §1.3).

        Payloads are concatenated in ascending VA order skipping BSS:
        ``img_off[k] = code_offset + sum(size[j] for j < k if not BSS)``.
        """
        off = self.code_offset
        for s in self.sections:
            if s.index >= idx:
                break
            if s.contains_payload:
                off += s.size
        return off

    # populated by parse_module
    _symtab_bytes: bytes = b""
    _sectab_bytes: bytes = b""



def align4_at_end(buf: bytearray) -> bytearray:
    """Pad ``buf`` with NULs to a multiple of 4 bytes (mkmodule's tail pad)."""
    pad = (4 - (len(buf) % 4)) % 4
    if pad:
        buf += b"\x00" * pad
    return buf


# ─────────────────────────────────────────────────────────────────────────────
# Image parsing
# ─────────────────────────────────────────────────────────────────────────────

def _pool_name(symtab_bytes: bytes, name_off: int, symt_size: int) -> Optional[str]:
    """Decode a NUL-terminated name at ``name_off`` in the symbol-table string
    pool. Offsets are symt-relative (the count word and entries prefix the
    pool), mirroring get_sym_at_raw; out-of-range offsets decode as None."""
    if name_off >= symt_size:
        return None
    end = symtab_bytes.find(b"\x00", name_off)
    if end < 0:
        end = symt_size
    return symtab_bytes[name_off:end].decode("utf-8", errors="replace")


def _synthesized_sections(header: ModuleHeader) -> List[ImageSection]:
    """The untagged reporting view (contract §1.5): three implicit MAIN
    sections with today's exact bases and align=4. Resolution never consults
    these — untagged data/bss values are arena-relative, the ranges overlap,
    and today's SYM_INFO_CODE_MASK branch stays the only disambiguator."""
    return [
        ImageSection(0, 0, 0, header.code_size, 4, SEC_CLASS_CODE, SEC_FLAG_MAIN, None, True),
        ImageSection(1, 0, header.code_size, header.data_size, 4, SEC_CLASS_DATA, SEC_FLAG_MAIN, None, True),
        ImageSection(2, 0, header.code_size + header.data_size, header.bss_size, 4,
                     SEC_CLASS_BSS, SEC_FLAG_MAIN, None, True),
    ]


def _parse_section_table(data: bytes, header: ModuleHeader, symtab_bytes: bytes,
                         sectab_off: int, validate: bool
                         ) -> Tuple[List[ImageSection], bytes]:
    """Decode the section table at ``sectab_off`` (flag-gated; contract §1.2).

    Returns the entries (sorted by va) and the raw table bytes for
    re-serialization. Bounds-checked like the loader: every malformed or
    inconsistent table (truncation, cap overrun, size%4, bad align/class,
    reserved flag bits, name_off outside the symtab blob, unsorted/overlapping
    VAs) raises under ``validate`` — the same set the loader rejects with
    UDYNLINK_ERR_LOAD_BAD_SECTION_TABLE (index 17). The extent caps
    (UDYNLINK_MAX_IMAGE_SIZE / MAX_RAM_SIZE) stay loader-side, like today's
    untagged path. With ``validate=False`` a truncated table stops early.
    """
    num_sections = header.num_sections   # carried in the header, not the table
    if validate and sectab_off + num_sections * SECTION_ENTRY_SIZE > len(data):
        raise ValueError("buffer truncated: section table needs %d bytes at %d, got %d"
                         % (num_sections * SECTION_ENTRY_SIZE, sectab_off, len(data)))
    entries: List[ImageSection] = []
    raw = bytearray()   # the table is entries only; the count lives in the header
    for i in range(num_sections):
        off = sectab_off + i * SECTION_ENTRY_SIZE
        if off + SECTION_ENTRY_SIZE > len(data):
            break  # truncated table; stop like the loader's bounds check
        fields = struct.unpack_from(SECTION_ENTRY_FORMAT, data, off)
        raw += struct.pack(SECTION_ENTRY_FORMAT, *fields)
        name_off, va, size, align, cls, flags = fields
        name = None
        if name_off != 0:  # 0 = unnamed
            name = _pool_name(symtab_bytes, name_off, header.symt_size)
            if validate and name is None:
                raise ValueError("section %d name offset %d outside the symbol table (%d bytes)"
                                 % (i, name_off, header.symt_size))
        if validate:
            if size % 4 != 0:
                raise ValueError("section %d ('%s') size %d not a multiple of 4" % (i, name, size))
            if align < 4 or (align & (align - 1)) != 0:
                raise ValueError("section %d ('%s') align %d not a power of two >= 4" % (i, name, align))
            if cls > SEC_CLASS_BSS:
                raise ValueError("section %d ('%s') unknown class %d" % (i, name, cls))
            if flags & SEC_FLAG_RESERVED_MASK:
                raise ValueError("section %d ('%s') reserved flag bits 0x%X must be 0 in v1"
                                 % (i, name, flags & SEC_FLAG_RESERVED_MASK))
        entries.append(ImageSection(i, name_off, va, size, align, cls, flags, name))
    if validate:
        for prev, cur in zip(entries, entries[1:]):
            if cur.va < prev.va + prev.size:
                raise ValueError("section table not sorted by va / ranges overlap: "
                                 "[%s @0x%X+0x%X] vs [%s @0x%X+0x%X]"
                                 % (prev.name, prev.va, prev.size, cur.name, cur.va, cur.size))
    return entries, bytes(raw)


def parse_module(data: bytes, validate: bool = True) -> ModuleImage:
    """Parse a UDLM module image blob into a :class:`ModuleImage`.

    This mirrors the reader side of the format exactly: it locates the header,
    relocation table, symbol table, section table (when the header flag is
    set), and the section payloads by the same offsets the C loader uses
    (udynlink_image_from_memory / get_sym_at_raw), and decodes each
    symbol/relocation/section entry with the on-disk bit layout.

    Set ``validate=False`` to skip the signature/size sanity checks (useful for
    probing truncated/legacy blobs).
    """
    if len(data) < HEADER_SIZE:
        raise ValueError("buffer too short for a UDLM header (got %d bytes)" % len(data))

    header = ModuleHeader.unpack(data, 0)
    if validate:
        if not header.signature_ok:
            raise ValueError("bad signature: %r (expected %r)" % (header.sign, MODULE_SIGNATURE))
        if header.header_size != HEADER_SIZE:
            raise ValueError("unsupported header size %d (v2.0 images not supported by this parser)"
                             % header.header_size)
        if header.flags & UDYNLINK_HDR_FLAG_RESERVED_MASK:
            raise ValueError("unknown header flag bits 0x%X (bits 15:8 must be 0)"
                             % (header.flags & UDYNLINK_HDR_FLAG_RESERVED_MASK))
        # Count/flag consistency (BAD_SECTION_TABLE set): the count rides in
        # flags bits 7:1, so an impossible encoding is visible header-only.
        if header.has_sections:
            if not 1 <= header.num_sections <= MAX_SECTION_COUNT:
                raise ValueError("section count %d out of range 1..%d"
                                 % (header.num_sections, MAX_SECTION_COUNT))
        elif header.num_sections:
            raise ValueError("section count %d set without UDYNLINK_HDR_FLAG_SECTIONS"
                             % header.num_sections)
        if header.has_sections and header.udynlink_version < pack_version(3, 1):
            # The version fence: a 3.0 loader rejects sectioned images with
            # VERSION_MISMATCH, so a sectioned image declaring <= 3.0 is a
            # malformed artifact, not a legacy one.
            raise ValueError("sectioned image must declare udynlink_version >= 3.1 (got %d.%d)"
                             % (get_major_version(header.udynlink_version),
                                get_minor_version(header.udynlink_version)))

    relocs_off = header.relocs_offset
    symtab_off = header.symtab_offset
    if validate:
        # Bound the metadata region before decoding it, so a truncated image
        # fails with a clear error instead of a struct.error mid-table.
        need = symtab_off + header.symt_size + header.sectab_size
        if len(data) < need:
            raise ValueError("buffer truncated: need %d bytes, got %d" % (need, len(data)))

    # Relocations
    relocations: List[Relocation] = []
    for i in range(header.num_rels):
        lot_off, value = struct.unpack_from("<II", data, relocs_off + i * 8)
        relocations.append(Relocation(lot_off, value))

    # Symbol table: [count:u32][count * (name_off:u32, val:u32)][name pool]
    symtab_bytes = data[symtab_off:symtab_off + header.symt_size]
    num_entries = struct.unpack_from("<I", symtab_bytes, 0)[0] if header.symt_size >= 4 else 0
    symbols: List[Symbol] = []
    for i in range(num_entries):
        base = 4 + i * 8
        if base + 8 > header.symt_size:
            break  # truncated symtab; stop like the loader's bounds check
        name_off_raw, val_raw = struct.unpack_from("<II", symtab_bytes, base)
        info = name_off_raw >> SYM_INFO_SHIFT
        sym_type = info & SYM_INFO_TYPE_MASK
        location = SYM_LOCATION_CODE if (info & SYM_INFO_CODE_MASK) else SYM_LOCATION_DATA
        if sym_type == SYM_TYPE_INTERNAL or name_off_raw == 0xFFFFFFFF:
            name = None
        else:
            name = _pool_name(symtab_bytes, name_off_raw & SYM_NAME_OFFSET_MASK, header.symt_size)
        symbols.append(Symbol(i, name_off_raw, val_raw, name, sym_type, location))

    # Section table + payload geometry. The count travels in the header, so
    # code_offset = align4(sectab_offset + sectab_size) is header-only for both
    # kinds; untagged (count 0) reduces to today's align4(symtab_offset+symt).
    code_off = header.code_offset
    if header.has_sections:
        sections, sectab_bytes = _parse_section_table(data, header, symtab_bytes,
                                                      header.sectab_offset, validate)
    else:
        sections, sectab_bytes = _synthesized_sections(header), b""
    data_off = code_off + header.code_size

    # Payloads are concatenated in ascending VA order skipping BSS
    # (contract §1.3): img_off[k] = code_offset + sum(size[j] for j < k, not BSS).
    # For untagged images this places [text][data] exactly like today.
    payloads: List[bytes] = []
    off = code_off
    for sec in sections:
        if sec.is_bss:
            payloads.append(b"")
            continue
        payloads.append(data[off:off + sec.size])
        off += sec.size
    code = payloads[0] if payloads else b""
    data_section = payloads[1] if len(payloads) > 1 else b""

    if validate:
        img_size = code_off + sum(s.size for s in sections if s.contains_payload)
        if len(data) < img_size:
            raise ValueError("buffer truncated: image needs %d bytes, got %d" % (img_size, len(data)))

    img = ModuleImage(
        header=header,
        relocations=relocations,
        symbols=symbols,
        code=code,
        data=data_section,
        relocs_offset=relocs_off,
        symtab_offset=symtab_off,
        code_offset=code_off,
        data_offset=data_off,
        sections=sections,
        payloads=payloads,
        sectab_offset=header.sectab_offset if header.has_sections else 0,
    )
    img._symtab_bytes = symtab_bytes  # type: ignore[attr-defined]
    img._sectab_bytes = sectab_bytes  # type: ignore[attr-defined]
    return img


# ─────────────────────────────────────────────────────────────────────────────
# Runtime RAM layout
#
# Mirrors the load path in udynlink.c udynlink_load_module_image() and the
# helpers get_code_pointer()/get_data_pointer()/get_ram_size_for_header().
# The main RAM block laid out by the loader is:
#
#   untagged (today's exact layout — no padding, no alignment work):
#     COPY_ALL:       [LOT][header][relocs][symtab][pad][code][data][bss]
#     COPY_TEXT_DATA: [LOT][code][data][bss]          (metadata stays in flash)
#     XIP:            [LOT][data][bss]                (code stays in flash)
#
#   sectioned (each MAIN base aligned up to its table `align`):
#     COPY_ALL:       [LOT][bases][hdr+relocs+symt+sectab][pad][text][pad][data][pad][bss]
#     COPY_TEXT_DATA: [LOT][bases][pad][text][pad][data][pad][bss]
#     XIP:            [LOT][bases][pad][data][pad][bss]  (main text in the image)
#
# The LOT occupies the first num_lot*4 bytes (r9 = p_ram); the non-main
# section-base array sits immediately after it (empty for untagged modules,
# keeping today's layout, sizes and offsets bit-identical). Non-main (tagged)
# sections live outside this block at host-resolved addresses: their CODE/DATA
# payloads are copied from the image and BSS zeroed, in every load mode —
# tagged code is copied even under XIP, because the host asked for that memory.
# ─────────────────────────────────────────────────────────────────────────────

@dataclass
class RamSection:
    """A contiguous region of the loaded module's RAM (or flash, for XIP code)."""
    name: str
    base: int       # absolute address (ram_base-relative when ram_base=0)
    size: int
    in_ram: bool    # False for XIP .code (executed in place from the image)
    # Linkage to the section table (None for loader-internal regions like the
    # LOT, base array or copied header).
    section_index: Optional[int] = None
    section_class: Optional[int] = None   # SEC_CLASS_* when table-backed


@dataclass
class AllocationCall:
    """One simulated udynlink_external_malloc/free argument set (contract §3).

    ``section is None`` is the module's main RAM block; any other value names
    a tagged section, valid for the duration of the call only (it points into
    the image's string pool, like the C callback's ``const char *``).
    """
    size: int
    section: Optional[str]
    align: int
    flags: int          # host-visible hints (MAIN + reserved bits masked out)


@dataclass
class SectionBlock:
    """A simulated host-owned block backing one non-main section."""
    section: ImageSection
    base: int
    data: bytearray     # payload copied for CODE/DATA, zeroed for BSS


@dataclass
class ResolvedSymbol:
    """Result of resolving a symbol to its runtime address."""
    symbol: Symbol
    address: int               # absolute runtime address (ram_base-relative)
    resolved_by: str           # "module" | "host" | "module-default" | "unresolved" | "unmapped"


@dataclass
class RelocationReport:
    """Per-relocation outcome of RuntimeLayout.apply_relocations()."""
    index: int
    relocation: Relocation
    target: str                # "LOT" | "DATA" | "CODE" | tagged section name
    target_address: int
    kind: str                  # RelocKind name or "SYMBOL:<type>"
    written: Optional[int]     # the 32-bit word written (None if unresolved/deferred)
    note: str                  # human-readable detail


@dataclass
class LoadSimulation:
    """Result of RuntimeLayout.simulate_load()."""
    ram: bytearray
    reports: List[RelocationReport]
    section_blocks: Dict[int, SectionBlock]   # by section index
    allocations: List[AllocationCall]
    issues: List[str]   # placement/validation problems (empty = clean load)


@dataclass
class RuntimeLayout:
    """Models where each section of a module lands once loaded under ``mode``.

    Addresses are ``ram_base``-relative by default (set ``ram_base`` to host a
    module at a real address). Section placement exactly reproduces
    udynlink.c's get_code_pointer / get_data_pointer / the COPY_ALL copy path.
    For sectioned images each MAIN base is aligned up to its table ``align``
    inside the block, and non-main sections resolve through
    ``section_bases`` (index -> host address), mirroring the loader's
    section-base array and the extended allocator validation.
    """
    image: ModuleImage
    mode: LoadMode
    ram_base: int = 0
    section_bases: Optional[Dict[int, int]] = None   # non-main index -> base

    # ── section-table views ──────────────────────────────────────────────
    @property
    def main_sections(self) -> List[ImageSection]:
        return [s for s in self.image.sections if s.is_main]

    @property
    def nonmain_sections(self) -> List[ImageSection]:
        return [s for s in self.image.sections if not s.is_main]

    @property
    def nonmain_bases_size(self) -> int:
        """Bytes of the base array stored right after the LOT (0 untagged)."""
        return 4 * len(self.nonmain_sections)   # uintptr_t is 4 on the ARM targets

    @property
    def main_align(self) -> int:
        """Alignment the main-block allocation must satisfy (4 untagged)."""
        return max((s.align for s in self.main_sections), default=4)

    def _main_block_walk(self) -> Tuple[Dict[int, Optional[int]], int]:
        """Walk the main block; returns ({section_index: base|None}, end_offset).

        The LOT sits at offset 0, the base array follows it, COPY_ALL appends
        the copied metadata, and every MAIN base is aligned up to its
        ``align``. With the synthesized untagged view (all aligns 4, no base
        array) this reduces exactly to today's get_code_pointer /
        get_data_pointer formulas — bit-identical offsets and ram_size.
        """
        h = self.image.header
        off = h.num_lot * 4 + self.nonmain_bases_size
        if self.mode == LoadMode.COPY_ALL:
            off += self.image.code_offset   # metadata block copied verbatim
        bases: Dict[int, Optional[int]] = {}
        for sec in self.main_sections:
            if self.mode == LoadMode.XIP and sec.index == 0 and sec.cls == SEC_CLASS_CODE:
                bases[sec.index] = None     # main .text executes in the image
                continue
            off = align_up(off, sec.align)
            bases[sec.index] = self.ram_base + off
            off += sec.size
        return bases, off

    @property
    def main_bases(self) -> Dict[int, Optional[int]]:
        return self._main_block_walk()[0]

    # ── section bases (absolute: ram_base + offset) ──────────────────────
    @property
    def lot_base(self) -> int:
        """Address of the LOT (start of the RAM region)."""
        return self.ram_base

    @property
    def ram_size(self) -> int:
        return self._main_block_walk()[1]

    @property
    def code_base(self) -> int:
        """Runtime address of the main .code section. Mirrors get_code_pointer().

        COPY_ALL/COPY_TEXT_DATA: in RAM after the LOT (and, for COPY_ALL, the
        copied metadata), aligned to the table's ``align``. XIP: the main text
        stays in the source image and, like today's loader, this reports the
        ram_base placeholder — see code_in_image_offset for the real location.
        """
        base = self.main_bases.get(0)
        return self.ram_base if base is None else base

    @property
    def code_in_image_offset(self) -> int:
        """For XIP, the main .code lives in the source image at this offset."""
        return self.image.payload_image_offset(0) if self.mode == LoadMode.XIP else 0

    @property
    def code_in_ram(self) -> bool:
        return self.mode != LoadMode.XIP

    @property
    def data_base(self) -> int:
        """Runtime address of the main .data section. Mirrors get_data_pointer()."""
        base = self.main_bases.get(1)
        return self.ram_base if base is None else base

    @property
    def bss_base(self) -> int:
        base = self.main_bases.get(2)
        return self.data_base if base is None else base

    @property
    def header_in_ram(self) -> bool:
        return self.mode == LoadMode.COPY_ALL

    @property
    def header_base(self) -> int:
        """Address where the (possibly copied) header lives at runtime.

        In COPY_ALL it follows the non-main base array
        (p_ram + num_lot*4 + nonmain_bases_size, contract §2.3).
        """
        if self.mode == LoadMode.COPY_ALL:
            return self.ram_base + self.image.header.num_lot * 4 + self.nonmain_bases_size
        # COPY_TEXT_DATA / XIP: header stays in the source image (offset 0).
        return self.ram_base

    def _section_base(self, sec: ImageSection) -> Optional[int]:
        """Runtime base of a table section (None = not placed)."""
        if sec.is_main:
            return self.main_bases.get(sec.index)
        if self.section_bases is None:
            return None
        return self.section_bases.get(sec.index)

    def runtime_address(self, va: int) -> Optional[int]:
        """Resolve a link-time VA through the section map (sectioned images).

        runtime(va) = sec_base[idx(va)] + (va - sec[idx].va). Returns None when
        no section window contains ``va`` or its base is not placed. Never use
        for untagged images: their code/data ranges overlap (arena-relative
        values) and resolution keeps the SYM_INFO_CODE_MASK branch.
        """
        sec = self.image.find_section_by_va(va)
        if sec is None:
            return None
        base = self._section_base(sec)
        if base is None:
            return None
        return base + (va - sec.va)

    def _ram_section_for(self, sec: ImageSection, base: int, in_ram: bool,
                         fallback: str) -> RamSection:
        name = sec.name or fallback
        return RamSection(name, base, sec.size, in_ram, sec.index, sec.cls)

    def sections(self) -> List[RamSection]:
        main = {s.index: s for s in self.main_sections}
        out: List[RamSection] = []
        if self.header_in_ram:
            out.append(RamSection("header", self.header_base,
                                  self.image.header.header_size, True))
        out.append(RamSection("lot", self.lot_base, self.image.header.num_lot * 4, True))
        if self.nonmain_bases_size:
            out.append(RamSection("section bases", self.ram_base + self.image.header.num_lot * 4,
                                  self.nonmain_bases_size, True))
        if self.code_in_ram:
            out.append(self._ram_section_for(main[0], self.code_base, True, "code"))
        else:
            out.append(self._ram_section_for(main[0], self.ram_base + self.code_in_image_offset,
                                             False, "code(xip)"))
        out.append(self._ram_section_for(main[1], self.data_base, True, "data"))
        out.append(self._ram_section_for(main[2], self.bss_base, True, "bss"))
        for sec in self.nonmain_sections:
            base = self._section_base(sec)
            if base is None:
                continue    # unplaced sections appear once the host maps them
            out.append(self._ram_section_for(sec, base, True, "section[%d]" % sec.index))
        return out

    # ── symbol resolution (mirrors offset_sym) ───────────────────────────
    def resolve_symbol(self, sym: Symbol,
                        host_resolver: Optional[Callable[[str], int]] = None
                        ) -> ResolvedSymbol:
        """Resolve a symbol's runtime address.

        Untagged images keep today's branch verbatim: SYM_INFO_CODE_MASK picks
        code_base vs data_base and ``val`` is section-relative (data/bss
        values are arena-relative). Sectioned images resolve through the
        flat-VA map — runtime(sym.val) via the symbol's own section; the
        location bit is redundant there. WEAK additionally consults
        ``host_resolver``; a positive override replaces the default (a
        deferred symbol keeps the default). EXTERN: consults ``host_resolver``
        only. Mirrors udynlink.c offset_sym + the WEAK/EXTERN paths in
        udynlink_load_apply_relocations / udynlink_lookup_symbol.
        """
        if sym.type in (SYM_TYPE_INTERNAL, SYM_TYPE_EXPORTED, SYM_TYPE_WEAK):
            if self.image.has_sections:
                addr = self.runtime_address(sym.val_raw)
                if addr is None:
                    return ResolvedSymbol(sym, 0, "unmapped")
            else:
                base = self.code_base if sym.is_in_code else self.data_base
                addr = base + sym.val_raw
            if sym.type == SYM_TYPE_WEAK and host_resolver is not None:
                override = host_resolver(sym.name or "")
                if override == 1:  # UDYNLINK_SYM_DEFERRED
                    return ResolvedSymbol(sym, addr, "module-default")
                if override > 0:
                    return ResolvedSymbol(sym, override, "host")
            return ResolvedSymbol(sym, addr, "module")
        if sym.type == SYM_TYPE_EXTERN:
            if host_resolver is None:
                return ResolvedSymbol(sym, 0, "unresolved")
            addr = host_resolver(sym.name or "")
            if addr == 1:  # deferred
                return ResolvedSymbol(sym, 0, "unresolved")
            return ResolvedSymbol(sym, addr, "host" if addr > 0 else "unresolved")
        # MODULE_NAME has no runtime address.
        return ResolvedSymbol(sym, 0, "n/a")

    # ── allocation simulation (mirrors the extended callbacks, §3) ───────
    def allocation_plan(self) -> List[AllocationCall]:
        """The udynlink_external_malloc calls the loader will make, in order.

        One call for the main block (section=None, align=main_align) plus one
        per non-main section with its name/align/host flags. Untagged modules
        produce exactly today's single call: (ram_size, None, 4, 0).
        """
        plan = [AllocationCall(self.ram_size, None, self.main_align, 0)]
        for sec in self.nonmain_sections:
            plan.append(AllocationCall(sec.size, sec.name, sec.align, sec.host_flags))
        return plan

    def placement_issues(self) -> List[str]:
        """Loader-side validation of the host-supplied placement (empty = OK).

        Mirrors the load failures: a NULL/missing base is
        UDYNLINK_ERR_LOAD_SECTION_UNRESOLVED, a base violating the section's
        ``align`` is UDYNLINK_ERR_LOAD_SECTION_UNALIGNED, and the main block
        must satisfy main_align (UDYNLINK_ERR_LOAD_RAM_UNALIGNED).
        """
        issues: List[str] = []
        if self.ram_base % self.main_align != 0:
            issues.append("main block base 0x%X violates main_align %d (RAM_UNALIGNED)"
                          % (self.ram_base, self.main_align))
        bases = self.section_bases or {}
        for sec in self.nonmain_sections:
            base = bases.get(sec.index)
            if not base:
                issues.append("section[%d] '%s': no host base (SECTION_UNRESOLVED)"
                              % (sec.index, sec.name))
            elif base % sec.align != 0:
                issues.append("section[%d] '%s': base 0x%X not aligned to %d (SECTION_UNALIGNED)"
                              % (sec.index, sec.name, base, sec.align))
        return issues

    def section_block(self, sec: ImageSection) -> SectionBlock:
        """Simulated host-owned block for a non-main section: payload copied
        for CODE/DATA (in every load mode), zeroed for BSS."""
        base = (self.section_bases or {})[sec.index]
        data = bytearray(sec.size)
        if not sec.is_bss:
            payload = self.image.section_payload(sec.index)
            data[:len(payload)] = payload
        return SectionBlock(sec, base, data)

    def simulate_load(self,
                      host_resolver: Optional[Callable[[str], int]] = None,
                      host_malloc: Optional[Callable[[int, Optional[str], int, int], Optional[int]]] = None,
                      section_bases: Optional[Dict[int, int]] = None,
                      ) -> LoadSimulation:
        """Full load simulation: allocate, place, copy, relocate.

        ``host_malloc(size, section, align, flags)`` drives the extended
        allocator callback: its main-block answer becomes the block base and
        each per-section answer that section's base (None counts as
        SECTION_UNRESOLVED; alignment is validated afterwards). Without it,
        ``section_bases`` supplies the non-main addresses directly. Issues
        (NULL returns, misalignment) are collected instead of raised — the
        loader would fail with the corresponding error code.
        """
        allocations = self.allocation_plan()
        issues: List[str] = []
        ram_base = self.ram_base
        bases: Dict[int, int] = dict(self.section_bases or {})
        if host_malloc is not None:
            name_to_idx = {s.name: s.index for s in self.nonmain_sections}
            for call in allocations:
                addr = host_malloc(call.size, call.section, call.align, call.flags)
                if addr is None:
                    issues.append("malloc(size=%d, section=%r, align=%d, flags=0x%X) returned NULL (%s)"
                                  % (call.size, call.section, call.align, call.flags,
                                     "RAM_UNRESOLVED" if call.section is None else "SECTION_UNRESOLVED"))
                    continue
                if call.section is None:
                    ram_base = addr
                else:
                    bases[name_to_idx[call.section]] = addr
        elif section_bases is not None:
            bases = dict(section_bases)
        lay = RuntimeLayout(self.image, self.mode, ram_base, bases)
        issues += lay.placement_issues()
        # Host-owned blocks only for sections the placement actually resolved;
        # unresolved ones stay absent (their relocations report BAD).
        blocks = {sec.index: lay.section_block(sec)
                  for sec in lay.nonmain_sections if (lay.section_bases or {}).get(sec.index)}
        ram, reports = lay.apply_relocations(
            host_resolver, section_buffers={idx: b.data for idx, b in blocks.items()})
        return LoadSimulation(ram, reports, blocks, allocations, issues)

    # ── relocation application (mirrors udynlink_load_apply_relocations) ─
    def build_ram(self) -> bytearray:
        """Construct the initial (pre-relocation) main-block RAM buffer.

        LOT and the non-main base array are zero-filled (the loader writes the
        bases once the allocator answered); MAIN sections are copied from the
        image at their aligned bases according to the load mode; .bss is
        zeroed. Mirrors the copy section of udynlink_load_module_image(). For
        XIP the returned buffer excludes the in-flash main text (use
        code_in_image_offset). Non-main sections live in host-owned blocks
        outside this buffer — see section_block()/simulate_load().
        """
        h = self.image.header
        buf = bytearray(self.ram_size)
        bases, _ = self._main_block_walk()
        if self.nonmain_bases_size:
            # storage order = ascending index among non-main sections
            arr_off = h.num_lot * 4
            for i, sec in enumerate(self.nonmain_sections):
                base = (self.section_bases or {}).get(sec.index)
                if base:
                    struct.pack_into("<I", buf, arr_off + 4 * i, base & 0xFFFFFFFF)
        if self.mode == LoadMode.COPY_ALL:
            meta_off = h.num_lot * 4 + self.nonmain_bases_size
            # header + relocs + symtab (+ section table when present)
            buf[meta_off:meta_off + h.header_size] = self.image.header.pack()
            buf[meta_off + h.relocs_offset:meta_off + h.relocs_offset + h.relocs_size] = b"".join(r.pack() for r in self.image.relocations)
            buf[meta_off + h.symtab_offset:meta_off + h.symtab_offset + h.symt_size] = self.image._symtab_bytes
            if self.image._sectab_bytes:
                buf[meta_off + self.image.sectab_offset:meta_off + self.image.sectab_offset + len(self.image._sectab_bytes)] = self.image._sectab_bytes
        # MAIN section payloads at their aligned bases (bss already zeroed)
        for sec in self.main_sections:
            base = bases.get(sec.index)
            if base is None or sec.is_bss:
                continue    # XIP main text (base None) stays in the image
            off = base - self.ram_base
            buf[off:off + sec.size] = self.image.section_payload(sec.index)
        return buf

    def _lot_and_data_views(self, buf: bytearray):
        """Return (p_lot, p_data) as memoryviews over the RAM buffer.

        Mirrors the loader's p_lot = p_ram; p_data = get_data_pointer(p_mod).
        """
        h = self.image.header
        p_lot = memoryview(buf)[0:h.num_lot * 4]
        data_off = self.data_base - self.ram_base
        p_data = memoryview(buf)[data_off:data_off + h.data_size]
        return p_lot, p_data, data_off

    def apply_relocations(self,
                          host_resolver: Optional[Callable[[str], int]] = None,
                          section_buffers: Optional[Dict[int, bytearray]] = None,
                          ) -> Tuple[bytearray, List[RelocationReport]]:
        """Apply all relocations to a fresh RAM buffer, mirroring
        udynlink_load_apply_relocations().

        Untagged images keep today's semantics verbatim: the
        ``num_lot + data_size/4`` bound, ``*p += data_base - addend`` for
        R_ARM_ABS32 and ``*p = code_base + *p`` for the code-base form.
        Sectioned images resolve through the VA map: the target word's link VA
        is ``code_size + 4*(lot_offset - num_lot)`` and must land inside a
        CODE or DATA section with room for 4 bytes (BSS is never a target),
        and both base-additive forms collapse to ``*p = runtime(*p)`` — the
        in-place word holds a VA and the value word's low bits are ignored.
        A target inside a non-main section is patched in the matching
        ``section_buffers[idx]`` entry when supplied (see simulate_load).

        Returns the patched main-block buffer and a per-relocation report.
        EXTERN symbols without a resolver (or unresolved by it) are reported
        as unresolved and their target is left at 0 — unlike the real loader
        this does not raise; inspect the report's ``note`` for failures.
        """
        h = self.image.header
        buf = self.build_ram()
        p_lot, p_data, data_off = self._lot_and_data_views(buf)
        # Untagged bound (today); sectioned images validate through the VA map.
        max_lot_offset = None if self.image.has_sections else h.num_lot + (h.data_size // 4)
        reports: List[RelocationReport] = []

        def put32(view, byte_index, value):
            struct.pack_into("<I", view, byte_index, value & 0xFFFFFFFF)

        for i, r in enumerate(self.image.relocations):
            lot_offset = r.lot_offset
            value = r.value
            if max_lot_offset is not None and lot_offset >= max_lot_offset:
                reports.append(RelocationReport(i, r, "?", 0, "BAD", None,
                                                "lot_offset %d >= max %d (BAD_RELOCATION_TABLE)" % (lot_offset, max_lot_offset)))
                continue

            if lot_offset < h.num_lot:
                target_byte = lot_offset * 4
                target_view = p_lot
                target_name = "LOT"
                target_addr = self.lot_base + target_byte
            elif self.image.has_sections:
                # The target word's link VA in the flat space: post-LOT data
                # words start at VA code_size, one u32 apart (encoder unchanged).
                va = h.code_size + 4 * (lot_offset - h.num_lot)
                sec = self.image.find_section_by_va(va)
                if (sec is None or sec.cls not in (SEC_CLASS_CODE, SEC_CLASS_DATA)
                        or va + 4 > sec.va + sec.size):
                    reports.append(RelocationReport(i, r, "?", 0, "BAD", None,
                                                    "target VA 0x%X not inside a CODE/DATA section with 4-byte room (BAD_RELOCATION_TABLE)" % va))
                    continue
                target_addr = self.runtime_address(va)
                if target_addr is None:
                    reports.append(RelocationReport(i, r, sec.name or "?", 0, "BAD", None,
                                                    "section '%s' has no runtime base (host mapping missing)" % (sec.name,)))
                    continue
                if sec.is_main:
                    target_byte = target_addr - self.ram_base
                    target_view = memoryview(buf)
                    target_name = "CODE" if sec.cls == SEC_CLASS_CODE else "DATA"
                else:
                    target_name = sec.name or ("section[%d]" % sec.index)
                    target_view = (memoryview(section_buffers[sec.index])
                                   if section_buffers and sec.index in section_buffers else None)
                    if target_view is None:
                        reports.append(RelocationReport(i, r, target_name, target_addr, "BAD", None,
                                                        "target in non-main section '%s' but no buffer supplied" % target_name))
                        continue
                    target_byte = target_addr - self.section_bases[sec.index]
            else:
                target_byte = (lot_offset - h.num_lot) * 4
                target_view = p_data
                target_name = "DATA"
                target_addr = self.data_base + target_byte

            if r.kind == RelocKind.DATA_BASE:
                if lot_offset < h.num_lot:
                    reports.append(RelocationReport(i, r, target_name, target_addr, "DATA_BASE", None,
                                                    "R_ARM_ABS32 lot_offset < num_lot (BAD)"))
                    continue
                old = struct.unpack_from("<I", target_view, target_byte)[0]
                if self.image.has_sections:
                    # *p = runtime(*p): the word holds a link VA; the low bits
                    # of value (the section symbol's value) are ignored.
                    new = self.runtime_address(old)
                    if new is None:
                        reports.append(RelocationReport(i, r, target_name, target_addr, "DATA_BASE", None,
                                                        "in-place VA 0x%X not inside any section (BAD_RELOCATION_TABLE)" % old))
                        continue
                    put32(target_view, target_byte, new)
                    reports.append(RelocationReport(i, r, target_name, target_addr, "DATA_BASE", new,
                                                    "R_ARM_ABS32: data[0x%X]: VA 0x%X -> 0x%X" % (target_byte, old, new)))
                else:
                    new = (old + (self.data_base - r.data_addend)) & 0xFFFFFFFF
                    put32(target_view, target_byte, new)
                    reports.append(RelocationReport(i, r, target_name, target_addr, "DATA_BASE", new,
                                                    "R_ARM_ABS32: data[0x%X] += 0x%X - 0x%X" % (target_byte, self.data_base, r.data_addend)))
            elif r.kind == RelocKind.CODE_BASE:
                if lot_offset < h.num_lot:
                    reports.append(RelocationReport(i, r, target_name, target_addr, "CODE_BASE", None,
                                                    "code-base lot_offset < num_lot (BAD)"))
                    continue
                old = struct.unpack_from("<I", target_view, target_byte)[0]
                if self.image.has_sections:
                    # Same runtime(*p) op — both base-additive forms collapse.
                    new = self.runtime_address(old)
                    if new is None:
                        reports.append(RelocationReport(i, r, target_name, target_addr, "CODE_BASE", None,
                                                        "in-place VA 0x%X not inside any section (BAD_RELOCATION_TABLE)" % old))
                        continue
                    put32(target_view, target_byte, new)
                    reports.append(RelocationReport(i, r, target_name, target_addr, "CODE_BASE", new,
                                                    "R_ARM_TARGET1: data[0x%X]: VA 0x%X -> 0x%X" % (target_byte, old, new)))
                else:
                    new = (self.code_base + old) & 0xFFFFFFFF
                    put32(target_view, target_byte, new)
                    reports.append(RelocationReport(i, r, target_name, target_addr, "CODE_BASE", new,
                                                    "R_ARM_TARGET1: data[0x%X] = code_base + 0x%X" % (target_byte, old)))
            else:  # SYMBOL
                if value >= len(self.image.symbols):
                    reports.append(RelocationReport(i, r, target_name, target_addr, "SYMBOL", None,
                                                    "symt index %d out of range" % value))
                    continue
                sym = self.image.symbols[value]
                if sym.type == SYM_TYPE_MODULE_NAME:
                    reports.append(RelocationReport(i, r, target_name, target_addr, "SYMBOL:MODULE_NAME", None,
                                                    "relocation against module name (BAD)"))
                    continue
                res = self.resolve_symbol(sym, host_resolver)
                if sym.type == SYM_TYPE_EXTERN and res.resolved_by != "host":
                    reports.append(RelocationReport(i, r, target_name, target_addr,
                                                    "SYMBOL:EXTERN", None,
                                                    "unresolved extern '%s'" % (sym.name or "")))
                elif res.resolved_by == "unmapped":
                    reports.append(RelocationReport(i, r, target_name, target_addr,
                                                    "SYMBOL:%s" % sym.type_name, None,
                                                    "symbol '%s' VA 0x%X not inside any placed section" % (sym.name, sym.val_raw)))
                else:
                    put32(target_view, target_byte, res.address)
                    reports.append(RelocationReport(i, r, target_name, target_addr,
                                                    "SYMBOL:%s" % sym.type_name, res.address,
                                                    "%s '%s' -> 0x%X (%s)" % (sym.type_name, sym.name, res.address, res.resolved_by)))
        return buf, reports
