#!/usr/bin/env python3
"""
udynlink toolchain benchmark suite.

Measures compile (gcc) + mkmodule pipeline performance across module
complexities and optimization levels.  No QEMU needed — pure host-side
toolchain profiling.

Usage:
    python3 bench_toolchain.py                    # default: cortex-m4, all opt levels
    python3 bench_toolchain.py --target cortex-m33 # different target
    python3 bench_toolchain.py -O 0 -O s -O 2     # specific opt levels
    python3 bench_toolchain.py --iterations 5      # repeat for stable numbers
    python3 bench_toolchain.py --json results.json  # machine-readable output
"""

import argparse
import json
import os
import resource
import statistics
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Dict, List, Optional

REPO_ROOT = Path(__file__).resolve().parent.parent
SCRIPTS_DIR = REPO_ROOT / "scripts"
MODULES_DIR = Path(__file__).resolve().parent / "modules"

OPT_LEVELS = ["0", "s", "z", "2", "3"]
DEFAULT_TARGET = "cortex-m4"


@dataclass
class StageTimings:
    compile_s: float = 0.0
    link_s: float = 0.0
    process_s: float = 0.0
    total_s: float = 0.0


@dataclass
class SizeMetrics:
    bin_bytes: int = 0
    text_bytes: int = 0
    data_bytes: int = 0
    bss_bytes: int = 0
    header_bytes: int = 0
    reloc_bytes: int = 0
    symtab_bytes: int = 0


@dataclass
class SymCounts:
    exported: int = 0
    external: int = 0
    weak: int = 0
    local: int = 0


@dataclass
class RelocCounts:
    lot_entries: int = 0
    total_relocs: int = 0
    data_relocs: int = 0


@dataclass
class BenchResult:
    name: str
    target: str
    opt_level: str
    sources: List[str] = field(default_factory=list)
    extra_flags: List[str] = field(default_factory=list)
    iterations: int = 1
    timings: StageTimings = field(default_factory=StageTimings)
    timings_stdev: Optional[StageTimings] = None
    size: SizeMetrics = field(default_factory=SizeMetrics)
    syms: SymCounts = field(default_factory=SymCounts)
    relocs: RelocCounts = field(default_factory=RelocCounts)
    peak_rss_kb: int = 0
    success: bool = True
    error: str = ""


def _avg(lst):
    return statistics.mean(lst) if lst else 0.0


def _stdev(lst):
    return statistics.stdev(lst) if len(lst) > 1 else 0.0


def _parse_binary_sizes(bin_path: str) -> SizeMetrics:
    import struct
    m = SizeMetrics()
    try:
        with open(bin_path, "rb") as f:
            data = f.read()
        m.bin_bytes = len(data)
        if len(data) < 28 or data[:4] != b"UDLM":
            return m
        (lot_entries, total_relocs, _res, symtsize,
         codesize, datasize, bsssize) = struct.unpack_from("<HHHHIII", data, 8)
        m.text_bytes = codesize
        m.data_bytes = datasize
        m.bss_bytes = bsssize
        m.header_bytes = 28
        m.reloc_bytes = total_relocs * 8
        m.symtab_bytes = symtsize
    except Exception:
        pass
    return m


def _parse_elf_symbols(elf_path: str) -> SymCounts:
    from elftools.elf.elffile import ELFFile
    from elftools.elf.sections import SymbolTableSection
    sc = SymCounts()
    try:
        with open(elf_path, "rb") as f:
            elf = ELFFile(f)
            for section in elf.iter_sections():
                if not isinstance(section, SymbolTableSection):
                    continue
                for sym in section.iter_symbols():
                    b = sym["st_info"]["bind"]
                    if b == "STB_GLOBAL":
                        if sym["st_shndx"] == "SHN_UNDEF":
                            sc.external += 1
                        else:
                            sc.exported += 1
                    elif b == "STB_WEAK":
                        if sym["st_shndx"] == "SHN_UNDEF":
                            sc.external += 1
                        else:
                            sc.weak += 1
                    elif b == "STB_LOCAL":
                        sc.local += 1
    except Exception:
        pass
    return sc


def _parse_bin_relocs(bin_path: str) -> RelocCounts:
    import struct
    rc = RelocCounts()
    try:
        with open(bin_path, "rb") as f:
            data = f.read()
        if len(data) < 28 or data[:4] != b"UDLM":
            return rc
        lot_entries, total_relocs = struct.unpack_from("<HH", data, 10)
        rc.lot_entries = lot_entries
        rc.total_relocs = total_relocs
    except Exception:
        pass
    return rc


