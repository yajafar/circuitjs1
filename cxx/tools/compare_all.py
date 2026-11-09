#!/usr/bin/env python3
# Check every example circuit and auto-test, in parallel, in one of two ways:
#
#   tools/compare_all.py [--steps N] [--jobs J] [--tol RTOL] [circuit.txt ...]
#       TS vs C++: runs tools/compare.py on each circuit.  Environment variables (ENGINE,
#       SOLVER, SIMTRACE_BUILD) pass through, so `ENGINE=ref tools/compare_all.py` checks the
#       reference engine against TS and the default checks the fast one.
#
#   tools/compare_all.py --engines [--steps N] [--jobs J] [circuit.txt ...]
#       C++ fast engine vs C++ reference engine, same build: the traces must be byte-identical.
#       This is the gate for engine optimizations; it needs no Node and runs much faster.
#
# Prints one line per circuit that isn't "ok" and a summary; exits 1 on any mismatch.
#
# Known exception (TS vs C++ on Windows only): triodeamp.txt differs in the last bit, because
# Node's Math.pow on Windows x64 differs from V8's portable C pow (third_party/v8) in the last
# bit for about 9% of inputs.

import argparse
import glob
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def simtrace_path():
    build = os.environ.get("SIMTRACE_BUILD", "build-win" if os.name == "nt" else "build-mac")
    return os.path.join(ROOT, build, "simtrace")


def compare_ts(c, a):
    cmd = [sys.executable, os.path.join(HERE, "compare.py")]
    if a.tol:
        cmd += ["--tol", a.tol]
    cmd += [c, str(a.steps)]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    out = (r.stdout + r.stderr).strip()
    return out.splitlines()[0] if out else "empty (no output)"


def compare_engines(c, a):
    def trace(engine):
        env = dict(os.environ, ENGINE=engine)
        r = subprocess.run([simtrace_path(), c, str(a.steps)], cwd=ROOT, capture_output=True, text=True, env=env)
        return r.stdout.splitlines(), r.stderr
    ref, ref_err = trace("ref")
    fast, _ = trace("fast")
    if "unrecognized" in ref_err or "unsupported" in ref_err:
        return "skip (unported elements)"
    if not ref:
        return "empty (reference engine printed nothing)"
    for n, (x, y) in enumerate(zip(ref, fast)):
        if x != y:
            # find the first differing column, to say which node or element it is
            xs, ys = x.split(), y.split()
            k = next((i for i, (p, q) in enumerate(zip(xs, ys)) if p != q), min(len(xs), len(ys)))
            return f"FAIL step {n}, column {k}: ref {xs[k] if k < len(xs) else '-'} vs fast {ys[k] if k < len(ys) else '-'}"
    if len(ref) != len(fast):
        return f"FAIL ref ran {len(ref)} steps, fast ran {len(fast)}"
    return "ok"


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--engines", action="store_true", help="compare C++ fast vs reference engine")
    p.add_argument("--steps", type=int, default=2000)
    p.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    p.add_argument("--tol", type=str, default=None)
    p.add_argument("circuits", nargs="*")
    a = p.parse_args()

    circuits = a.circuits or sorted(glob.glob(os.path.join(ROOT, "..", "ts", "public", "circuits", "*.txt")) +
                                    [f for f in glob.glob(os.path.join(ROOT, "..", "auto-tests", "*.txt"))
                                     if os.path.basename(f) != "tests.txt"])
    check = compare_engines if a.engines else compare_ts

    counts = {}
    with ThreadPoolExecutor(a.jobs) as pool:
        for c, result in zip(circuits, pool.map(lambda c: check(c, a), circuits)):
            word = result.split()[0]
            counts[word] = counts.get(word, 0) + 1
            if word != "ok":
                print(f"{os.path.basename(c)}: {result}")
    print("summary: " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())))
    sys.exit(0 if set(counts) <= {"ok", "skip"} else 1)


if __name__ == "__main__":
    main()
