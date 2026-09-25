// Headless CPU benchmark: renders every Monomachine machine through the emulated DSP and reports the cost
// against Move's audio deadline (128 frames at 44.1 kHz = 2902 us).
//
//   mnm-bench <os.syx> [seconds=4] [cpu=-1] [machine-substring]
//   mnm-bench <os.syx> --engines N [seconds=10] [machine-substring=SID] [fifo-priority=0 (SCHED_OTHER)]
//
// --engines: N engines on min(N,3) worker threads pinned to cores 0-2 (core 3 is Move's audio core), all
// rendering each 128-frame block together, as the module will. Reports the per-block makespan (time until
// every engine has its block) against the deadline, and resident memory.
//
// Per machine: mean and p99/max wall time per 128-frame Move block, as a percentage of the deadline, plus
// DSP instructions per 16-frame engine block. Synth machines hold a note; FX machines process white noise.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include <atomic>
#include <memory>
#include <fstream>
#include <thread>
#include <map>
#include <cctype>
#include <mutex>
#include <condition_variable>

#ifdef __linux__
#include <sched.h>
#include <cstdio>
#endif

#include "MonoVoice.h"
#include "firmware/Firmware.h"
#include "host/Machines.h"

using namespace mnm;
using Clock = std::chrono::steady_clock;

static constexpr int kMoveFrames = 128;
static constexpr double kDeadlineUs = kMoveFrames * 1e6 / 44100.0;

// Proportional set size: shared pages count once, split between their mappings. VmRSS overstates the
// engine many times over, because dsp56300 maps one physical page behind many virtual blocks.
static long memKb(const char* file, const char* key)
{
    std::ifstream f(file);
    std::string line;
    const size_t n = std::strlen(key);
    while (std::getline(f, line)) if (line.compare(0, n, key) == 0) return std::atol(line.c_str() + n);
    return -1;
}
// MNM_SMAPS=1: after init, the resident mappings grouped by (name, mapping size), largest PSS first.
static void dumpSmaps()
{
    std::ifstream f("/proc/self/smaps");
    std::string line, name; long size = 0;
    std::map<std::pair<std::string, long>, std::pair<long, int>> agg;   // -> (pss kb, count)
    while (std::getline(f, line)) {
        const auto dash = line.find('-'), sp0 = line.find(' ');
        if (dash != std::string::npos && sp0 != std::string::npos && dash < sp0 && std::isxdigit((unsigned char)line[0])) {
            unsigned long a = 0, b = 0; std::sscanf(line.c_str(), "%lx-%lx", &a, &b);
            size = long((b - a) / 1024);
            // fields: range perms offset dev inode [path]
            char perms[8] = {}, dev[16] = {}; unsigned long off = 0, inode = 0; int consumed = 0;
            std::sscanf(line.c_str(), "%*s %7s %lx %15s %lu %n", perms, &off, dev, &inode, &consumed);
            name = consumed > 0 && size_t(consumed) < line.size() ? line.substr(size_t(consumed)) : "[anon]";
            name += std::string(" ") + perms;
        } else if (line.rfind("Pss:", 0) == 0) {
            auto& e = agg[{name, size}]; e.first += std::atol(line.c_str() + 4); e.second++;
        }
    }
    std::vector<std::pair<long, std::string>> v;
    for (auto& [k, e] : agg) if (e.first > 512) v.push_back({e.first, k.first + "  map " + std::to_string(k.second) + " KB x" + std::to_string(e.second)});
    std::sort(v.rbegin(), v.rend());
    for (size_t i = 0; i < v.size() && i < 15; ++i) std::printf("  pss %7ld KB  %s\n", v[i].first, v[i].second.c_str());
}

static long pssKb() { return memKb("/proc/self/smaps_rollup", "Pss:"); }
static long availKb() { return memKb("/proc/meminfo", "MemAvailable:"); }

