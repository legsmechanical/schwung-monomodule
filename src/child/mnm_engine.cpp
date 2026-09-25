// mnm-engine: the engine process of one Monomodule instance. Spawned by the plugin with the shared segment
// (src/common/mnm_shm.h) on fd 3:
//
//   mnm-engine <os.syx> <log-file> [fifo-priority=10]
//
// Runs every engine of the instance on this process's main thread (M2: one engine), pinned to cores 0-2
// (core 3 is Move's audio core), SCHED_FIFO at the given priority when allowed, SCHED_OTHER otherwise.
// Renders ahead of the audio callback by `depth` blocks and sleeps on a futex between blocks.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <vector>

#include <linux/futex.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "MonoVoice.h"
#include "firmware/Firmware.h"
#include "host/Machines.h"
#include "mnm_shm.h"

using namespace mnm;
using Clock = std::chrono::steady_clock;

namespace {

FILE* g_log = nullptr;
uint32_t g_opCounts[16] = {};   // commands received, by op (diagnostics, logged every 5 s)

void logf(const char* fmt, ...)
{
    if (!g_log) return;
    std::time_t t = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof ts, "%H:%M:%S", std::localtime(&t));
    std::fprintf(g_log, "%s [%d] ", ts, int(getpid()));
    va_list ap; va_start(ap, fmt); std::vfprintf(g_log, fmt, ap); va_end(ap);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

void futexWait(std::atomic<uint32_t>* addr, uint32_t expected)
{
    timespec ts{0, 50 * 1000 * 1000};   // bounded, so shutdown and a dead parent are noticed
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(addr), FUTEX_WAIT, expected, &ts, nullptr, 0);
}

void fail(shm::Segment& s, const char* why)
{
    std::snprintf(s.error, sizeof s.error, "%s", why);
    s.phase.store(uint32_t(shm::Phase::Failed), std::memory_order_release);
    logf("FAILED: %s", why);
}

// Idle parking: an engine whose output (and, for FX, input) has been exactly zero for this long, with no
// note held, stops running the DSP until a command or input audio arrives. Long enough for the EFX delay
// and the reverb to have emptied: output that is exactly zero (at the host's 16 bits) for 2 s means nothing
// is still in flight.
constexpr uint32_t kParkAfterBlocks = 44100 * 2 / shm::kFrames;

struct Engine {
    std::unique_ptr<MonoVoice> voice;
    uint32_t silentBlocks = 0;
    bool parked = false;
    bool woken = false;   // a command arrived for it this iteration
    host::Machine machine = host::Machine::GND;
    int heldNotes[16] = {};
    int held = 0;
};

void applyMachineRouting(Engine& e)
{
    auto& h = e.voice->host();
    const bool fx = host::isFxMachine(e.machine);
    h.setRouting(fx ? host::dspInputBits(host::FxInput::InpAB) : 0u);
    if (fx) h.noteOn(60);   // an FX machine runs with its envelope open, as upstream does
}

