"""Standalone parser for udynlink module images (``UDLM``) and runtime RAM layouts.

This module is the single source of truth, in Python, for the udynlink binary
module format.  Every constant and layout rule below is extracted verbatim from
the two authoritative definitions of the format and kept in sync with them:

* **Writer** — ``scripts/mkmodule`` (``process()``): how a ``.bin`` image is
  encoded (header field order/struct format, relocation entry layout, symbol
  table bit packing, code/data placement).
* **C reader** — ``udynlink/udynlink.h`` + ``udynlink/udynlink.c``: how the
  loader decodes the same bytes at runtime (``udynlink_module_header_t``,
  ``get_sym_at_raw``, ``udynlink_load_apply_relocations``,
  ``udynlink_image_from_memory``, ``get_code_pointer``/``get_data_pointer``,
  ``get_ram_size_for_header``).

Both encode and decode an identical on-disk layout; this parser reproduces it
losslessly and also models the *runtime* layout (where each section lands in
RAM once a module is loaded under a given load mode, and how relocations patch
that RAM).

The parser is standalone: it depends only on the Python standard library.
``mkmodule`` and the test suite import the format constants from here so the
format is defined exactly once across the Python toolchain.
"""

import struct
from dataclasses import dataclass, field
from enum import IntEnum
from typing import Callable, List, Optional, Tuple