def _time_mkmodule_stages(
    sources: List[str],
    target: str,
    opt_level: str,
    extra_flags: List[str],
    work_dir: str,
) -> dict:
    """Run mkmodule in three separate invocations to get per-stage timings.

    Each invocation starts from a clean slate (different work dirs) so the
    stage times are independent and additive:
      - compile:  --stop-after-compile
      - link:     --stop-after-link   (re-compiles, stops after link)
      - full:     complete pipeline    (re-compiles + links + processes)
    process_s = full_total - link_total   (link includes compile)
    compile_s = compile_only
    link_s    = link_total - compile_s
    """
    base_src = sources[0]
    stem = Path(base_src).stem
    bin_path = os.path.join(work_dir, stem + ".bin")
    elf_path = os.path.join(work_dir, stem + ".elf")
    stages = {}

    common_args = [
        sys.executable, str(SCRIPTS_DIR / "mkmodule"),
        "--no-verbose",
        "--target", target,
        "-O", opt_level,
        "--bin-name", bin_path,
    ] + extra_flags + sources

    # Stage 1: compile only
    t0 = time.perf_counter()
    r = subprocess.run(
        common_args + ["--stop-after-compile"],
        capture_output=True, text=True, cwd=work_dir,
    )
    stages["compile_s"] = time.perf_counter() - t0
    if r.returncode != 0:
        return {**stages, "error": r.stderr[-500:] if r.stderr else "compile failed"}

    # Stage 2: compile + link
    t0 = time.perf_counter()
    r = subprocess.run(
        common_args + ["--stop-after-link"],
        capture_output=True, text=True, cwd=work_dir,
    )
    stages["link_total_s"] = time.perf_counter() - t0
    if r.returncode != 0:
        return {**stages, "error": r.stderr[-500:] if r.stderr else "link failed"}

    # Stage 3: full pipeline (compile + link + process)
    t0 = time.perf_counter()
    r = subprocess.run(
        common_args,
        capture_output=True, text=True, cwd=work_dir,
    )
    stages["total_s"] = time.perf_counter() - t0
    if r.returncode != 0:
        return {**stages, "error": r.stderr[-500:] if r.stderr else "process failed"}

    stages["link_s"] = max(0, stages["link_total_s"] - stages["compile_s"])
    stages["process_s"] = max(0, stages["total_s"] - stages["link_total_s"])
    stages["bin_path"] = bin_path
    stages["elf_path"] = elf_path
    return stages


BENCHMARKS = [
    {
        "name": "pure-fn",
        "sources": ["bench_pure_fn.c"],
        "extra_flags": [],
        "desc": "Single pure function, no imports/globals",
    },
    {
        "name": "multi-export",
        "sources": ["bench_multi_export.c"],
        "extra_flags": [],
        "desc": "20 exported functions with varying signatures",
    },
    {
        "name": "many-imports",
        "sources": ["bench_many_imports.c"],
        "extra_flags": [],
        "desc": "12 extern symbols (host API simulation)",
    },
    {
        "name": "globals",
        "sources": ["bench_globals.c"],
        "extra_flags": [],
        "desc": "19 volatile globals + pointer array",
    },
    {
        "name": "data-relocs",
        "sources": ["bench_data_relocs.c"],
        "extra_flags": [],
        "desc": "R_ARM_ABS32 heavy: function-pointer arrays + extern data refs",
    },
    {
        "name": "multi-file",
        "sources": ["bench_multi_file_main.c", "bench_vec_ops.c"],
        "extra_flags": [],
        "desc": "2-file module: vector ops library + consumer",
    },
    {
        "name": "cpp",
        "sources": ["bench_cpp.cpp"],
        "extra_flags": [],
        "desc": "C++: classes, static ctors, virtual dispatch",
    },
    {
        "name": "public-syms",
        "sources": ["bench_public_syms.c"],
        "extra_flags": ["--public-symbols", "public_add,public_mul,public_compute"],
        "desc": "Selective export: 3 public + 4 internal (gc-sections)",
    },
]


