"""Read results/results.csv and results/bench.csv, write plots and results/summary.txt."""
import csv, os, statistics as st
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
RES = os.path.join(ROOT, "results")

rows = list(csv.DictReader(open(os.path.join(RES, "results.csv"))))
groups = defaultdict(list)   # (policy, load, overest) -> rows
for r in rows:
    groups[(r["policy"], float(r["load"]), float(r["overest"]))].append(r)

def stat(policy, load, over, col):
    vals = [float(r[col]) for r in groups[(policy, load, over)]]
    return st.mean(vals), (st.stdev(vals) if len(vals) > 1 else 0.0)

loads = sorted({k[1] for k in groups})
overs = sorted({k[2] for k in groups})
REF_OVER, REF_LOAD = 3.0, 0.9
nreps = len(groups[("FCFS", REF_LOAD, REF_OVER)])

def vs_load(col, ylabel, fname, log=False):
    plt.figure(figsize=(6, 4))
    for p, mk in (("FCFS", "o-"), ("EASY", "s-")):
        m = [stat(p, l, REF_OVER, col)[0] for l in loads]
        s = [stat(p, l, REF_OVER, col)[1] for l in loads]
        plt.errorbar(loads, m, yerr=s, fmt=mk, capsize=3, label=p)
    if log: plt.yscale("log")
    plt.xlabel("offered load"); plt.ylabel(ylabel)
    plt.title(f"{ylabel} vs load (mean +/- sd, n={nreps})", fontsize=10)
    plt.legend(); plt.grid(alpha=0.3); plt.tight_layout()
    plt.savefig(os.path.join(RES, fname), dpi=150); plt.close()

vs_load("avg_wait", "average wait", "wait_vs_load.png", log=True)
vs_load("util", "utilization", "util_vs_load.png")

# effect of walltime overestimation on EASY
plt.figure(figsize=(6, 4))
for col, mk, lab in (("avg_wait", "o-", "average wait"), ("max_wait", "s-", "max wait")):
    m = [stat("EASY", REF_LOAD, o, col)[0] for o in overs]
    s = [stat("EASY", REF_LOAD, o, col)[1] for o in overs]
    plt.errorbar(overs, m, yerr=s, fmt=mk, capsize=3, label=f"EASY {lab}")
plt.axhline(stat("FCFS", REF_LOAD, REF_OVER, "avg_wait")[0], ls="--", c="gray", label="FCFS average wait")
plt.xlabel("max overestimation factor (req = runtime * U(1, x))"); plt.ylabel("time units")
plt.title(f"Effect of walltime overestimation at load {REF_LOAD}", fontsize=10)
plt.legend(); plt.grid(alpha=0.3); plt.tight_layout()
plt.savefig(os.path.join(RES, "overestimation_effect.png"), dpi=150); plt.close()

# speedup of the parallel sweep
lines = []
bench_path = os.path.join(RES, "bench.csv")
if os.path.exists(bench_path):
    b = list(csv.DictReader(open(bench_path)))
    th = [int(r["threads"]) for r in b]; sp = [float(r["speedup"]) for r in b]
    plt.figure(figsize=(5, 4))
    plt.plot(th, sp, "o-", label="measured"); plt.plot(th, th, "--", c="gray", label="ideal")
    plt.xlabel("threads"); plt.ylabel("speedup vs 1 thread")
    plt.title("Parallel parameter sweep speedup", fontsize=10)
    plt.legend(); plt.grid(alpha=0.3); plt.tight_layout()
    plt.savefig(os.path.join(RES, "speedup.png"), dpi=150); plt.close()
    lines.append("Speedup (threads: speedup): " + ", ".join(f"{t}: {s:.2f}x" for t, s in zip(th, sp)))

# numbers to quote in the README
fw, fu, fm = (stat("FCFS", REF_LOAD, REF_OVER, c)[0] for c in ("avg_wait", "util", "max_wait"))
ew, eu, em = (stat("EASY", REF_LOAD, REF_OVER, c)[0] for c in ("avg_wait", "util", "max_wait"))
lines += [
    f"At load {REF_LOAD}, overestimation <= {REF_OVER:g}x, {nreps} replications:",
    f"  average wait   FCFS {fw:.0f}  EASY {ew:.0f}  ({100*(1-ew/fw):.0f}% lower)",
    f"  utilization    FCFS {100*fu:.1f}%  EASY {100*eu:.1f}%",
    f"  max wait       FCFS {fm:.0f}  EASY {em:.0f}",
    "EASY average / max wait by overestimation factor at that load:",
] + [f"  {o:g}x: avg {stat('EASY', REF_LOAD, o, 'avg_wait')[0]:.0f}, max {stat('EASY', REF_LOAD, o, 'max_wait')[0]:.0f}" for o in overs]
open(os.path.join(RES, "summary.txt"), "w").write("\n".join(lines) + "\n")
print("\n".join(lines))
