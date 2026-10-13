#!/usr/bin/env python3
"""elfconv lifted-IR size iteration helper.

Subcommands:
  lift [NAME]          Lift sm64ex ELF with worktree elflift; record size/time; print delta.
  sizes BC [TOPN]      Bitcode size + instruction histogram + top functions (bounded output).
  opttest "PASS,..."   Run opt with comma-separated passes on baseline.bc; report size delta.
  failures save        Run ctest, save failing test names to state.
  failures cmp         Run ctest, compare failures vs saved baseline (new/fixed only).

State: .elfconv-logs/irsize-state.json.  Ground-truth artifact: .elfconv-logs/sm64-baseline.bc
"""
import json, os, re, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGS = os.path.join(ROOT, ".elfconv-logs")
STATE = os.path.join(LOGS, "irsize-state.json")
ELF = "/home/brian/Git/elfconv/examples/sm64ex/build/us_pc_pie/sm64.us.f3dex2e"
LIFTER = os.path.join(ROOT, "build/src/lifter/elflift")
LLVM = "/home/brian/.local/opt/llvm-23.1.2/LLVM-23.1.2-Linux-X64/bin"
BUILD = os.path.join(ROOT, "build")


def state():
    if os.path.exists(STATE):
        with open(STATE) as f:
            return json.load(f)
    return {}


def save_state(s):
    with open(STATE, "w") as f:
        json.dump(s, f, indent=1)


def lift(name):
    out = os.path.join(LOGS, f"{name}.bc")
    t0 = time.time()
    r = subprocess.run([LIFTER, "--arch", "i386", "--target_arch", "emscripten32",
                        "--target_elf", ELF, "--bc_out", out, "--norm_mode", "1",
                        "--fork_emulation", "0", "--float_exception", "0"],
                       capture_output=True, text=True)
    dt = time.time() - t0
    if r.returncode != 0:
        print("LIFT FAILED:", r.stderr[-2000:])
        sys.exit(1)
    sz = os.path.getsize(out)
    s = state()
    base = s.get("baseline_bytes")
    delta = f"  ({sz - base:+d} vs baseline {base})" if base else "  (no baseline recorded)"
    s.setdefault("lifts", {})[name] = {"bytes": sz, "seconds": round(dt, 2)}
    save_state(s)
    print(f"{name}: {sz:,} bytes in {dt:.1f}s{delta}")


def sizes(bc, topn=10):
    sz = os.path.getsize(bc)
    ll = bc[:-3] + ".tmp.ll"
    subprocess.run([f"{LLVM}/llvm-dis", bc, "-o", ll], check=True)
    hist = {}
    funcs = []
    cur = None
    fn_re = re.compile(r'^define [^@]*@([\w.$-]+)\(')
    ins_re = re.compile(r'^  ?(?:tail )?(?:volatile )?([a-z][\w.]*)')
    with open(ll) as f:
        for line in f:
            m = fn_re.match(line)
            if m:
                cur = {"name": m.group(1), "ins": 0}
                funcs.append(cur)
                continue
            if line.startswith("}") and cur:
                cur = None
                continue
            if line.lstrip().startswith(";"):
                continue
            m = ins_re.match(line)
            if m:
                hist[m.group(1)] = hist.get(m.group(1), 0) + 1
                if cur:
                    cur["ins"] += 1
    os.unlink(ll)
    total = sum(hist.values())
    print(f"{os.path.basename(bc)}: {sz:,} bytes, {total:,} instructions")
    for op, n in sorted(hist.items(), key=lambda kv: -kv[1])[:topn]:
        print(f"  {op:22s} {n:9,}  {100 * n / total:5.1f}%")
    funcs.sort(key=lambda f: -f["ins"])
    print("top functions by instructions:")
    for f in funcs[:topn]:
        print(f"  {f['ins']:9,}  {f['name'][:60]}")


def opttest(pipeline):
    src = os.path.join(LOGS, "sm64-baseline.bc")
    out = os.path.join(LOGS, "opttest.bc")
    passes = [p.strip() for p in pipeline.split(",") if p.strip()]
    t0 = time.time()
    r = subprocess.run([f"{LLVM}/opt", "-passes", pipeline, "-S", "-o", "/dev/null",
                        "-disable-output", src], capture_output=True, text=True)
    if r.returncode != 0:
        print("OPT FAILED:", r.stderr[-1500:])
        sys.exit(1)
    t_opt = time.time() - t0
    subprocess.run([f"{LLVM}/opt", "-passes", pipeline, src, "-o", out], check=True,
                   capture_output=True)
    sz = os.path.getsize(out)
    base = os.path.getsize(src)
    print(f"passes [{pipeline}]: {sz:,} bytes ({sz - base:+d}, {100 * (sz - base) / base:+.2f}%) "
          f"opt {t_opt:.1f}s")


def run_ctest():
    r = subprocess.run(["ctest", "--test-dir", BUILD, "-j6", "--timeout", "600"],
                       capture_output=True, text=True)
    failed = sorted(set(re.findall(r"Test\s+#\d+: ([\w./+-]+) \*{2,}", r.stdout)))
    return r.returncode, failed


def failures(cmd):
    s = state()
    if cmd == "import":  # parse a ctest summary ("The following tests FAILED" block)
        failed = []
        in_block = False
        with open(sys.argv[2]) as f:
            for line in f:
                if line.startswith("The following tests FAILED"):
                    in_block = True
                elif in_block:
                    m = re.match(r"\s+\d+ - ([\w./+-]+)", line)
                    if m:
                        failed.append(m.group(1))
                    elif line.strip() and not line.startswith("\t"):
                        in_block = False
        failed = sorted(set(failed))
        s["baseline_failures"] = failed
        save_state(s)
        print(f"imported {len(failed)} baseline failures")
        return
    if cmd == "save":
        rc, failed = run_ctest()
        s["baseline_failures"] = failed
        s["baseline_ctest_rc"] = rc
        save_state(s)
        print(f"saved {len(failed)} baseline failures (ctest rc={rc})")
        for f in failed:
            print(" ", f)
        return
    base = set(s.get("baseline_failures", []))
    rc, failed = run_ctest()
    new = sorted(set(failed) - base)
    fixed = sorted(base - set(failed))
    print(f"ctest rc={rc}; failing now: {len(failed)}; baseline: {len(base)}")
    if new:
        print("NEW FAILURES (regressions):")
        for f in new:
            print("  !", f)
    if fixed:
        print("fixed since baseline:")
        for f in fixed:
            print("  +", f)
    if not new:
        print("no new regressions")
    sys.exit(1 if new else 0)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    cmd = sys.argv[1]
    if cmd == "lift":
        lift(sys.argv[2] if len(sys.argv) > 2 else f"lift-{int(time.time())}")
    elif cmd == "sizes":
        sizes(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 10)
    elif cmd == "opttest":
        opttest(sys.argv[2])
    elif cmd == "failures":
        failures(sys.argv[2])
    else:
        print(__doc__)
        sys.exit(2)
