"""pytest tests for scripts/sm2module (JSON state chart -> UDLM image).

sm2module is the codegen-integration example (docs/codegen-integrations.md).
Needs the ARM cross-compiler; tests are skipped without it. protoc/nanopb
are NOT required.
"""

import json
import os
import subprocess
import sys

import pytest

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.abspath(os.path.join(_THIS_DIR, "..", ".."))
_SM2MODULE = os.path.join(_REPO_ROOT, "scripts", "sm2module")
import shutil

sys.path.insert(0, os.path.join(_REPO_ROOT, "scripts"))
from udynlink_parser import parse_module  # noqa: E402

pytestmark = pytest.mark.skipif(
    shutil.which("arm-none-eabi-gcc") is None,
    reason="arm-none-eabi-gcc not available",
)


ACTION_CHART = {
    "name": "door",
    "initial": "closed",
    "states": ["closed", "open", "locked"],
    "events": ["push", "lock", "unlock"],
    "transitions": [
        {"from": "closed", "event": "push", "to": "open"},
        {"from": "open", "event": "lock", "to": "locked", "action": "lock"},
        {"from": "locked", "event": "unlock", "to": "open",
         "action": "unlock"},
    ],
}

PURE_CHART = {
    "name": "pure",
    "initial": "idle",
    "states": ["idle", "busy"],
    "events": ["start", "stop"],
    "transitions": [
        {"from": "idle", "event": "start", "to": "busy"},
        {"from": "busy", "event": "stop", "to": "idle"},
    ],
}


def _run(tmp_path, chart, *extra):
    tmp_path.mkdir(parents=True, exist_ok=True)
    p = tmp_path / (chart["name"] + ".json")
    p.write_text(json.dumps(chart))
    out = tmp_path / "out"
    out.mkdir()
    res = subprocess.run(
        [sys.executable, _SM2MODULE, "--out-dir", str(out), *extra, str(p)],
        capture_output=True, text=True)
    assert res.returncode == 0, res.stdout + res.stderr
    return out


def _exports(mod):
    return sorted(s.name for s in mod.symbols
                  if s.type_name == "EXPORTED" and s.name)


def _externs(mod):
    return sorted(s.name for s in mod.symbols
                  if s.type_name == "EXTERN")


def test_exports_and_action_externs(tmp_path):
    """A chart with actions exports the 4-function ABI and exactly one
    extern per distinct action name."""
    out = _run(tmp_path, ACTION_CHART)
    mod = parse_module((out / "door_mod.bin").read_bytes())
    assert _exports(mod) == ["door_event_id", "door_event_name",
                             "door_state_name", "door_step"]
    assert _externs(mod) == ["door_action_lock", "door_action_unlock"]


def test_action_free_chart_is_self_contained(tmp_path):
    """No actions -> zero externs: the module is fully self-contained."""
    out = _run(tmp_path, PURE_CHART)
    mod = parse_module((out / "pure_mod.bin").read_bytes())
    assert _exports(mod) == ["pure_event_id", "pure_event_name",
                             "pure_state_name", "pure_step"]
    assert _externs(mod) == []


def test_size_budget(tmp_path):
    """The state-machine module stays tiny (fixed overhead dominated)."""
    out = _run(tmp_path, ACTION_CHART)
    data = (out / "door_mod.bin").read_bytes()
    h = parse_module(data).header
    assert len(data) < 1024
    assert h.num_lot * 4 + h.data_size + h.bss_size < 128  # XIP RAM budget


def test_contract_header_contents(tmp_path):
    """The contract header carries state/event constants and, with actions,
    the hook prototypes the host must implement."""
    out = _run(tmp_path, ACTION_CHART)
    api = (out / "door_contract.h").read_text()
    assert "#define DOOR_STATE_CLOSED 0u" in api
    assert "#define DOOR_EVENT_UNLOCK 2u" in api
    assert "void door_action_lock(void *ctx);" in api
    # self-contained chart: no hook prototypes
    out = _run(tmp_path / "p2", PURE_CHART)
    api = (out / "pure_contract.h").read_text()
    assert "void pure_action_" not in api
    assert "resolve_symbol" not in api

def test_export_prefix_and_bare_names(tmp_path):
    out = _run(tmp_path, PURE_CHART, "--export-prefix", "sm")
    mod = parse_module((out / "pure_mod.bin").read_bytes())
    assert "sm_step" in _exports(mod)
    out = _run(tmp_path / "p2", PURE_CHART, "--export-prefix", "")
    mod = parse_module((out / "pure_mod.bin").read_bytes())
    assert "step" in _exports(mod)


def test_duplicate_transition_rejected(tmp_path):
    chart = json.loads(json.dumps(ACTION_CHART))
    chart["transitions"].append({"from": "closed", "event": "push",
                                 "to": "locked"})
    p = tmp_path / "chart.json"
    p.write_text(json.dumps(chart))
    res = subprocess.run([sys.executable, _SM2MODULE, str(p)],
                         capture_output=True, text=True)
    assert res.returncode != 0
    assert "duplicate transition" in res.stderr


def test_unknown_state_rejected(tmp_path):
    chart = json.loads(json.dumps(PURE_CHART))
    chart["transitions"][0]["to"] = "nowhere"
    p = tmp_path / "chart.json"
    p.write_text(json.dumps(chart))
    res = subprocess.run([sys.executable, _SM2MODULE, str(p)],
                         capture_output=True, text=True)
    assert res.returncode != 0
    assert "unknown state" in res.stderr


def test_positional_must_be_json(tmp_path):
    res = subprocess.run(
        [sys.executable, _SM2MODULE, "--struct", "X", "-O", "s", "c.json"],
        capture_output=True, text=True)
    assert res.returncode != 0
    assert "does not look like a .json chart" in res.stderr
