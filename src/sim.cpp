// sim.cpp - HPC batch-scheduler simulator: FCFS vs EASY backfilling.
//
// Modes:
//   sim demo                          3-job hand-checkable example
//   sim sweep [threads] [reps] [jobs] parameter sweep, CSV on stdout
//   sim bench [reps] [jobs]           time the sweep on 1,2,4,8 threads, CSV on stdout
//
// Limitations: non-preemptive, rigid jobs (all cores at once), no memory/network
// model, synthetic workload, a single homogeneous cluster.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>
#if !defined(__GLIBCXX__) || defined(_GLIBCXX_HAS_GTHREADS)
#include <thread>
#define HAVE_THREADS 1
#else
#define HAVE_THREADS 0   // e.g. MinGW with win32 thread model: run single-threaded
#endif
using namespace std;

// ---------------------------------------------------------------- data types
struct Job {
    int id;
    double submit;   // arrival time
    double runtime;  // ACTUAL runtime (unknown to the scheduler)
    double req;      // REQUESTED walltime (what the scheduler plans with), req >= runtime
    int cores;       // cores needed, all at once
    double start = 0, end = 0;
};
enum Policy { FCFS = 0, EASY = 1 };
const char* POLICY_NAME[] = {"FCFS", "EASY"};

struct Metrics { double avg_wait, max_wait, bsld, util; };

// ------------------------------------------------------------- workload model
// load = offered load (average core-time demanded / core-time available).
// overest = max overestimation factor: req = runtime * U(1, overest).
vector<Job> generate(int n, unsigned seed, double load, double overest, int total_cores) {
    mt19937_64 rng(seed);
    lognormal_distribution<double> rt(4.0, 1.2);               // median ~55 time units
    discrete_distribution<int> csel({30, 25, 20, 12, 8, 5});   // 1,2,4,8,16,32 cores
    uniform_real_distribution<double> over(1.0, overest);
    vector<Job> jobs(n);
    double work = 0;
    for (int i = 0; i < n; i++) {
        Job& j = jobs[i];
        j.id = i;
        j.cores = min(total_cores, 1 << csel(rng));
        j.runtime = min(5000.0, max(1.0, rt(rng)));
        j.req = j.runtime * over(rng);
        work += j.runtime * j.cores;
    }
    double gap = (work / n) / (load * total_cores);            // mean inter-arrival time
    exponential_distribution<double> ia(1.0 / gap);
    double t = 0;
    for (auto& j : jobs) { t += ia(rng); j.submit = t; }
    return jobs;                                               // sorted by submit
}

// ------------------------------------------------------------------ simulator
Metrics simulate(vector<Job> jobs, Policy pol, int total_cores, bool verbose = false) {
    const double INF = 1e300;
    int free_cores = total_cores;
    double now = 0;
    size_t next = 0;                 // next job not yet submitted
    vector<int> waiting, running;    // indices into jobs; waiting is in FCFS order

    auto start_job = [&](size_t pos) {
        int i = waiting[pos];
        waiting.erase(waiting.begin() + pos);
        jobs[i].start = now;
        jobs[i].end = now + jobs[i].runtime;
        free_cores -= jobs[i].cores;
        running.push_back(i);
        if (verbose) printf("  t=%.1f start job %d (free_cores=%d)\n", now, jobs[i].id, free_cores);
    };

    while (next < jobs.size() || !running.empty()) {
        // 1. advance the clock to the next event (submit or end)
        double t_sub = next < jobs.size() ? jobs[next].submit : INF;
        double t_end = INF;
        for (int i : running) t_end = min(t_end, jobs[i].end);
        now = min(t_sub, t_end);

        // 2. ends first (free cores), then submits
        for (size_t k = 0; k < running.size();) {
            int i = running[k];
            if (jobs[i].end <= now) {
                free_cores += jobs[i].cores;
                if (verbose) printf("  t=%.1f end job %d (free_cores=%d)\n", now, jobs[i].id, free_cores);
                running[k] = running.back();
                running.pop_back();
            } else k++;
        }
        while (next < jobs.size() && jobs[next].submit <= now) waiting.push_back((int)next++);

        // 3a. FCFS rule: start the head of the queue while it fits
        while (!waiting.empty() && jobs[waiting[0]].cores <= free_cores) start_job(0);

        // 3b. EASY backfilling: head is blocked; let later jobs jump ahead if they
        //     cannot delay the head's promised start (the "shadow time").
        if (pol == EASY && !waiting.empty()) {
            int head_cores = jobs[waiting[0]].cores;
            vector<pair<double, int>> pred;   // (predicted end = start + REQUESTED walltime, cores)
            for (int i : running) pred.push_back({jobs[i].start + jobs[i].req, jobs[i].cores});
            sort(pred.begin(), pred.end());
            int avail = free_cores;
            double shadow = INF;
            for (auto& p : pred) {
                avail += p.second;
                if (avail >= head_cores) { shadow = p.first; break; }
            }
            int extra = avail - head_cores;   // cores spare at shadow time after head starts
            for (size_t pos = 1; pos < waiting.size();) {
                const Job& j = jobs[waiting[pos]];
                bool fits = j.cores <= free_cores;
                bool ends_before_shadow = now + j.req <= shadow;
                if (fits && (ends_before_shadow || j.cores <= extra)) {
                    if (!ends_before_shadow) extra -= j.cores;
                    start_job(pos);           // erases position pos, so do not advance pos
                } else pos++;
            }
        }
    }

    // metrics
    const double TAU = 10.0;
    double tw = 0, mw = 0, tb = 0, core_time = 0, makespan = 0;
    for (const Job& j : jobs) {
        double wait = j.start - j.submit;
        tw += wait; mw = max(mw, wait);
        tb += max(1.0, (wait + j.runtime) / max(j.runtime, TAU));
        core_time += j.runtime * j.cores;
        makespan = max(makespan, j.end);
    }
    int n = (int)jobs.size();
    double span = makespan - jobs.front().submit;
    return {tw / n, mw, tb / n, core_time / (total_cores * span)};
}