static int multiEngine(const fw::Firmware& fw, int n, double seconds, const char* only, int fifoPrio)
{
    const host::MachineDef* def = nullptr;
    for (const auto& d : host::kMachineDefs) if (std::strstr(d.name, only)) { def = &d; break; }
    if (!def) { std::fprintf(stderr, "no machine matches '%s'\n", only); return 2; }
    const bool fx = host::isFxMachine(def->machine);
    const long pss0 = pssKb(), avail0 = availKb();
    std::vector<std::unique_ptr<MonoVoice>> voices;
    auto t0 = Clock::now();
    for (int e = 0; e < n; ++e) {
        voices.push_back(std::make_unique<MonoVoice>(fw));
        auto& h = voices.back()->host();
        h.setMachine(def->machine);
        h.setRouting(fx ? host::dspInputBits(host::FxInput::InpAB) : 0u);
        voices.back()->warmUp(8);
        h.noteOn(fx ? 60 : 40 + 3 * e);
    }
    const double initMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    const long pss1 = pssKb(), avail1 = availKb();
    if (std::getenv("MNM_SMAPS")) dumpSmaps();

    const int workers = std::min(n, 3);
    const int blocks = int(seconds * 44100.0 / kMoveFrames);
    std::mutex m;
    std::condition_variable cvGo, cvDone;
    int gen = 0, done = 0;
    bool quit = false;
    std::vector<std::thread> pool;
    for (int w = 0; w < workers; ++w)
        pool.emplace_back([&, w] {
#ifdef __linux__
            cpu_set_t s; CPU_ZERO(&s); CPU_SET(w, &s); sched_setaffinity(0, sizeof(s), &s);
            if (fifoPrio > 0) { sched_param sp{}; sp.sched_priority = fifoPrio; if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) std::perror("SCHED_FIFO"); }
#endif
            std::vector<float> L(kMoveFrames), R(kMoveFrames), in(kMoveFrames, 0.1f);
            int seen = 0;
            for (;;) {
                {
                    std::unique_lock<std::mutex> lk(m);
                    cvGo.wait(lk, [&] { return quit || gen != seen; });
                    if (quit) return;
                    seen = gen;
                }
                for (int e = w; e < n; e += workers) {
                    if (fx) voices[size_t(e)]->processFx(in.data(), in.data(), L.data(), R.data(), kMoveFrames);
                    else voices[size_t(e)]->process(L.data(), R.data(), kMoveFrames);
                }
                { std::lock_guard<std::mutex> lk(m); ++done; }
                cvDone.notify_one();
            }
        });
    std::vector<double> us; us.reserve(blocks);
    auto next = Clock::now();
    for (int b = 0; b < blocks; ++b) {
        next += std::chrono::microseconds(int(kDeadlineUs));   // one block per deadline, as the audio callback
        std::this_thread::sleep_until(next);
        const auto s = Clock::now();
        { std::lock_guard<std::mutex> lk(m); done = 0; ++gen; }
        cvGo.notify_all();
        { std::unique_lock<std::mutex> lk(m); cvDone.wait(lk, [&] { return done == workers; }); }
        us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - s).count());
    }
    { std::lock_guard<std::mutex> lk(m); quit = true; }
    cvGo.notify_all();
    for (auto& t : pool) t.join();
    std::vector<double> sorted = us; std::sort(sorted.begin(), sorted.end());
    double mean = 0; for (double x : us) mean += x; mean /= us.size();
    std::printf("%s x%d on %d cores: makespan mean %.0f us (%.1f%%)  p99 %.0f us (%.1f%%)  p99.9 %.0f us  max %.0f us\n",
                def->name, n, workers, mean, 100 * mean / kDeadlineUs, sorted[size_t(sorted.size() * 0.99)],
                100 * sorted[size_t(sorted.size() * 0.99)] / kDeadlineUs, sorted[size_t(sorted.size() * 0.999)], sorted.back());
    std::printf("init %.0f ms total; per engine: PSS %.1f MB, MemAvailable drop %.1f MB\n", initMs,
                pss0 > 0 ? (pss1 - pss0) / 1024.0 / n : 0.0, avail0 > 0 ? (avail0 - avail1) / 1024.0 / n : 0.0);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc > 3 && std::string(argv[2]) == "--engines") {
        const auto fw = fw::loadFirmware(argv[1]);
        return multiEngine(fw, std::atoi(argv[3]), argc > 4 ? std::atof(argv[4]) : 10.0, argc > 5 ? argv[5] : "SID",
                           argc > 6 ? std::atoi(argv[6]) : 0);
    }
    if (argc < 2) { std::fprintf(stderr, "usage: mnm-bench <os.syx> [seconds=4] [cpu=-1] [machine-substring]\n"); return 2; }
    const double seconds = argc > 2 ? std::atof(argv[2]) : 4.0;
    const int cpu = argc > 3 ? std::atoi(argv[3]) : -1;
    const char* only = argc > 4 ? argv[4] : nullptr;