def run_benchmark(bench: dict, target: str, opt_level: str,
                  iterations: int, work_dir: str) -> BenchResult:
    result = BenchResult(
        name=bench["name"],
        target=target,
        opt_level=opt_level,
        sources=bench["sources"],
        extra_flags=bench.get("extra_flags", []),
        iterations=iterations,
    )

    src_paths = [str(MODULES_DIR / s) for s in bench["sources"]]
    extra = bench.get("extra_flags", [])

    all_compile = []
    all_link = []
    all_process = []
    all_total = []
    last_bin = None
    last_elf = None
    peak_rss = 0

    for i in range(iterations):
        bench_dir = os.path.join(work_dir, f"{bench['name']}_O{opt_level}_iter{i}")
        os.makedirs(bench_dir, exist_ok=True)

        for src in src_paths:
            dst = os.path.join(bench_dir, os.path.basename(src))
            if not os.path.exists(dst) or os.path.getmtime(src) > os.path.getmtime(dst):
                import shutil
                shutil.copy2(src, dst)

        local_srcs = [os.path.join(bench_dir, os.path.basename(s)) for s in bench["sources"]]

        rusage_before = resource.getrusage(resource.RUSAGE_CHILDREN)
        stages = _time_mkmodule_stages(local_srcs, target, opt_level, extra, bench_dir)
        rusage_after = resource.getrusage(resource.RUSAGE_CHILDREN)

        rss_delta = rusage_after.ru_maxrss - rusage_before.ru_maxrss
        if rss_delta > peak_rss or i == 0:
            peak_rss = rss_delta if rss_delta > 0 else rusage_after.ru_maxrss

        if "error" in stages:
            result.success = False
            result.error = stages["error"]
            return result

        all_compile.append(stages.get("compile_s", 0))
        all_link.append(stages.get("link_s", 0))
        all_process.append(stages.get("process_s", 0))
        all_total.append(stages.get("total_s", 0))
        last_bin = stages.get("bin_path")
        last_elf = stages.get("elf_path")

    result.timings = StageTimings(
        compile_s=_avg(all_compile),
        link_s=_avg(all_link),
        process_s=_avg(all_process),
        total_s=_avg(all_total),
    )

    if iterations > 1:
        result.timings_stdev = StageTimings(
            compile_s=_stdev(all_compile),
            link_s=_stdev(all_link),
            process_s=_stdev(all_process),
            total_s=_stdev(all_total),
        )

    if last_bin and os.path.exists(last_bin):
        result.size = _parse_binary_sizes(last_bin)
        result.relocs = _parse_bin_relocs(last_bin)

    if last_elf and os.path.exists(last_elf):
        result.syms = _parse_elf_symbols(last_elf)

    result.peak_rss_kb = peak_rss
    return result


def format_time(seconds: float) -> str:
    if seconds < 0.001:
        return f"{seconds * 1_000_000:.0f}us"
    elif seconds < 1.0:
        return f"{seconds * 1000:.1f}ms"
    else:
        return f"{seconds:.2f}s"


def format_bytes(b: int) -> str:
    if b < 1024:
        return f"{b}B"
    elif b < 1024 * 1024:
        return f"{b / 1024:.1f}KB"
    else:
        return f"{b / 1024 / 1024:.1f}MB"


def print_results_table(results: List[BenchResult]):
    # Console table
    hdr = (
        f"{'Benchmark':<16} {'Opt':>3} {'Compile':>10} {'Link':>10} "
        f"{'Process':>10} {'Total':>10} {'Bin':>8} {'Text':>8} "
        f"{'Data':>6} {'BSS':>6} {'Exp':>4} {'Ext':>4} {'LOT':>4} {'Relocs':>6}"
    )
    sep = "-" * len(hdr)
    print("\n" + hdr)
    print(sep)

    for r in results:
        if not r.success:
            print(f"{r.name:<16} {r.opt_level:>3}  FAILED: {r.error[:60]}")
            continue
        t = r.timings
        s = r.size
        row = (
            f"{r.name:<16} {r.opt_level:>3} {format_time(t.compile_s):>10} "
            f"{format_time(t.link_s):>10} {format_time(t.process_s):>10} "
            f"{format_time(t.total_s):>10} {format_bytes(s.bin_bytes):>8} "
            f"{format_bytes(s.text_bytes):>8} {s.data_bytes:>6} {s.bss_bytes:>6} "
            f"{r.syms.exported:>4} {r.syms.external:>4} "
            f"{r.relocs.lot_entries:>4} {r.relocs.total_relocs:>6}"
        )
        print(row)

    print(sep)