// -------------------------------------------------------------- parameter sweep
struct Task { double load, overest; unsigned seed; };

vector<Task> make_tasks(int reps) {
    vector<Task> tasks;
    for (double load : {0.5, 0.6, 0.7, 0.8, 0.9, 0.95})
        for (double over : {1.0, 2.0, 3.0, 5.0, 10.0})
            for (int r = 0; r < reps; r++) tasks.push_back({load, over, (unsigned)(1000 + r)});
    return tasks;
}

// Each task is independent (own workload, own simulators), so threads share nothing
// except the atomic task counter and write to distinct slots of `res`: no locks needed.
vector<array<Metrics, 2>> run_sweep(const vector<Task>& tasks, int nthreads, int njobs, int cores) {
    vector<array<Metrics, 2>> res(tasks.size());
    atomic<size_t> idx{0};
    auto worker = [&]() {
        for (;;) {
            size_t k = idx.fetch_add(1);
            if (k >= tasks.size()) break;
            auto jobs = generate(njobs, tasks[k].seed, tasks[k].load, tasks[k].overest, cores);
            for (int p = 0; p < 2; p++) res[k][p] = simulate(jobs, (Policy)p, cores);
        }
    };
#if HAVE_THREADS
    vector<thread> pool;
    for (int t = 0; t < nthreads; t++) pool.emplace_back(worker);
    for (auto& th : pool) th.join();
#else
    (void)nthreads;
    worker();
#endif
    return res;
}

double seconds_since(chrono::steady_clock::time_point t0) {
    return chrono::duration<double>(chrono::steady_clock::now() - t0).count();
}

// ------------------------------------------------------------------------ main
int main(int argc, char** argv) {
    string mode = argc > 1 ? argv[1] : "demo";
    const int CORES = 64;

    if (mode == "demo") {
        // Hand-checkable example from the notes: 8 cores.
        // FCFS expected avg wait 5.67; EASY expected 3.00 (C backfills at t=2).
        vector<Job> jobs = {{0, 0, 10, 10, 6}, {1, 1, 5, 5, 4}, {2, 2, 3, 3, 2}};
        for (int p = 0; p < 2; p++) {
            printf("%s\n", POLICY_NAME[p]);
            Metrics m = simulate(jobs, (Policy)p, 8, true);
            printf("  avg_wait=%.2f max_wait=%.2f bsld=%.2f util=%.1f%%\n",
                   m.avg_wait, m.max_wait, m.bsld, 100 * m.util);
        }
    } else if (mode == "sweep") {
        int threads = argc > 2 ? atoi(argv[2]) : 1;
        int reps = argc > 3 ? atoi(argv[3]) : 20;
        int njobs = argc > 4 ? atoi(argv[4]) : 2000;
        auto tasks = make_tasks(reps);
        auto t0 = chrono::steady_clock::now();
        auto res = run_sweep(tasks, threads, njobs, CORES);
        fprintf(stderr, "sweep: %zu tasks, %d threads, %.2f s\n", tasks.size(), threads, seconds_since(t0));
        printf("policy,load,overest,seed,avg_wait,max_wait,bsld,util\n");
        for (size_t k = 0; k < tasks.size(); k++)
            for (int p = 0; p < 2; p++)
                printf("%s,%.2f,%.1f,%u,%.4f,%.4f,%.4f,%.4f\n", POLICY_NAME[p], tasks[k].load,
                       tasks[k].overest, tasks[k].seed, res[k][p].avg_wait, res[k][p].max_wait,
                       res[k][p].bsld, res[k][p].util);
    } else if (mode == "bench") {
        int reps = argc > 2 ? atoi(argv[2]) : 20;
        int njobs = argc > 3 ? atoi(argv[3]) : 2000;
        auto tasks = make_tasks(reps);
#if HAVE_THREADS
        fprintf(stderr, "hardware threads: %u\n", thread::hardware_concurrency());
#endif
        printf("threads,seconds,speedup\n");
        double base = 0;
        for (int th : {1, 2, 4, 8}) {
            double best = 1e300;
            for (int rep = 0; rep < 3; rep++) {          // best of 3 to reduce noise
                auto t0 = chrono::steady_clock::now();
                run_sweep(tasks, th, njobs, CORES);
                best = min(best, seconds_since(t0));
            }
            if (th == 1) base = best;
            printf("%d,%.4f,%.3f\n", th, best, base / best);
            fflush(stdout);
        }
    } else {
        fprintf(stderr, "usage: sim demo | sim sweep [threads] [reps] [jobs] | sim bench [reps] [jobs]\n");
        return 1;
    }
    return 0;
}