#ifdef __linux__
    if (cpu >= 0) { cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s); sched_setaffinity(0, sizeof(s), &s); }
#endif

    auto t0 = Clock::now();
    const auto fw = fw::loadFirmware(argv[1]);
    std::printf("firmware loaded in %.0f ms\n", std::chrono::duration<double, std::milli>(Clock::now() - t0).count());

    const int blocks = int(seconds * 44100.0 / kMoveFrames);
    std::vector<float> L(kMoveFrames), R(kMoveFrames), inL(kMoveFrames), inR(kMoveFrames);
    std::mt19937 rng(1);
    std::uniform_real_distribution<float> noise(-0.5f, 0.5f);

    std::printf("deadline %.0f us per %d-frame block, %d blocks per machine\n\n", kDeadlineUs, kMoveFrames, blocks);
    std::printf("%-11s %8s %8s %8s %8s %9s %9s\n", "machine", "mean_us", "p99_us", "max_us", "mean_%", "p99_%", "instr/16f");

    double sumMeanPct = 0; int n = 0;
    for (const auto& def : host::kMachineDefs) {
        if (only && !std::strstr(def.name, only)) continue;
        t0 = Clock::now();
        MonoVoice v(fw);
        auto& h = v.host();
        h.setMachine(def.machine);
        const bool fx = host::isFxMachine(def.machine);
        h.setRouting(fx ? host::dspInputBits(host::FxInput::InpAB) : 0u);
        v.warmUp(8);
        const double initMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        h.noteOn(fx ? 60 : 48);

        std::vector<double> us; us.reserve(blocks);
        uint64_t instr0 = v.engine().stats().totalInstructions; uint32_t blk0 = v.engine().stats().blocks;
        for (int b = 0; b < blocks; ++b) {
            if (fx) for (int i = 0; i < kMoveFrames; ++i) { inL[i] = noise(rng); inR[i] = noise(rng); }
            const auto s = Clock::now();
            if (fx) v.processFx(inL.data(), inR.data(), L.data(), R.data(), kMoveFrames);
            else v.process(L.data(), R.data(), kMoveFrames);
            us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - s).count());
            if (!fx && b == blocks / 2) h.noteOn(55);   // a retrigger mid-run
        }
        const auto& st = v.engine().stats();
        const double ipb = st.blocks > blk0 ? double(st.totalInstructions - instr0) / (st.blocks - blk0) : 0;
        std::vector<double> sorted = us; std::sort(sorted.begin(), sorted.end());
        double mean = 0; for (double x : us) mean += x; mean /= us.size();
        const double p99 = sorted[size_t(sorted.size() * 0.99)], mx = sorted.back();
        std::printf("%-11s %8.1f %8.1f %8.1f %7.1f%% %8.1f%% %9.0f   (init %.0f ms%s)\n", def.name, mean, p99, mx,
                    100 * mean / kDeadlineUs, 100 * p99 / kDeadlineUs, ipb, initMs,
                    v.engine().faulted() ? ", FAULTED" : "");
        sumMeanPct += 100 * mean / kDeadlineUs; ++n;
    }
    if (n) std::printf("\naverage mean load %.1f%% of one core\n", sumMeanPct / n);
    return 0;
}