def print_summary_by_opt(results: List[BenchResult]):
    opts = sorted(set(r.opt_level for r in results))
    print("\n--- Aggregate by Optimization Level ---")
    print(f"{'Opt':>3} {'Avg Total':>10} {'Avg Compile':>12} {'Avg Bin Size':>12} {'Avg Text':>10}")
    print("-" * 52)
    for o in opts:
        subset = [r for r in results if r.opt_level == o and r.success]
        if not subset:
            continue
        avg_total = _avg([r.timings.total_s for r in subset])
        avg_compile = _avg([r.timings.compile_s for r in subset])
        avg_bin = _avg([r.size.bin_bytes for r in subset])
        avg_text = _avg([r.size.text_bytes for r in subset])
        print(f"{o:>3} {format_time(avg_total):>10} {format_time(avg_compile):>12} "
              f"{format_bytes(avg_bin):>12} {format_bytes(avg_text):>10}")
    print()


def print_summary_by_bench(results: List[BenchResult]):
    names = sorted(set(r.name for r in results))
    print("\n--- Aggregate by Benchmark (across all opt levels) ---")
    print(f"{'Benchmark':<16} {'Avg Total':>10} {'Min Bin':>10} {'Max Bin':>10} {'Min Text':>10} {'Max Text':>10}")
    print("-" * 72)
    for n in names:
        subset = [r for r in results if r.name == n and r.success]
        if not subset:
            continue
        avg_total = _avg([r.timings.total_s for r in subset])
        bins = [r.size.bin_bytes for r in subset]
        texts = [r.size.text_bytes for r in subset]
        print(f"{n:<16} {format_time(avg_total):>10} "
              f"{format_bytes(min(bins)):>10} {format_bytes(max(bins)):>10} "
              f"{format_bytes(min(texts)):>10} {format_bytes(max(texts)):>10}")
    print()


def results_to_dicts(results: List[BenchResult]) -> List[dict]:
    out = []
    for r in results:
        d = asdict(r)
        if r.timings_stdev is None:
            d["timings_stdev"] = None
        out.append(d)
    return out


def main():
    parser = argparse.ArgumentParser(
        description="udynlink toolchain benchmark suite"
    )
    parser.add_argument("--target", default=DEFAULT_TARGET,
                        help=f"Target CPU (default: {DEFAULT_TARGET})")
    parser.add_argument("-O", dest="opt_levels", action="append", default=None,
                        help="Optimization level(s) to test (repeatable)")
    parser.add_argument("--iterations", type=int, default=3,
                        help="Iterations per benchmark (default: 3)")
    parser.add_argument("--json", dest="json_path", default=None,
                        help="Write results as JSON to this path")
    parser.add_argument("--bench", action="append", default=None,
                        help="Run only named benchmark(s) (repeatable)")
    parser.add_argument("--list", action="store_true",
                        help="List available benchmarks and exit")
    args = parser.parse_args()

    if args.list:
        print("Available benchmarks:\n")
        for b in BENCHMARKS:
            print(f"  {b['name']:<16} {b['desc']}")
            print(f"  {'':16} sources: {', '.join(b['sources'])}")
            if b.get("extra_flags"):
                print(f"  {'':16} flags: {' '.join(b['extra_flags'])}")
            print()
        return

    opt_levels = args.opt_levels or OPT_LEVELS
    bench_names = args.bench or [b["name"] for b in BENCHMARKS]
    benches = [b for b in BENCHMARKS if b["name"] in bench_names]

    if not benches:
        print(f"No matching benchmarks. Available: {', '.join(b['name'] for b in BENCHMARKS)}")
        return 1

    print(f"udynlink toolchain benchmark")
    print(f"  target:     {args.target}")
    print(f"  opt levels: {', '.join(opt_levels)}")
    print(f"  iterations: {args.iterations}")
    print(f"  benchmarks: {', '.join(b['name'] for b in benches)}")

    with tempfile.TemporaryDirectory(prefix="udynlink_bench_") as work_dir:
        all_results: List[BenchResult] = []

        for bench in benches:
            for opt in opt_levels:
                print(f"  running {bench['name']} -O{opt} ...", end="", flush=True)
                t0 = time.perf_counter()
                result = run_benchmark(
                    bench, args.target, opt, args.iterations, work_dir
                )
                elapsed = time.perf_counter() - t0
                status = "OK" if result.success else "FAIL"
                print(f" {status} ({elapsed:.1f}s)")
                all_results.append(result)

    print_results_table(all_results)
    print_summary_by_opt(all_results)
    print_summary_by_bench(all_results)

    if args.json_path:
        with open(args.json_path, "w") as f:
            json.dump(results_to_dicts(all_results), f, indent=2)
        print(f"Results written to {args.json_path}")


if __name__ == "__main__":
    sys.exit(main() or 0)