void applyCmd(Engine& e, const shm::Cmd& c)
{
    auto& h = e.voice->host();
    switch (shm::Op(c.op)) {
    case shm::Op::NoteOn: {
        // mono, last-note priority
        int n = 0;
        for (int i = 0; i < e.held; ++i) if (e.heldNotes[i] != c.a) e.heldNotes[n++] = e.heldNotes[i];
        e.held = n;
        if (e.held < 16) e.heldNotes[e.held++] = c.a;
        h.noteOn(c.a);
        break;
    }
    case shm::Op::NoteOff: {
        const bool wasCurrent = e.held > 0 && e.heldNotes[e.held - 1] == c.a;
        int n = 0;
        for (int i = 0; i < e.held; ++i) if (e.heldNotes[i] != c.a) e.heldNotes[n++] = e.heldNotes[i];
        e.held = n;
        if (host::isFxMachine(e.machine)) break;
        if (e.held == 0) h.noteOff();
        else if (wasCurrent) h.noteOn(e.heldNotes[e.held - 1]);
        break;
    }
    case shm::Op::AllNotesOff:
        e.held = 0;
        if (!host::isFxMachine(e.machine)) h.noteOff();
        break;
    case shm::Op::SetMachine:
        e.machine = host::Machine(c.a);
        h.setMachine(e.machine);
        applyMachineRouting(e);
        break;
    case shm::Op::SetParam:
        if (c.a < 4 && c.b < 8) h.setParam(host::Page(c.a), c.b, std::clamp(int(c.c), 0, 127));
        break;
    case shm::Op::SetLfo:
        if (c.a < 3 && c.b < 8) h.setLfoParam(c.a, c.b, std::clamp(int(c.c), 0, 127));
        break;
    case shm::Op::SetLevel: h.setLevel(std::clamp(int(c.c), 0, 127)); break;
    case shm::Op::SetBpm: h.setBpm(c.c / 100.0); break;
    case shm::Op::SetRouting: h.setRouting(uint32_t(c.c)); break;
    case shm::Op::Reset:
        e.voice->reset();
        e.held = 0;
        break;
    case shm::Op::None: break;
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: mnm-engine <os.syx> <log-file> [fifo-priority]\n"); return 2; }
    g_log = std::fopen(argv[2], "a");
    const int fifo = argc > 3 ? std::atoi(argv[3]) : 10;
    prctl(PR_SET_PDEATHSIG, SIGKILL);   // die with the plugin's supervisor thread
    if (getppid() == 1) return 1;       // the parent died before the prctl took effect
    prctl(PR_SET_NAME, "mnm-engine");

    auto* seg = static_cast<shm::Segment*>(mmap(nullptr, sizeof(shm::Segment), PROT_READ | PROT_WRITE, MAP_SHARED, 3, 0));
    if (seg == MAP_FAILED) { logf("cannot map the shared segment"); return 3; }
    auto& s = *seg;
    if (s.magic != shm::kMagic || s.version != shm::kVersion) { logf("segment magic/version mismatch"); return 3; }
    s.child_pid.store(uint32_t(getpid()));

    // placement: cores 0-2, never Move's audio core
    cpu_set_t cpus; CPU_ZERO(&cpus); CPU_SET(0, &cpus); CPU_SET(1, &cpus); CPU_SET(2, &cpus);
    sched_setaffinity(0, sizeof cpus, &cpus);

    s.phase.store(uint32_t(shm::Phase::Loading));
    const auto t0 = Clock::now();
    std::unique_ptr<fw::Firmware> firmware;
    try {
        firmware = std::make_unique<fw::Firmware>(fw::loadFirmware(argv[1]));
    } catch (const std::exception& ex) {
        char why[150]; std::snprintf(why, sizeof why, "OS file: %s", ex.what());
        fail(s, why);
        return 4;
    }

    const int n = int(std::clamp<uint32_t>(s.engines, 1, shm::kMaxEngines));
    std::vector<Engine> engines(static_cast<size_t>(n));
    s.phase.store(uint32_t(shm::Phase::Prewarming));
    try {
        for (auto& e : engines) {
            e.voice = std::make_unique<MonoVoice>(*firmware);
            e.voice->prewarm();
            e.voice->host().setMachine(e.machine);
            e.voice->warmUp(8);
        }
    } catch (const std::exception& ex) {
        char why[150]; std::snprintf(why, sizeof why, "engine: %s", ex.what());
        fail(s, why);
        return 5;
    }
    logf("ready: %d engine(s) in %.0f ms (fifo %d)", n,
         std::chrono::duration<double, std::milli>(Clock::now() - t0).count(), fifo);

    // realtime only once booted: boot work must not starve Move
    if (fifo > 0) {
        sched_param sp{}; sp.sched_priority = fifo;
        if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) logf("SCHED_FIFO %d refused, staying SCHED_OTHER", fifo);
    }

    // start in step with the callback: the first block rendered is the one it will play next
    uint32_t next = s.host_block.load(std::memory_order_acquire);
    s.produced.store(next, std::memory_order_release);
    s.phase.store(uint32_t(shm::Phase::Ready), std::memory_order_release);

    std::vector<float> L(shm::kFrames), R(shm::kFrames), inL(shm::kFrames), inR(shm::kFrames);
    const bool fxIn = s.hasInput != 0;
    while (!s.shutdown.load(std::memory_order_acquire)) {
        if (getppid() == 1) break;   // MoveOriginal is gone
        s.heartbeat.fetch_add(1, std::memory_order_relaxed);
        const uint32_t hb = s.host_block.load(std::memory_order_acquire);
        const uint32_t depth = std::clamp<uint32_t>(s.depth.load(std::memory_order_relaxed), 1, shm::kMaxDepth);
        // FX: block j can be rendered once its input has arrived (host_block > j); it plays at host_block j+depth.
        // Synth: block j plays at host_block j; render it up to depth blocks early.
        const uint32_t limit = fxIn ? hb : hb + depth;
        const uint32_t oldest = fxIn ? (hb > depth ? hb - depth : 0) : hb;   // the next block the callback plays
        if (int32_t(next - oldest) < 0) {   // fell behind: skip forward, the engines keep their own time
            s.skips.fetch_add(oldest - next, std::memory_order_relaxed);
            next = oldest;
            s.produced.store(next, std::memory_order_release);
        }
        if (int32_t(next - limit) >= 0) { futexWait(&s.host_block, hb); continue; }

        // commands first, so events land in the next block rendered
        uint32_t r = s.cmd_read.load(std::memory_order_relaxed);
        const uint32_t w = s.cmd_write.load(std::memory_order_acquire);
        for (; r != w; ++r) {
            const auto& c = s.cmds[r & (shm::kCmdSlots - 1)];
            if (c.engine < n) {
                applyCmd(engines[c.engine], c); engines[c.engine].woken = true;
                if (c.op < 16) ++g_opCounts[c.op];
            }
        }
        s.cmd_read.store(r, std::memory_order_release);

        const int slot = int(next & (shm::kSlots - 1));
        bool inputSilent = true;
        if (fxIn) {
            const int32_t* in = s.in[slot];
            for (int i = 0; i < shm::kFrames * 2; ++i) if (in[i]) { inputSilent = false; break; }
            for (int i = 0; i < shm::kFrames; ++i) { inL[size_t(i)] = float(in[2 * i]) / 8388608.f; inR[size_t(i)] = float(in[2 * i + 1]) / 8388608.f; }
        }
        for (int k = 0; k < n; ++k) {
            const auto s0 = Clock::now();
            auto& e = engines[size_t(k)];
            auto& v = *e.voice;
            auto& st = s.stats[k];
            int32_t* out = s.out[slot][k];
            if (e.parked && (e.woken || !inputSilent)) { e.parked = false; e.silentBlocks = 0; st.parked.store(0, std::memory_order_relaxed); }
            e.woken = false;
            if (e.parked) {
                v.skip(shm::kFrames);
                std::memset(out, 0, sizeof(int32_t) * shm::kFrames * 2);
                st.parkedBlocks.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (fxIn) v.processFx(inL.data(), inR.data(), L.data(), R.data(), shm::kFrames);
            else v.process(L.data(), R.data(), shm::kFrames);
            bool outputSilent = true;
            for (int i = 0; i < shm::kFrames; ++i) {
                out[2 * i] = int32_t(std::lrint(L[size_t(i)] * 8388608.f));
                out[2 * i + 1] = int32_t(std::lrint(R[size_t(i)] * 8388608.f));
                // silent as Move hears it: every sample rounds to 0 at 16 bits (|v| < 128 in 24-bit). REVERB,
                // DYNAMIX and RINGMOD settle into a fixed-point residue (1-43 LSB, -106..-138 dBFS) that
                // never reaches exact 24-bit zero but is exactly zero in the int16 the host gets.
                if (uint32_t(out[2 * i] + 128) > 255u || uint32_t(out[2 * i + 1] + 128) > 255u) outputSilent = false;
            }
            if (outputSilent && inputSilent && e.held == 0) {
                if (++e.silentBlocks >= kParkAfterBlocks) { e.parked = true; st.parked.store(1, std::memory_order_relaxed); }
            } else {
                e.silentBlocks = 0;
            }
            const auto us = uint32_t(std::chrono::duration<double, std::micro>(Clock::now() - s0).count());
            st.lastUs.store(us, std::memory_order_relaxed);
            if (us > st.maxUs.load(std::memory_order_relaxed)) st.maxUs.store(us, std::memory_order_relaxed);
            st.blocks.fetch_add(1, std::memory_order_relaxed);
            if (v.engine().faulted()) {   // a JIT failure or a runaway block: silent for good. Let the
                st.faulted.store(1, std::memory_order_relaxed);   // supervisor restart us and replay the state.
                logf("engine %d fault: %s", k, v.engine().faultReason().c_str());
                return 6;
            }
        }
        ++next;
        s.produced.store(next, std::memory_order_release);
        if (std::getenv("MNM_DIAG") || true) {
            static uint32_t diagBlocks = 0;
            if (++diagBlocks % 1723 == 0) {   // ~5 s
                const auto& e0 = engines[0];
                logf("diag: parked %d silentBlocks %u held %d | cmds noteon %u noteoff %u allnotesoff %u machine %u param %u lfo %u level %u bpm %u routing %u reset %u",
                     int(e0.parked), e0.silentBlocks, e0.held, g_opCounts[1], g_opCounts[2], g_opCounts[3], g_opCounts[4], g_opCounts[5],
                     g_opCounts[6], g_opCounts[7], g_opCounts[8], g_opCounts[9], g_opCounts[10]);
            }
        }
    }
    logf("shutdown");
    return 0;
}
