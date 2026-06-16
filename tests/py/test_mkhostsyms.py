"""Comprehensive pytest tests for the mkhostsyms tool.

Unit tests exercise gnu_hash, next_power_of_2, build_gnu_hash_table, and
emit_c_header without needing an ARM cross-compiler.

Integration tests (marked @pytest.mark.integration) compile a small C file
with arm-none-eabi-gcc and exercise read_host_symbols + the full pipeline.
They are automatically skipped when the cross-compiler is unavailable.
"""

import os
import re
import shutil
import subprocess
import sys
import types

import pytest

# ---------------------------------------------------------------------------
# Import mkhostsyms (it has no .py extension, so use exec into a module)
# ---------------------------------------------------------------------------

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.abspath(os.path.join(_THIS_DIR, "..", ".."))
_MKHOSTSYMS_PATH = os.path.join(_REPO_ROOT, "scripts", "mkhostsyms")

mkhostsyms = types.ModuleType("mkhostsyms")
mkhostsyms.__file__ = _MKHOSTSYMS_PATH
with open(_MKHOSTSYMS_PATH) as _f:
    _code = compile(_f.read(), _MKHOSTSYMS_PATH, "exec")
    exec(_code, mkhostsyms.__dict__)

# Convenience aliases
gnu_hash = mkhostsyms.gnu_hash
next_power_of_2 = mkhostsyms.next_power_of_2
read_host_symbols = mkhostsyms.read_host_symbols
build_gnu_hash_table = mkhostsyms.build_gnu_hash_table
emit_c_header = mkhostsyms.emit_c_header
BLOOM_SHIFT = mkhostsyms.BLOOM_SHIFT
TrieFormat = mkhostsyms.TrieFormat
UDYNLINK_TRIE_NONE = TrieFormat.NONE
UDYNLINK_TRIE_FLAG_LEAF = TrieFormat.FLAG_LEAF
UDYNLINK_TRIE_FLAG_HAS_CHILD = TrieFormat.FLAG_HAS_CHILD

# ---------------------------------------------------------------------------
# Helpers for ARM cross-compiler detection
# ---------------------------------------------------------------------------

_ARM_GCC = shutil.which("arm-none-eabi-gcc")

skip_no_arm_gcc = pytest.mark.skipif(
    _ARM_GCC is None,
    reason="arm-none-eabi-gcc not available",
)


def _compile_arm_elf(source_path, output_path):
    """Compile *source_path* to an ARM ELF at *output_path*."""
    cmd = [
        _ARM_GCC,
        "-mcpu=cortex-m4",
        "-mthumb",
        "-nostdlib",
        "-Wl,--unresolved-symbols=ignore-in-object-files",
        "-o", output_path,
        source_path,
    ]
    subprocess.run(cmd, check=True)


def _write_test_c(path):
    """Write a small C source file with two global functions."""
    path.write_text(
        "int my_func(void) { return 42; }\n"
        "int another_func(void) { return 0; }\n"
    )


# ===================================================================
# Unit tests — no ARM toolchain required
# ===================================================================


class TestGnuHash:
    """Verify the GNU hash implementation against hand-computed values."""

    # Reference values computed from the canonical algorithm:
    #   h = 5381; for each c: h = (h*33 + ord(c)) & 0xFFFFFFFF

    @pytest.mark.parametrize(
        "name, expected",
        [
            ("", 0x1505),          # 5381 — initial value, empty string
            ("a", 0x2B606),
            ("printf", 0x156B2BB8),
            ("malloc", 0x0D39AD3D),
            ("my_func", 0xEB7CB676),
            ("another_func", 0x25FCBC01),
            ("free", 0x7C96F087),
        ],
    )
    def test_known_values(self, name, expected):
        assert gnu_hash(name) == expected

    def test_deterministic(self):
        """Same input always yields the same hash."""
        assert gnu_hash("hello") == gnu_hash("hello")

    def test_different_inputs_differ(self):
        """Different strings should (almost certainly) produce different hashes."""
        assert gnu_hash("abc") != gnu_hash("def")


