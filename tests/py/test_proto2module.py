"""pytest tests for the scripts/proto2module pipeline (proto -> UDLM image).

The pipeline needs protoc, the nanopb generator plugin, and the vendored
nanopb runtime (third_party/nanopb) in addition to the ARM cross-compiler,
so all tests here are marked integration and skipped when a piece is missing.
"""

import os
import shutil
import subprocess
import sys

import pytest

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.abspath(os.path.join(_THIS_DIR, "..", ".."))
_PROTO2MODULE = os.path.join(_REPO_ROOT, "scripts", "proto2module")
_NANOPB_DIR = os.path.join(_REPO_ROOT, "third_party", "nanopb")

sys.path.insert(0, os.path.join(_REPO_ROOT, "scripts"))
from udynlink_parser import parse_module  # noqa: E402

_PROTOC = (os.environ.get("UDYNLINK_PROTOC")
           or shutil.which("protoc"))
_PLUGIN = (os.environ.get("UDYNLINK_NANOPB_PLUGIN")
           or shutil.which("protoc-gen-nanopb")
           or os.path.join(_REPO_ROOT, ".venv", "bin", "protoc-gen-nanopb"))

skip_missing_tools = pytest.mark.skipif(
    _PROTOC is None or not os.path.isfile(_PLUGIN)
    or not os.path.isfile(os.path.join(_NANOPB_DIR, "pb.h"))
    or shutil.which("arm-none-eabi-gcc") is None,
    reason="protoc/nanopb/arm-none-eabi-gcc not all available",
)

SIMPLE_PROTO = """\
syntax = "proto3";
import "nanopb.proto";
message Telemetry {
  uint32 seq = 1;
  int32 value = 2;
  string label = 3 [(nanopb).max_size = 8];
}
"""

TWO_MSG_PROTO = """\
syntax = "proto3";
import "nanopb.proto";
message Alpha { uint32 a = 1; }
message Beta { string b = 1 [(nanopb).max_size = 4]; }
"""


def _run_pipeline(tmp_path, proto_text, *extra):
    proto = tmp_path / "telemetry.proto"
    proto.write_text(proto_text)
    out = tmp_path / "out"
    out.mkdir()
    cmd = [sys.executable, _PROTO2MODULE, "--out-dir", str(out),
           "--nanopb-dir", _NANOPB_DIR, str(proto)] + list(extra)
    res = subprocess.run(cmd, capture_output=True, text=True)
    assert res.returncode == 0, res.stdout + res.stderr
    return out


@skip_missing_tools
class TestProto2Module:
    def test_single_struct_exports_parse_write(self, tmp_path):
        out = _run_pipeline(tmp_path, SIMPLE_PROTO)
        mod = parse_module((out / "telemetry_mod.bin").read_bytes())
        names = {s.name: s for s in mod.symbols}
        assert names["parse"].type_name == "EXPORTED"
        assert names["write"].type_name == "EXPORTED"
        # Only the two wrappers are exported; schema tables are demoted.
        exported = [s for s in mod.symbols
                    if s.type_name == "EXPORTED" and s.name]
        assert sorted(n.name for n in exported) == ["parse", "write"]
        externs = sorted(s.name for s in mod.symbols
                         if s.type_name == "EXTERN")
        assert externs == ["pb_decode", "pb_encode",
                           "pb_istream_from_buffer", "pb_ostream_from_buffer"]

    def test_single_struct_size_budget(self, tmp_path):
        """1 module == 1 struct must stay small (fixed overhead dominated)."""
        out = _run_pipeline(tmp_path, SIMPLE_PROTO)
        data = (out / "telemetry_mod.bin").read_bytes()
        mod = parse_module(data)
        assert len(data) < 1024
        h = mod.header
        lot_ram = h.num_lot * 4
        assert lot_ram + h.data_size + h.bss_size < 128  # XIP RAM budget

    def test_multi_struct_named_exports(self, tmp_path):
        out = _run_pipeline(tmp_path, TWO_MSG_PROTO,
                            "--struct", "Alpha", "--struct", "Beta")
        mod = parse_module((out / "telemetry_mod.bin").read_bytes())
        exported = sorted(s.name for s in mod.symbols
                          if s.type_name == "EXPORTED" and s.name)
        assert exported == ["parse_alpha", "parse_beta",
                            "write_alpha", "write_beta"]

    def test_export_prefix_disambiguates_modules(self, tmp_path):
        """--export-prefix gives each codec module unique export names so
        several modules can coexist in the deps layer (which resolves by
        bare name, first match wins)."""
        out = _run_pipeline(tmp_path, SIMPLE_PROTO, "--export-prefix", "tele")
        mod = parse_module((out / "telemetry_mod.bin").read_bytes())
        exported = sorted(s.name for s in mod.symbols
                          if s.type_name == "EXPORTED" and s.name)
        assert exported == ["tele_parse", "tele_write"]

    def test_ambiguous_proto_requires_struct(self, tmp_path):
        proto = tmp_path / "multi.proto"
        proto.write_text(TWO_MSG_PROTO)
        out = tmp_path / "out"
        out.mkdir()
        cmd = [sys.executable, _PROTO2MODULE, "--out-dir", str(out),
               "--nanopb-dir", _NANOPB_DIR, str(proto)]
        res = subprocess.run(cmd, capture_output=True, text=True)
        assert res.returncode != 0
        assert "--struct" in res.stderr