__all__ = [
    # Signature / version
    "MODULE_SIGNATURE", "MODULE_SIGN_VALUE",
    "pack_version", "get_major_version", "get_minor_version",
    "LOADER_ABI_VERSION",
    # Header
    "HEADER_FORMAT", "HEADER_SIZE", "HEADER_SIZE_V2",
    "ModuleHeader",
    # Symbol table
    "SYM_TYPE_INTERNAL", "SYM_TYPE_EXPORTED", "SYM_TYPE_EXTERN",
    "SYM_TYPE_MODULE_NAME", "SYM_TYPE_WEAK",
    "SYM_NAME_OFFSET_MASK", "SYM_INFO_SHIFT",
    "SYM_INFO_TYPE_MASK", "SYM_INFO_CODE_MASK",
    "SYM_LOCATION_CODE", "SYM_LOCATION_DATA",
    "Symbol", "sym_type_name", "sym_location_name",
    # Relocations
    "RELOC_FLAG_DATA_BASE", "RELOC_FLAG_CODE_BASE", "RELOC_VALUE_MASK",
    "Relocation", "RelocKind",
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
# UDYNLINK_LOADER_ABI_VERSION = UDYNLINK_MAKE_VERSION(3, 0)). Used only for the
# version-comparison helper; the parser reads any udynlink_version field.
LOADER_ABI_VERSION = pack_version(3, 0)


# ─────────────────────────────────────────────────────────────────────────────
# Module image header (32 bytes for ABI v1.0 / v3.0+, 36 bytes for v2.0)
#
# Field order reproduced verbatim from mkmodule process() pack sequence and the
# C struct udynlink_module_header_t in udynlink.h:
#   sign(4) mod_version(2) udynlink_version(2) arch_tag(2) num_lot(2)
#   num_rels(2) reserved(2) symt_size(4) code_size(4) data_size(4) bss_size(4)
# ─────────────────────────────────────────────────────────────────────────────

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
    reserved: int
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
            self.num_lot, self.num_rels, self.reserved, self.symt_size,
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
    def code_offset(self) -> int:
        """Byte offset of the .code section from the image start.

        Mirrors get_code_offset_from_header(): header + relocs + symt, aligned
        up to 4.
        """
        return align4(self.symtab_offset + self.symt_size)

    @property
    def metadata_size(self) -> int:
        """Bytes from image start to .code start (header + relocs + symt + pad).

        Mirrors udynlink_get_image_metadata_size().
        """
        return self.code_offset

    @property
    def data_offset(self) -> int:
        """Byte offset of the .data section from the image start."""
        return self.code_offset + self.code_size

    @property
    def image_size(self) -> int:
        """Total on-disk image size (header + relocs + symt + code + data).

        Mirrors udynlink_get_image_size() (bss is not stored on disk).
        """
        return self.data_offset + self.data_size

    def ram_size(self, mode: "LoadMode") -> int:
        """RAM bytes required to load under ``mode``.

        Mirrors get_ram_size_for_header() in udynlink.c.
        """
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
# Parsed module image (on-disk layout)
# ─────────────────────────────────────────────────────────────────────────────

@dataclass
class ModuleImage:
    """A fully decoded UDLM module image.

    ``parse_module`` populates this; every byte of the source blob is accounted
    for by ``header.image_size``.
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

    def runtime_layout(self, mode: LoadMode, ram_base: int = 0) -> "RuntimeLayout":
        return RuntimeLayout(self, mode, ram_base)

    def to_bytes(self) -> bytes:
        """Re-serialize the image byte-for-byte from its decoded parts.

        Lossless round-trip: ``parse_module(img).to_bytes() == img`` for any
        well-formed image. The symbol table and relocation table are rebuilt
        from their raw on-disk words; the name pool and code/data are carried
        verbatim. This verifies the parser's understanding of the byte layout.
        """
        out = bytearray(self.header.pack())
        for r in self.relocations:
            out += r.pack()
        # Symtab: we rebuild from the decoded raw entries + the name pool that
        # followed them. The name pool length is symtab_offset-offset_of_names.
        out += self._symtab_bytes
        out = bytearray(align4_at_end(out))
        out += self.code
        out += self.data
        return bytes(out)

    # populated by parse_module
    _symtab_bytes: bytes = b""


def align4_at_end(buf: bytearray) -> bytearray:
    """Pad ``buf`` with NULs to a multiple of 4 bytes (mkmodule's tail pad)."""
    pad = (4 - (len(buf) % 4)) % 4
    if pad:
        buf += b"\x00" * pad
    return buf


# ─────────────────────────────────────────────────────────────────────────────
# Image parsing
# ─────────────────────────────────────────────────────────────────────────────

def parse_module(data: bytes, validate: bool = True) -> ModuleImage:
    """Parse a UDLM module image blob into a :class:`ModuleImage`.

    This mirrors the reader side of the format exactly: it locates the header,
    relocation table, symbol table, code and data sections by the same offsets
    the C loader uses (udynlink_image_from_memory / get_sym_at_raw), and decodes
    each symbol/relocation entry with the on-disk bit layout.

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
        img_size = header.image_size
        if len(data) < img_size:
            raise ValueError("buffer truncated: header claims %d bytes, got %d" % (img_size, len(data)))

    relocs_off = header.relocs_offset
    symtab_off = header.symtab_offset
    code_off   = header.code_offset
    data_off   = header.data_offset

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
            name_off = name_off_raw & SYM_NAME_OFFSET_MASK
            if name_off < header.symt_size:
                end = symtab_bytes.find(b"\x00", name_off)
                if end < 0:
                    end = header.symt_size
                name = symtab_bytes[name_off:end].decode("utf-8", errors="replace")
            else:
                name = None
        symbols.append(Symbol(i, name_off_raw, val_raw, name, sym_type, location))

    code = data[code_off:code_off + header.code_size]
    data_section = data[data_off:data_off + header.data_size]

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
    )
    img._symtab_bytes = symtab_bytes  # type: ignore[attr-defined]
    return img


# ─────────────────────────────────────────────────────────────────────────────
# Runtime RAM layout
#
# Mirrors the load path in udynlink.c udynlink_load_module_image() and the
# helpers get_code_pointer()/get_data_pointer()/get_ram_size_for_header().
# The RAM region laid out by the loader is:
#
#   COPY_ALL:       [LOT][header][relocs][symtab][pad][code][data][bss]
#   COPY_TEXT_DATA: [LOT][code][data][bss]          (metadata stays in flash)
#   XIP:            [LOT][data][bss]                 (code stays in flash)
#
# In all three modes the LOT occupies the first num_lot*4 bytes of RAM and the
# .data section (whose .data relocation targets index into) immediately follows
# the code (when code is in RAM) or the LOT (XIP). .bss is zeroed and lives
# right after .data.
# ─────────────────────────────────────────────────────────────────────────────

@dataclass
class RamSection:
    """A contiguous region of the loaded module's RAM (or flash, for XIP code)."""
    name: str
    base: int       # absolute address (ram_base-relative when ram_base=0)
    size: int
    in_ram: bool    # False for XIP .code (executed in place from the image)


@dataclass
class ResolvedSymbol:
    """Result of resolving a symbol to its runtime address."""
    symbol: Symbol
    address: int               # absolute runtime address (ram_base-relative)
    resolved_by: str           # "module" | "host" | "module-default" | "unresolved"


@dataclass
class RelocationReport:
    """Per-relocation outcome of RuntimeLayout.apply_relocations()."""
    index: int
    relocation: Relocation
    target: str                # "LOT" | "DATA"
    target_address: int
    kind: str                  # RelocKind name or "SYMBOL:<type>"
    written: Optional[int]     # the 32-bit word written (None if unresolved/deferred)
    note: str                  # human-readable detail


@dataclass
class RuntimeLayout:
    """Models where each section of a module lands once loaded under ``mode``.

    Addresses are ``ram_base``-relative by default (set ``ram_base`` to host a
    module at a real address). Section placement exactly reproduces
    udynlink.c's get_code_pointer / get_data_pointer / the COPY_ALL copy path.
    """
    image: ModuleImage
    mode: LoadMode
    ram_base: int = 0

    # ── section bases (absolute: ram_base + offset) ──────────────────────
    @property
    def lot_base(self) -> int:
        """Address of the LOT (start of the RAM region)."""
        return self.ram_base

    @property
    def ram_size(self) -> int:
        return self.image.header.ram_size(self.mode)

    @property
    def code_base(self) -> int:
        """Runtime address of .code.

        COPY_ALL/COPY_TEXT_DATA: in RAM, after the LOT (and, for COPY_ALL, after
        the copied metadata). XIP: in the source image at code_offset.
        Mirrors get_code_pointer().
        """
        h = self.image.header
        if self.mode == LoadMode.COPY_TEXT_DATA:
            return self.ram_base + h.num_lot * 4
        if self.mode == LoadMode.COPY_ALL:
            # p_header (in RAM) + code_offset; header is at ram_base + num_lot*4
            return self.ram_base + h.num_lot * 4 + h.code_offset
        # XIP: code stays in the source image.
        return self.ram_base  # placeholder; see code_in_image_offset

    @property
    def code_in_image_offset(self) -> int:
        """For XIP, the .code lives in the source image at this byte offset."""
        return self.image.header.code_offset if self.mode == LoadMode.XIP else 0

    @property
    def code_in_ram(self) -> bool:
        return self.mode != LoadMode.XIP

    @property
    def data_base(self) -> int:
        """Runtime address of .data. Mirrors get_data_pointer()."""
        h = self.image.header
        if self.mode == LoadMode.XIP:
            return self.ram_base + h.num_lot * 4
        if self.mode == LoadMode.COPY_TEXT_DATA:
            return self.ram_base + h.num_lot * 4 + h.code_size
        # COPY_ALL: p_header (in RAM) + code_offset + code_size
        return self.ram_base + h.num_lot * 4 + h.code_offset + h.code_size

    @property
    def bss_base(self) -> int:
        return self.data_base + self.image.header.data_size

    @property
    def header_in_ram(self) -> bool:
        return self.mode == LoadMode.COPY_ALL

    @property
    def header_base(self) -> int:
        """Address where the (possibly copied) header lives at runtime."""
        if self.mode == LoadMode.COPY_ALL:
            return self.ram_base + self.image.header.num_lot * 4
        # COPY_TEXT_DATA / XIP: header stays in the source image (offset 0).
        return self.ram_base

    def sections(self) -> List[RamSection]:
        h = self.image.header
        out: List[RamSection] = []
        if self.header_in_ram:
            out.append(RamSection("header", self.header_base, h.header_size, True))
        out.append(RamSection("lot", self.lot_base, h.num_lot * 4, True))
        if self.code_in_ram:
            out.append(RamSection("code", self.code_base, h.code_size, True))
        else:
            out.append(RamSection("code(xip)", self.ram_base + self.code_in_image_offset, h.code_size, False))
        out.append(RamSection("data", self.data_base, h.data_size, True))
        out.append(RamSection("bss", self.bss_base, h.bss_size, True))
        return out

    # ── symbol resolution (mirrors offset_sym) ───────────────────────────
    def resolve_symbol(self, sym: Symbol,
                        host_resolver: Optional[Callable[[str], int]] = None
                        ) -> ResolvedSymbol:
        """Resolve a symbol's runtime address.

        For INTERNAL/EXPORTED/WEAK: the module's own address (code_base or
        data_base + sym.val_raw). WEAK additionally consults ``host_resolver``;
        a positive override replaces the default (a deferred symbol keeps the
        default). EXTERN: consults ``host_resolver`` only.
        Mirrors udynlink.c offset_sym + the WEAK/EXTERN paths in
        udynlink_load_apply_relocations / udynlink_lookup_symbol.
        """
        if sym.type in (SYM_TYPE_INTERNAL, SYM_TYPE_EXPORTED, SYM_TYPE_WEAK):
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

    # ── relocation application (mirrors udynlink_load_apply_relocations) ─
    def build_ram(self) -> bytearray:
        """Construct the initial (pre-relocation) RAM buffer for this layout.

        LOT is zeroed; .code/.data are copied from the image (according to the
        load mode); .bss is zeroed. Mirrors the copy section of
        udynlink_load_module_image(). For XIP the returned buffer excludes the
        in-flash code (use code_base to locate it in the source image).
        """
        h = self.image.header
        buf = bytearray(self.ram_size)
        # LOT occupies [0, num_lot*4); left zeroed.
        if self.mode == LoadMode.COPY_ALL:
            meta_off = h.num_lot * 4
            # header + relocs + symtab
            buf[meta_off:meta_off + h.header_size] = self.image.header.pack()
            buf[meta_off + h.relocs_offset:meta_off + h.relocs_offset + h.relocs_size] = b"".join(r.pack() for r in self.image.relocations)
            buf[meta_off + h.symtab_offset:meta_off + h.symtab_offset + h.symt_size] = self.image._symtab_bytes
            # pad between symtab and code already zeroed
            buf[meta_off + h.code_offset:meta_off + h.code_offset + h.code_size] = self.image.code
            buf[meta_off + h.data_offset:meta_off + h.data_offset + h.data_size] = self.image.data
        elif self.mode == LoadMode.COPY_TEXT_DATA:
            cd_off = h.num_lot * 4
            buf[cd_off:cd_off + h.code_size] = self.image.code
            buf[cd_off + h.code_size:cd_off + h.code_size + h.data_size] = self.image.data
        else:  # XIP
            d_off = h.num_lot * 4
            buf[d_off:d_off + h.data_size] = self.image.data
        # bss already zeroed
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
                          ) -> Tuple[bytearray, List[RelocationReport]]:
        """Apply all relocations to a fresh RAM buffer, mirroring
        udynlink_load_apply_relocations() exactly.

        Returns the patched RAM buffer and a per-relocation report. EXTERN
        symbols without a resolver (or unresolved by it) are reported as
        unresolved and their target is left at 0 — unlike the real loader this
        does not raise; inspect the report's ``note`` for failures.
        """
        h = self.image.header
        buf = self.build_ram()
        p_lot, p_data, data_off = self._lot_and_data_views(buf)
        max_lot_offset = h.num_lot + (h.data_size // 4)
        reports: List[RelocationReport] = []

        def put32(view, byte_index, value):
            struct.pack_into("<I", view, byte_index, value & 0xFFFFFFFF)

        for i, r in enumerate(self.image.relocations):
            lot_offset = r.lot_offset
            value = r.value
            if lot_offset >= max_lot_offset:
                reports.append(RelocationReport(i, r, "?", 0, "BAD", None,
                                                "lot_offset %d >= max %d (BAD_RELOCATION_TABLE)" % (lot_offset, max_lot_offset)))
                continue

            target_is_lot = lot_offset < h.num_lot
            if target_is_lot:
                target_byte = lot_offset * 4
                target_view = p_lot
                target_name = "LOT"
                target_addr = self.lot_base + target_byte
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
                else:
                    put32(target_view, target_byte, res.address)
                    reports.append(RelocationReport(i, r, target_name, target_addr,
                                                    "SYMBOL:%s" % sym.type_name, res.address,
                                                    "%s '%s' -> 0x%X (%s)" % (sym.type_name, sym.name, res.address, res.resolved_by)))
        return buf, reports