class TestNextPowerOf2:
    """Edge cases for next_power_of_2."""

    @pytest.mark.parametrize(
        "n, expected",
        [
            (0, 1),
            (1, 1),
            (2, 2),
            (3, 4),
            (4, 4),
            (7, 8),
            (8, 8),
            (9, 16),
            (255, 256),
            (256, 256),
            (257, 512),
            (1023, 1024),
            (1024, 1024),
        ],
    )
    def test_values(self, n, expected):
        assert next_power_of_2(n) == expected

    def test_result_is_always_power_of_2(self):
        for n in range(0, 300):
            p = next_power_of_2(n)
            assert p > 0 and (p & (p - 1)) == 0, f"next_power_of_2({n}) = {p}"


class TestBuildGnuHashTableEmpty:
    """build_gnu_hash_table with an empty symbol list."""

    def test_returns_valid_structure(self):
        table = build_gnu_hash_table([])
        assert table["nbuckets"] == 1
        assert table["bloom_size"] == 1
        assert table["bloom"] == [0]
        assert table["buckets"] == [0]
        assert table["hash_values"] == []
        assert table["sym_addrs"] == []
        assert table["strtab"] == b""
        assert table["strtab_offsets"] == []
        assert table["names"] == []


class TestBuildGnuHashTableSingle:
    """build_gnu_hash_table with a single symbol."""

    def test_structure(self):
        symbols = [("my_func", 0x0800)]
        table = build_gnu_hash_table(symbols)

        assert table["names"] == ["my_func"]
        assert table["sym_addrs"] == [0x0800]
        assert len(table["hash_values"]) == 1
        # The sole entry is the last in its chain, so the terminator bit must be set.
        assert table["hash_values"][0] & 1 == 1
        # The bucket pointed to by this symbol's hash must be symoffset (0).
        h = gnu_hash("my_func")
        bucket_idx = h % table["nbuckets"]
        assert table["buckets"][bucket_idx] == table["symoffset"]

    def test_strtab_properly_built(self):
        symbols = [("my_func", 0x0800)]
        table = build_gnu_hash_table(symbols)

        assert table["strtab_offsets"][0] == 0
        # strtab is the null-terminated name
        assert table["strtab"] == b"my_func\x00"

    def test_bloom_filter_covers_hash(self):
        symbols = [("my_func", 0x0800)]
        table = build_gnu_hash_table(symbols)

        h = gnu_hash("my_func")
        h2 = h >> BLOOM_SHIFT
        idx = (h // 32) % table["bloom_size"]
        mask = (1 << (h % 32)) | (1 << (h2 % 32))
        assert table["bloom"][idx] & mask == mask


class TestBuildGnuHashTableMultiple:
    """build_gnu_hash_table with several symbols."""

    @pytest.fixture()
    def table(self):
        symbols = [
            ("my_func", 0x0800),
            ("another_func", 0x0900),
            ("helper", 0x0A00),
        ]
        return build_gnu_hash_table(symbols)

    def test_names_match(self, table):
        assert set(table["names"]) == {"my_func", "another_func", "helper"}

    def test_sym_addrs_match(self, table):
        name_to_addr = dict(zip(table["names"], table["sym_addrs"]))
        assert name_to_addr["my_func"] == 0x0800
        assert name_to_addr["another_func"] == 0x0900
        assert name_to_addr["helper"] == 0x0A00

    def test_strtab_null_terminated(self, table):
        """Each name in strtab must be null-terminated and recoverable."""
        for i, name in enumerate(table["names"]):
            offset = table["strtab_offsets"][i]
            end = table["strtab"].index(b"\x00", offset)
            recovered = table["strtab"][offset:end].decode("utf-8")
            assert recovered == name

    def test_bloom_filter_covers_all(self, table):
        """Every symbol's hash must pass the bloom filter check."""
        for name in table["names"]:
            h = gnu_hash(name)
            h2 = h >> BLOOM_SHIFT
            idx = (h // 32) % table["bloom_size"]
            mask = (1 << (h % 32)) | (1 << (h2 % 32))
            assert table["bloom"][idx] & mask == mask, (
                f"Bloom filter does not cover symbol {name!r}"
            )

    def test_bucket_chain_consistency(self, table):
        """Buckets must point to the start of each hash chain, and chains
        must be terminated with the low bit set on the last entry."""
        for bucket_val in table["buckets"]:
            if bucket_val == 0:
                continue
            pos = bucket_val - table["symoffset"]
            assert 0 <= pos < len(table["hash_values"])
            # Walk the chain until the terminator bit
            while True:
                assert 0 <= pos < len(table["hash_values"])
                hv = table["hash_values"][pos]
                if hv & 1:
                    break
                pos += 1

    def test_global_indices_sorted_by_bucket(self, table):
        """global_indices must be sorted by (hash % nbuckets)."""
        gi = table["global_indices"]
        hashes = [gnu_hash(table["names"][i]) for i in gi]
        buckets = [h % table["nbuckets"] for h in hashes]
        assert buckets == sorted(buckets)

    def test_custom_nbuckets(self):
        """Explicit nbuckets overrides the auto-calculated value."""
        symbols = [("a", 1), ("b", 2), ("c", 3)]
        table = build_gnu_hash_table(symbols, nbuckets=8)
        assert table["nbuckets"] == 8


class TestEmitCHeader:
    """Verify that emit_c_header produces compilable C output."""

    @pytest.fixture()
    def sample_table(self):
        symbols = [("my_func", 0x0800), ("helper", 0x0A00)]
        return build_gnu_hash_table(symbols)

    @pytest.fixture()
    def header_path(self, tmp_path):
        return tmp_path / "host_syms.h"

    def test_output_contains_arrays(self, sample_table, header_path):
        emit_c_header(sample_table, str(header_path))
        text = header_path.read_text()

        assert "g_host_hash_bloom[]" in text
        assert "g_host_hash_buckets[]" in text
        assert "g_host_hash_values[]" in text
        assert "g_host_sym_addrs[]" in text
        assert "g_host_strtab[]" in text
        assert "g_host_strtab_offsets[]" in text

    def test_output_contains_struct(self, sample_table, header_path):
        emit_c_header(sample_table, str(header_path))
        text = header_path.read_text()

        assert "udynlink_hash_table_t g_host_sym_table" in text

    def test_include_guard_default(self, sample_table, header_path):
        emit_c_header(sample_table, str(header_path))
        text = header_path.read_text()

        guard = "HOST_SYMS_HOST_SYMS_H"
        assert f"#ifndef {guard}" in text
        assert f"#define {guard}" in text
        assert f"#endif" in text

    def test_includes_udynlink_hash(self, sample_table, header_path):
        emit_c_header(sample_table, str(header_path))
        text = header_path.read_text()

        assert '#include "udynlink_hash.h"' in text

    def test_emit_c_header_custom_guard(self, sample_table, header_path):
        emit_c_header(sample_table, str(header_path), guard_name="MY_CUSTOM_GUARD")
        text = header_path.read_text()

        assert "#ifndef MY_CUSTOM_GUARD" in text
        assert "#define MY_CUSTOM_GUARD" in text
        text_after_endif = text[text.rfind("#endif"):]
        assert "#endif" in text_after_endif

    def test_strtab_hex_bytes(self, sample_table, header_path):
        """The strtab array should contain hex bytes matching the symbol names."""
        emit_c_header(sample_table, str(header_path))
        text = header_path.read_text()

        # The strtab should contain the bytes for "helper\0" and "my_func\0"
        # (order depends on hash sorting). Just verify it contains hex patterns.
        assert "0x" in text  # at least some hex bytes


class TestEmitCHeaderEmptyTable:
    """emit_c_header with an empty table (edge case)."""

    def test_produces_valid_file(self, tmp_path):
        table = build_gnu_hash_table([])
        header_path = tmp_path / "empty_syms.h"
        emit_c_header(table, str(header_path))

        text = header_path.read_text()
        assert "#ifndef" in text
        assert "#endif" in text
        assert "g_host_hash_bloom[]" in text


# ===================================================================
# Integration tests — require arm-none-eabi-gcc
# ===================================================================


@pytest.mark.integration
@skip_no_arm_gcc
class TestReadHostSymbols:
    """read_host_symbols against a real ARM ELF."""

    def test_read_host_symbols(self, tmp_path):
        """Compile a small C file, read its symbols, verify expected names."""
        src = tmp_path / "test.c"
        elf = tmp_path / "test.elf"
        _write_test_c(src)
        _compile_arm_elf(str(src), str(elf))

        symbols = read_host_symbols(str(elf))
        names = [s[0] for s in symbols]

        # Both global functions should be present
        assert "my_func" in names
        assert "another_func" in names

    def test_read_host_symbols_values_nonzero(self, tmp_path):
        """Symbol addresses should be non-zero for defined symbols."""
        src = tmp_path / "test.c"
        elf = tmp_path / "test.elf"
        _write_test_c(src)
        _compile_arm_elf(str(src), str(elf))

        symbols = read_host_symbols(str(elf))
        for name, value in symbols:
            if name in ("my_func", "another_func"):
                assert value != 0, f"Symbol {name!r} has zero address"


@pytest.mark.integration
@skip_no_arm_gcc
class TestReadHostSymbolsFilter:
    """read_host_symbols with a --filter regex."""

    def test_filter_selects_matching(self, tmp_path):
        src = tmp_path / "test.c"
        elf = tmp_path / "test.elf"
        _write_test_c(src)
        _compile_arm_elf(str(src), str(elf))

        filter_re = re.compile(r"my_")
        symbols = read_host_symbols(str(elf), filter_re=filter_re)
        names = [s[0] for s in symbols]

        assert "my_func" in names
        assert "another_func" not in names

    def test_filter_excludes_all(self, tmp_path):
        src = tmp_path / "test.c"
        elf = tmp_path / "test.elf"
        _write_test_c(src)
        _compile_arm_elf(str(src), str(elf))

        filter_re = re.compile(r"^nonexistent_$")
        symbols = read_host_symbols(str(elf), filter_re=filter_re)
        assert symbols == []


@pytest.mark.integration
@skip_no_arm_gcc
class TestEndToEnd:
    """Full pipeline: compile C → read symbols → build table → emit header →
    verify header compiles with arm-none-eabi-gcc."""

    def test_generated_header_compiles(self, tmp_path):
        src = tmp_path / "test.c"
        elf = tmp_path / "test.elf"
        _write_test_c(src)
        _compile_arm_elf(str(src), str(elf))

        # Full pipeline
        symbols = read_host_symbols(str(elf))
        table = build_gnu_hash_table(symbols)

        header_path = tmp_path / "host_syms.h"
        emit_c_header(table, str(header_path))

        # Verify the generated header compiles as C
        udynlink_include_dir = os.path.join(_REPO_ROOT, "udynlink")
        compile_cmd = [
            _ARM_GCC,
            "-mcpu=cortex-m4",
            "-mthumb",
            "-fsyntax-only",
            f"-I{udynlink_include_dir}",
            str(header_path),
        ]
        result = subprocess.run(compile_cmd, capture_output=True, text=True)
        assert result.returncode == 0, (
            f"Generated header failed to compile:\n"
            f"stdout: {result.stdout}\nstderr: {result.stderr}"
        )

    def test_generated_header_lookup_function(self, tmp_path):
        """Generate a header, compile a consumer that calls
        udynlink_resolve_hashed_symbol, and verify it links (syntax-only)."""
        src = tmp_path / "test.c"
        elf = tmp_path / "test.elf"
        _write_test_c(src)
        _compile_arm_elf(str(src), str(elf))

        symbols = read_host_symbols(str(elf))
        table = build_gnu_hash_table(symbols)

        header_path = tmp_path / "host_syms.h"
        emit_c_header(table, str(header_path))

        # Write a small consumer that uses the table
        consumer_path = tmp_path / "consumer.c"
        consumer_path.write_text(
            '#include "host_syms.h"\n'
            "void *test_lookup(void) {\n"
            '    return udynlink_resolve_hashed_symbol(&g_host_sym_table, "my_func");\n'
            "}\n"
        )

        udynlink_include_dir = os.path.join(_REPO_ROOT, "udynlink")
        compile_cmd = [
            _ARM_GCC,
            "-mcpu=cortex-m4",
            "-mthumb",
            "-fsyntax-only",
            f"-I{udynlink_include_dir}",
            f"-I{tmp_path}",
            str(consumer_path),
        ]
        result = subprocess.run(compile_cmd, capture_output=True, text=True)
        assert result.returncode == 0, (
            f"Consumer C file failed to compile:\n"
            f"stdout: {result.stdout}\nstderr: {result.stderr}"
        )


@pytest.mark.integration
@skip_no_arm_gcc
class TestEndToEndWithFilter:
    """End-to-end pipeline with a symbol filter applied."""

    def test_filtered_pipeline(self, tmp_path):
        src = tmp_path / "test.c"
        elf = tmp_path / "test.elf"
        _write_test_c(src)
        _compile_arm_elf(str(src), str(elf))

        filter_re = re.compile(r"my_")
        symbols = read_host_symbols(str(elf), filter_re=filter_re)
        table = build_gnu_hash_table(symbols)

        # Only my_func should be in the table
        assert "my_func" in table["names"]
        assert "another_func" not in table["names"]

        header_path = tmp_path / "host_syms.h"
        emit_c_header(table, str(header_path))

        # Verify the header still compiles
        udynlink_include_dir = os.path.join(_REPO_ROOT, "udynlink")
        compile_cmd = [
            _ARM_GCC,
            "-mcpu=cortex-m4",
            "-mthumb",
            "-fsyntax-only",
            f"-I{udynlink_include_dir}",
            str(header_path),
        ]
        result = subprocess.run(compile_cmd, capture_output=True, text=True)
        assert result.returncode == 0, (
            f"Filtered header failed to compile:\n"
            f"stdout: {result.stdout}\nstderr: {result.stderr}"
        )


# ===================================================================
# Trie format tests
# ===================================================================


def _trie_lookup(table, name):
    """Pure-Python reimplementation of udynlink_resolve_trie_symbol."""
    if table["num_nodes"] == 0:
        return None
    next_level = table["root_child"]
    matched_idx = UDYNLINK_TRIE_NONE
    for c in name:
        cur = next_level
        matched_idx = UDYNLINK_TRIE_NONE
        while cur != UDYNLINK_TRIE_NONE:
            node = table["nodes"][cur]
            node_ch = node["ch_flags"] & 0xFF
            if node_ch == ord(c):
                matched_idx = cur
                next_level = node["child"]
                break
            if node_ch > ord(c):
                return None
            cur = node["sibling"]
        if matched_idx == UDYNLINK_TRIE_NONE:
            return None
    if matched_idx != UDYNLINK_TRIE_NONE and (
        table["nodes"][matched_idx]["ch_flags"] & UDYNLINK_TRIE_FLAG_LEAF
    ):
        return table["leaf_addrs"][table["nodes"][matched_idx]["leaf_index"]]
    return None


class TestTrieBuildEmpty:
    def test_empty(self):
        fmt = TrieFormat()
        table = fmt.build([])
        assert table["num_nodes"] == 0
        assert table["num_leaves"] == 0
        assert table["root_child"] == UDYNLINK_TRIE_NONE
        assert table["nodes"] == []
        assert table["leaf_addrs"] == []


class TestTrieBuildSingle:
    def test_single_symbol(self):
        symbols = [("my_func", 0x0800)]
        fmt = TrieFormat()
        table = fmt.build(symbols)
        assert table["num_nodes"] == 7
        assert table["num_leaves"] == 1
        assert table["names"] == ["my_func"]
        assert table["leaf_addrs"] == [0x0800]

    def test_single_lookup(self):
        symbols = [("my_func", 0x0800)]
        fmt = TrieFormat()
        table = fmt.build(symbols)
        assert _trie_lookup(table, "my_func") == 0x0800
        assert _trie_lookup(table, "my_fun") is None
        assert _trie_lookup(table, "other") is None


class TestTrieBuildMultiple:
    @pytest.fixture()
    def table(self):
        symbols = [
            ("my_func", 0x0800),
            ("another_func", 0x0900),
            ("helper", 0x0A00),
        ]
        return TrieFormat().build(symbols)

    def test_lookup_all(self, table):
        assert _trie_lookup(table, "my_func") == 0x0800
        assert _trie_lookup(table, "another_func") == 0x0900
        assert _trie_lookup(table, "helper") == 0x0A00

    def test_lookup_miss(self, table):
        assert _trie_lookup(table, "nonexistent") is None
        assert _trie_lookup(table, "my_fun") is None
        assert _trie_lookup(table, "helpers") is None

    def test_siblings_sorted(self, table):
        """Root-level siblings must be sorted by character."""
        cur = table["root_child"]
        chars = []
        while cur != UDYNLINK_TRIE_NONE:
            node = table["nodes"][cur]
            chars.append(node["ch_flags"] & 0xFF)
            cur = node["sibling"]
        assert chars == sorted(chars)

    def test_leaf_flags(self, table):
        """Every leaf node must have the LEAF flag, non-leaves must not."""
        for node in table["nodes"]:
            if node["leaf_index"] != UDYNLINK_TRIE_NONE:
                assert node["ch_flags"] & UDYNLINK_TRIE_FLAG_LEAF
            if node["ch_flags"] & UDYNLINK_TRIE_FLAG_LEAF:
                assert node["leaf_index"] != UDYNLINK_TRIE_NONE

    def test_total_leaf_count(self, table):
        assert table["num_leaves"] == 3

    def test_prefix_sharing(self):
        """Symbols sharing a prefix should share trie nodes."""
        symbols = [("abc", 1), ("abd", 2)]
        table = TrieFormat().build(symbols)
        # 'a'(1) → 'b'(2) → 'c'/'d' siblings(2) = 4 nodes
        assert table["num_nodes"] == 4

    def test_no_prefix_sharing(self):
        """Completely different symbols should not share nodes."""
        symbols = [("abc", 1), ("xyz", 2)]
        table = TrieFormat().build(symbols)
        # 3 + 3 = 6 nodes (no shared prefix)
        assert table["num_nodes"] == 6


class TestTrieBuildPrefix:
    """One symbol is a prefix of another."""

    def test_prefix_is_leaf(self):
        symbols = [("foo", 0x100), ("foobar", 0x200)]
        table = TrieFormat().build(symbols)
        assert _trie_lookup(table, "foo") == 0x100
        assert _trie_lookup(table, "foobar") == 0x200
        assert _trie_lookup(table, "foob") is None

    def test_prefix_not_separate(self):
        """The 'foo' node should be both leaf and have children."""
        symbols = [("foo", 0x100), ("foobar", 0x200)]
        table = TrieFormat().build(symbols)
        # Navigate: root → f → o1 → o2 (leaf for "foo", has child for "foobar")
        cur = table["root_child"]
        found_f = None
        while cur != UDYNLINK_TRIE_NONE:
            if (table["nodes"][cur]["ch_flags"] & 0xFF) == ord("f"):
                found_f = cur
                break
            cur = table["nodes"][cur]["sibling"]
        assert found_f is not None
        o1 = table["nodes"][found_f]["child"]
        assert (table["nodes"][o1]["ch_flags"] & 0xFF) == ord("o")
        o2 = table["nodes"][o1]["child"]
        assert (table["nodes"][o2]["ch_flags"] & 0xFF) == ord("o")
        assert table["nodes"][o2]["ch_flags"] & UDYNLINK_TRIE_FLAG_LEAF
        assert table["nodes"][o2]["ch_flags"] & UDYNLINK_TRIE_FLAG_HAS_CHILD


class TestTrieEmit:
    @pytest.fixture()
    def sample_table(self):
        symbols = [("my_func", 0x0800), ("helper", 0x0A00)]
        return TrieFormat().build(symbols)

    @pytest.fixture()
    def header_path(self, tmp_path):
        return tmp_path / "host_syms.h"

    def test_output_contains_arrays(self, sample_table, header_path):
        TrieFormat().emit(sample_table, str(header_path))
        text = header_path.read_text()
        assert "g_host_trie_nodes[]" in text
        assert "g_host_trie_leaf_addrs[]" in text

    def test_output_contains_struct(self, sample_table, header_path):
        TrieFormat().emit(sample_table, str(header_path))
        text = header_path.read_text()
        assert "udynlink_trie_table_t g_host_sym_table" in text

    def test_includes_trie_header(self, sample_table, header_path):
        TrieFormat().emit(sample_table, str(header_path))
        text = header_path.read_text()
        assert '#include "udynlink_trie.h"' in text

    def test_include_guard(self, sample_table, header_path):
        TrieFormat().emit(sample_table, str(header_path))
        text = header_path.read_text()
        guard = "HOST_SYMS_HOST_SYMS_H"
        assert f"#ifndef {guard}" in text
        assert f"#define {guard}" in text


class TestTrieEmitEmptyTable:
    def test_produces_valid_file(self, tmp_path):
        table = TrieFormat().build([])
        header_path = tmp_path / "empty_syms.h"
        TrieFormat().emit(table, str(header_path))
        text = header_path.read_text()
        assert "#ifndef" in text
        assert "#endif" in text


@pytest.mark.integration
@skip_no_arm_gcc
class TestTrieEndToEnd:
    def test_generated_header_compiles(self, tmp_path):
        src = tmp_path / "test.c"
        elf = tmp_path / "test.elf"
        _write_test_c(src)
        _compile_arm_elf(str(src), str(elf))

        symbols = read_host_symbols(str(elf))
        table = TrieFormat().build(symbols)

        header_path = tmp_path / "host_syms.h"
        TrieFormat().emit(table, str(header_path))

        udynlink_include_dir = os.path.join(_REPO_ROOT, "udynlink")
        compile_cmd = [
            _ARM_GCC,
            "-mcpu=cortex-m4",
            "-mthumb",
            "-fsyntax-only",
            f"-I{udynlink_include_dir}",
            str(header_path),
        ]
        result = subprocess.run(compile_cmd, capture_output=True, text=True)
        assert result.returncode == 0, (
            f"Generated trie header failed to compile:\n"
            f"stdout: {result.stdout}\nstderr: {result.stderr}"
        )

    def test_consumer_compiles(self, tmp_path):
        src = tmp_path / "test.c"
        elf = tmp_path / "test.elf"
        _write_test_c(src)
        _compile_arm_elf(str(src), str(elf))

        symbols = read_host_symbols(str(elf))
        table = TrieFormat().build(symbols)

        header_path = tmp_path / "host_syms.h"
        TrieFormat().emit(table, str(header_path))

        consumer_path = tmp_path / "consumer.c"
        consumer_path.write_text(
            '#include "host_syms.h"\n'
            "void *test_lookup(void) {\n"
            '    return udynlink_resolve_trie_symbol(&g_host_sym_table, "my_func");\n'
            "}\n"
        )

        udynlink_include_dir = os.path.join(_REPO_ROOT, "udynlink")
        compile_cmd = [
            _ARM_GCC,
            "-mcpu=cortex-m4",
            "-mthumb",
            "-fsyntax-only",
            f"-I{udynlink_include_dir}",
            f"-I{tmp_path}",
            str(consumer_path),
        ]
        result = subprocess.run(compile_cmd, capture_output=True, text=True)
        assert result.returncode == 0, (
            f"Trie consumer C file failed to compile:\n"
            f"stdout: {result.stdout}\nstderr: {result.stderr}"
        )
