// Headless CPU benchmark: renders every Monomachine machine through the emulated DSP and reports the cost
// against Move's audio deadline (128 frames at 44.1 kHz = 2902 us).
//
//   mnm-bench <os.syx> [seconds=4] [cpu=-1]
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

#ifdef __linux__
#include <sched.h>
#endif

#include "MonoVoice.h"
#include "firmware/Firmware.h"
#include "host/Machines.h"

using namespace mnm;
using Clock = std::chrono::steady_clock;

static constexpr int kMoveFrames = 128;
static constexpr double kDeadlineUs = kMoveFrames * 1e6 / 44100.0;

int main(int argc, char** argv)
{
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
