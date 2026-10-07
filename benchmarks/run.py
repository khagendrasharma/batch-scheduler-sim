"""One command to reproduce everything: compile, verify, run sweep and benchmark, plot.
Usage:  python benchmarks/run.py [threads_for_sweep]
"""
import os, subprocess, sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
os.chdir(ROOT)
os.makedirs("results", exist_ok=True)
exe = os.path.join(".", "sim.exe" if os.name == "nt" else "sim")
threads = sys.argv[1] if len(sys.argv) > 1 else "4"

def run(cmd, out=None):
    print(">", " ".join(cmd), flush=True)
    if out:
        with open(out, "w") as f:
            subprocess.run(cmd, stdout=f, check=True)
    else:
        subprocess.run(cmd, check=True)

cxxflags = ["-O2", "-std=c++17"]
if os.name != "nt":
    cxxflags.append("-pthread")          # MinGW-w64 (no pthreads) cannot link -pthread
run(["g++"] + cxxflags + ["-o", exe, os.path.join("src", "sim.cpp")])
run([exe, "demo"])                                           # hand-checkable example
run([exe, "sweep", threads, "20", "2000"], "results/results.csv")
# run([exe, "bench", "40", "3000"], "results/bench.csv")     # no std::thread on this toolchain
run([sys.executable, os.path.join("benchmarks", "plot.py")])
