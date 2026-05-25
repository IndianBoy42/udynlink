#!/usr/bin/env python3
"""List supported udynlink targets from the target database."""

from targets import TARGETS

def list_targets():
    print(f"{'Target':<20} {'mcpu':<12} {'arch':<12} {'fpu':<12} {'float_abi'}")
    print("-" * 70)
    for name, info in TARGETS.items():
        fpu = info.get("fpu", "-") or "-"
        print(f"{name:<20} {info['mcpu']:<12} {info['arch']:<12} {fpu:<12} {info['float_abi']}")

def show_target_info(target_name):
    import json
    t = TARGETS.get(target_name)
    if t:
        print(json.dumps(t, indent=2))
    else:
        print(f"Unknown target: {target_name}")
        print(f"Supported targets: {', '.join(TARGETS.keys())}")

if __name__ == "__main__":
    import sys
    if len(sys.argv) > 1:
        show_target_info(sys.argv[1])
    else:
        list_targets()
