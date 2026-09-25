// Monomodule One (sound generator) and Monomodule FX (audio FX): the plugin half, loaded into MoveOriginal.
//
// Every entry point here runs on the SPI audio callback (docs/MODULES.md "there is no control thread"), so
// nothing here allocates, locks, logs or does file IO after create_instance. The engines run in a separate
// process (mnm-engine, src/child) that a per-instance supervisor thread spawns, watches and respawns. The
// callback only copies audio to and from shared memory (src/common/mnm_shm.h), queues commands and keeps
// the parameter table, which is the authority: a respawned engine is replayed from it.
//
// Built twice: MNM_FX=0 -> dsp.so (move_plugin_init_v2), MNM_FX=1 -> monomodule-fx.so (move_audio_fx_init_v2).
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include <dlfcn.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "host_api/plugin_api_v1.h"
#include "host_api/audio_fx_api_v2.h"
#include "host/Machines.h"
#include "mnm_shm.h"

extern char** environ;

namespace {

using namespace mnm;

const host_api_v1_t* g_host = nullptr;

constexpr const char* kOsFile = "os/Elektron_SFX6-60_OS1.32B.syx";
constexpr int kDefaultDepth = 2;
constexpr int kDefaultFifo = 10;

// ---- parameters (the bare test surface; the real UI is designed separately)
// SYN/AMP/FILT/EFX pages x 8 raw 0-127 values, plus machine and level. Keys: "machine", "level",
// "syn1".."syn8", "amp1".., "filt1".., "efx1"..; "depth" (latency, blocks ahead).
constexpr const char* kPageKeys[4] = {"syn", "amp", "filt", "efx"};

#if MNM_FX
constexpr host::Machine kFxMachines[] = {host::Machine::THRU, host::Machine::REVERB, host::Machine::CHORUS,
    host::Machine::DYNAMIX, host::Machine::RINGMOD, host::Machine::PHASER, host::Machine::FLANGER};
#endif

struct Params {
    int machine = 0;              // host::Machine value
    int level = 100;
    int page[4][8] = {};
};

struct Instance {
    char moduleDir[512];
    pthread_t supervisor{};
    bool supervisorStarted = false;
    std::atomic<shm::Segment*> seg{nullptr};
    std::atomic<uint32_t> segGen{0};        // bumped by the supervisor after each (re)spawn
    uint32_t replayedGen = 0;               // callback: last generation the table was replayed into
    std::atomic<uint32_t> stop{0};          // callback -> supervisor
    std::atomic<uint32_t> supervisorWake{0};
    std::atomic<uint32_t> respawns{0};
    Params params;
    int depth = kDefaultDepth;
    float lastBpm = 0.f;
    uint32_t bpmTick = 0;
    char error[200];
};

// ---------------------------------------------------------------------------------------------------- helpers

void futexWake(std::atomic<uint32_t>* a)
{
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(a), FUTEX_WAKE, 1, nullptr, nullptr, 0);
}

void futexWaitMs(std::atomic<uint32_t>* a, uint32_t expected, int ms)
{
    timespec ts{ms / 1000, (ms % 1000) * 1000000L};
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(a), FUTEX_WAIT, expected, &ts, nullptr, 0);
}

void push(Instance& in, shm::Op op, int a = 0, int b = 0, int c = 0)
{
    auto* s = in.seg.load(std::memory_order_acquire);
    if (!s) return;   // not booted yet: the table is replayed when it is
    shm::Cmd cmd{};
    cmd.op = uint16_t(op); cmd.engine = 0; cmd.a = uint8_t(a); cmd.b = uint8_t(b); cmd.c = c;
    shm::pushCmd(*s, cmd);
}

void replay(Instance& in)
{
    const auto& p = in.params;
    push(in, shm::Op::SetMachine, p.machine);   // loads the machine's page defaults: the pages go after it
    for (int pg = 0; pg < 4; ++pg)
        for (int k = 0; k < 8; ++k) push(in, shm::Op::SetParam, pg, k, p.page[pg][k]);
    push(in, shm::Op::SetLevel, 0, 0, p.level);
    if (in.lastBpm > 0.f) push(in, shm::Op::SetBpm, 0, 0, int(std::lround(in.lastBpm * 100.f)));
}

// The page values a machine starts with (the table must match what the engine does on SetMachine).
void loadMachineDefaults(Params& p)
{
    const auto m = host::Machine(p.machine);
    const auto* def = host::machineDef(m);
    for (int k = 0; k < 8; ++k) {
        p.page[0][k] = def ? def->defaults[size_t(k)] : 0;
        p.page[1][k] = host::isFxMachine(m) ? host::kDefaultAmpFx[size_t(k)] : host::kDefaultAmp[size_t(k)];
        p.page[2][k] = host::kDefaultFilt[size_t(k)];
        p.page[3][k] = host::kDefaultEfx[size_t(k)];
    }
}

const char* machineName(int m)
{
    const auto* d = host::machineDef(host::Machine(m));
    return d ? d->name : "?";
}

bool parseMachine(const char* val, int& out)
{
    char* end = nullptr;
    const long idx = std::strtol(val, &end, 10);
#if MNM_FX
    constexpr int kCount = int(sizeof kFxMachines / sizeof kFxMachines[0]);
    if (end != val && *end == 0) { if (idx < 0 || idx >= kCount) return false; out = int(kFxMachines[idx]); return true; }
    for (const auto m : kFxMachines) if (std::strcmp(val, machineName(int(m))) == 0) { out = int(m); return true; }
#else
    if (end != val && *end == 0) {   // an index into kMachineDefs (the enum's option order)
        if (idx < 0 || idx >= host::kNumMachineDefs) return false;
        out = int(host::kMachineDefs[idx].machine); return true;
    }
    for (const auto& d : host::kMachineDefs) if (std::strcmp(val, d.name) == 0) { out = int(d.machine); return true; }
#endif
    return false;
}

// ---------------------------------------------------------------------------------------------------- supervisor

bool spawnChild(Instance& in, int fd, pid_t& pid)
{
    char exe[600], os[600], log[600], prio[8];
    std::snprintf(exe, sizeof exe, "%s/mnm-engine", in.moduleDir);
    std::snprintf(os, sizeof os, "%s/%s", in.moduleDir, kOsFile);
    std::snprintf(log, sizeof log, "%s/mnm-engine.log", in.moduleDir);
    std::snprintf(prio, sizeof prio, "%d", kDefaultFifo);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd, 3);
    posix_spawn_file_actions_addopen(&fa, 1, log, O_WRONLY | O_CREAT | O_APPEND, 0644);   // the emulator prints
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    char* argv[] = {exe, os, log, prio, nullptr};
    const int rc = posix_spawn(&pid, exe, &fa, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) { std::snprintf(in.error, sizeof in.error, "cannot start %s: %s", exe, std::strerror(rc)); return false; }
    return true;
}

void resetSegment(shm::Segment& s, bool fx, int depth)
{
    // Only the fields the child owns or reads at boot. host_block, cmd_write and underruns belong to the
    // callback and keep running across a respawn.
    s.produced.store(s.host_block.load());
    s.phase.store(uint32_t(shm::Phase::Spawning));
    s.child_pid.store(0);
    s.shutdown.store(0);
    s.error[0] = 0;
    s.engines = 1;
    s.hasInput = fx ? 1 : 0;
    s.depth.store(uint32_t(depth));
    s.cmd_read.store(s.cmd_write.load());   // a respawned engine starts from the table replay, not stale commands
}

// destroy_instance returns without waiting (a child's exit takes ~150 ms: its locked memory is torn down),
// so the supervisor frees the instance itself once destroy has handed it over.
void freeWhenStopped(Instance& in)
{
    // stop is set by destroy_instance; an early return (missing OS file) must still wait for it
    while (!in.stop.load(std::memory_order_acquire)) futexWaitMs(&in.supervisorWake, in.supervisorWake.load(), 1000);
    in.~Instance();
    std::free(&in);
}

void* supervise(void* arg)
{
    auto& in = *static_cast<Instance*>(arg);
    // Threads inherit the callback's SCHED_FIFO 70: demote first, and keep off Move's audio core.
    sched_param sp{}; sp.sched_priority = 0;
    sched_setscheduler(0, SCHED_OTHER, &sp);
    cpu_set_t cpus; CPU_ZERO(&cpus); CPU_SET(0, &cpus); CPU_SET(1, &cpus); CPU_SET(2, &cpus);
    sched_setaffinity(0, sizeof cpus, &cpus);

    char osPath[600];
    std::snprintf(osPath, sizeof osPath, "%s/%s", in.moduleDir, kOsFile);
    struct stat st{};
    if (stat(osPath, &st) != 0) {
        std::snprintf(in.error, sizeof in.error, "Monomachine OS file missing: add %s (free download from Elektron)", kOsFile);
        freeWhenStopped(in);
        return nullptr;
    }

    const int fd = int(syscall(SYS_memfd_create, "mnm-shm", 0));
    if (fd < 0 || ftruncate(fd, sizeof(shm::Segment)) != 0) {
        std::snprintf(in.error, sizeof in.error, "shared memory: %s", std::strerror(errno));
        if (fd >= 0) close(fd);
        freeWhenStopped(in);
        return nullptr;
    }
    auto* seg = static_cast<shm::Segment*>(mmap(nullptr, sizeof(shm::Segment), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (seg == MAP_FAILED) {
        std::snprintf(in.error, sizeof in.error, "shared memory map: %s", std::strerror(errno));
        close(fd);
        freeWhenStopped(in);
        return nullptr;
    }
    new (seg) shm::Segment();
    seg->magic = shm::kMagic; seg->version = shm::kVersion;

    const bool fx = MNM_FX != 0;
    pid_t pid = -1;
    uint32_t lastHeartbeat = 0, lastHostBlock = 0;
    int stalledPolls = 0, failures = 0;
    bool published = false;
    while (!in.stop.load(std::memory_order_acquire)) {
        if (pid <= 0) {
            if (failures >= 5) {   // give up rather than spawn in a loop; the error says why
                futexWaitMs(&in.supervisorWake, in.supervisorWake.load(), 1000);
                continue;
            }
            resetSegment(*seg, fx, in.depth);
            if (!spawnChild(in, fd, pid)) { pid = -1; ++failures; continue; }
            if (!published) { in.seg.store(seg, std::memory_order_release); published = true; }
            in.segGen.fetch_add(1, std::memory_order_release);   // the callback replays the table
            stalledPolls = 0;
        }
        futexWaitMs(&in.supervisorWake, in.supervisorWake.load(), 100);

        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            if (seg->phase.load() == uint32_t(shm::Phase::Failed)) {
                std::snprintf(in.error, sizeof in.error, "%s", seg->error);
                failures = 5;   // a boot failure (bad OS file) will not fix itself
            } else {
                ++failures;
            }
            pid = -1;
            in.respawns.fetch_add(1);
            continue;
        }
        // watchdog: the callback is advancing but the child's loop is not -> hung
        const uint32_t hb = seg->heartbeat.load(), hostBlock = seg->host_block.load();
        if (seg->phase.load() == uint32_t(shm::Phase::Ready) && hb == lastHeartbeat && hostBlock != lastHostBlock) {
            if (++stalledPolls >= 10) {   // ~1 s
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                pid = -1;
                in.respawns.fetch_add(1);
                continue;
            }
        } else {
            stalledPolls = 0;
            if (seg->phase.load() == uint32_t(shm::Phase::Ready)) failures = 0;
        }
        lastHeartbeat = hb; lastHostBlock = hostBlock;
    }

    if (pid > 0) {
        seg->shutdown.store(1, std::memory_order_release);
        futexWake(&seg->host_block);
        int status = 0;
        for (int i = 0; i < 20 && waitpid(pid, &status, WNOHANG) != pid; ++i) usleep(5000);
        if (waitpid(pid, &status, WNOHANG) == 0) { kill(pid, SIGKILL); waitpid(pid, &status, 0); }
    }
    in.seg.store(nullptr, std::memory_order_release);
    munmap(seg, sizeof(shm::Segment));
    close(fd);
    freeWhenStopped(in);
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------- entry points

// Keeps this .so mapped for the life of the process: supervisor threads outlive destroy_instance, and the
// host may dlclose the module right after it.
void pinSelf()
{
    static std::atomic<bool> pinned{false};
    if (pinned.exchange(true)) return;
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&pinSelf), &info) && info.dli_fname)
        dlopen(info.dli_fname, RTLD_NOW | RTLD_NODELETE | RTLD_NOLOAD);
}

void* createInstance(const char* moduleDir, const char*)
{
    auto* in = static_cast<Instance*>(std::calloc(1, sizeof(Instance)));
    if (!in) return nullptr;
    new (in) Instance();
    std::snprintf(in->moduleDir, sizeof in->moduleDir, "%s", moduleDir ? moduleDir : ".");
#if MNM_FX
    in->params.machine = int(host::Machine::CHORUS);
#else
    in->params.machine = int(host::Machine::FM_PAR);
#endif
    loadMachineDefaults(in->params);
    pinSelf();
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    in->supervisorStarted = pthread_create(&in->supervisor, &attr, supervise, in) == 0;
    pthread_attr_destroy(&attr);
    return in;
}

void destroyInstance(void* p)
{
    auto* in = static_cast<Instance*>(p);
    if (!in) return;
    if (!in->supervisorStarted) { in->~Instance(); std::free(in); return; }
    // Hand the instance to the supervisor: it stops the child, unmaps, and frees. Nothing here waits.
    in->stop.store(1, std::memory_order_release);
    in->supervisorWake.fetch_add(1);
    futexWake(&in->supervisorWake);
}

void onMidi(void* p, const uint8_t* msg, int len, int)
{
    auto& in = *static_cast<Instance*>(p);
    if (len < 1) return;
    const uint8_t type = msg[0] & 0xF0;
    if (type == 0x90 && len >= 3 && msg[2] > 0) push(in, shm::Op::NoteOn, msg[1]);
    else if ((type == 0x80 && len >= 3) || (type == 0x90 && len >= 3)) push(in, shm::Op::NoteOff, msg[1]);
    else if (type == 0xB0 && len >= 3 && (msg[1] == 123 || msg[1] == 120)) push(in, shm::Op::AllNotesOff);
}

// "state": the whole table, self-contained. Format: m=<machine>;l=<level>;d=<depth>;p=<32 comma-separated>
int writeState(const Instance& in, char* buf, int len)
{
    const auto& p = in.params;
    int n = std::snprintf(buf, size_t(len), "{\"mnm\":\"m=%d;l=%d;d=%d;p=", p.machine, p.level, in.depth);
    for (int pg = 0; pg < 4; ++pg)
        for (int k = 0; k < 8 && n < len; ++k)
            n += std::snprintf(buf + n, size_t(len - n), "%d%s", p.page[pg][k], (pg == 3 && k == 7) ? "" : ",");
    if (n < len) n += std::snprintf(buf + n, size_t(len - n), "\"}");
    return n < len ? n : -1;
}

void readState(Instance& in, const char* val)
{
    const char* s = std::strstr(val, "m=");
    if (!s) return;
    Params p = in.params;
    int depth = in.depth;
    if (std::sscanf(s, "m=%d;l=%d;d=%d;p=", &p.machine, &p.level, &depth) != 3) return;
    const char* v = std::strstr(s, "p=");
    if (!v) return;
    v += 2;
    for (int i = 0; i < 32; ++i) {
        char* end = nullptr;
        const long x = std::strtol(v, &end, 10);
        if (end == v) return;   // malformed: keep the current table
        p.page[i / 8][i % 8] = int(std::clamp<long>(x, 0, 127));
        v = *end == ',' ? end + 1 : end;
    }
    in.params = p;
    in.depth = std::clamp(depth, 1, shm::kMaxDepth);
    if (auto* sg = in.seg.load()) sg->depth.store(uint32_t(in.depth));
    replay(in);
}

void setParam(void* ptr, const char* key, const char* val)
{
    auto& in = *static_cast<Instance*>(ptr);
    if (!key || !val) return;
    const char* k = std::strrchr(key, ':'); k = k ? k + 1 : key;   // "<prefix>:state"
    if (std::strcmp(k, "state") == 0) { readState(in, val); return; }
    if (std::strcmp(k, "machine") == 0) {
        int m;
        if (parseMachine(val, m) && m != in.params.machine) {
            in.params.machine = m;
            loadMachineDefaults(in.params);
            push(in, shm::Op::SetMachine, m);
        }
        return;
    }
    const int v = std::clamp(std::atoi(val), 0, 127);
    if (std::strcmp(k, "level") == 0) { in.params.level = v; push(in, shm::Op::SetLevel, 0, 0, v); return; }
    if (std::strcmp(k, "depth") == 0) {
        in.depth = std::clamp(std::atoi(val), 1, shm::kMaxDepth);
        if (auto* s = in.seg.load()) s->depth.store(uint32_t(in.depth));
        return;
    }
    for (int pg = 0; pg < 4; ++pg) {
        const size_t n = std::strlen(kPageKeys[pg]);
        if (std::strncmp(k, kPageKeys[pg], n) == 0 && k[n] >= '1' && k[n] <= '8' && k[n + 1] == 0) {
            const int idx = k[n] - '1';
            in.params.page[pg][idx] = v;
            push(in, shm::Op::SetParam, pg, idx, v);
            return;
        }
    }
}

int getParam(void* ptr, const char* key, char* buf, int len)
{
    auto& in = *static_cast<Instance*>(ptr);
    if (!key || !buf || len <= 0) return -1;
    const char* k = std::strrchr(key, ':'); k = k ? k + 1 : key;
    if (std::strcmp(k, "state") == 0) return writeState(in, buf, len);
    if (std::strcmp(k, "machine") == 0) {
#if MNM_FX
        for (int i = 0; i < int(sizeof kFxMachines / sizeof kFxMachines[0]); ++i)
            if (int(kFxMachines[i]) == in.params.machine) return std::snprintf(buf, size_t(len), "%s", machineName(in.params.machine));
        return -1;
#else
        return std::snprintf(buf, size_t(len), "%s", machineName(in.params.machine));
#endif
    }
    if (std::strcmp(k, "level") == 0) return std::snprintf(buf, size_t(len), "%d", in.params.level);
    if (std::strcmp(k, "depth") == 0) return std::snprintf(buf, size_t(len), "%d", in.depth);
    if (std::strcmp(k, "status") == 0) {   // diagnostics
        auto* s = in.seg.load();
        if (!s) return std::snprintf(buf, size_t(len), "{\"phase\":\"none\",\"error\":\"%s\"}", in.error);
        return std::snprintf(buf, size_t(len),
            "{\"phase\":%u,\"pid\":%u,\"underruns\":%u,\"skips\":%u,\"respawns\":%u,\"last_us\":%u,\"max_us\":%u,\"faulted\":%u,\"depth\":%d}",
            s->phase.load(), s->child_pid.load(), s->underruns.load(), s->skips.load(), in.respawns.load(), s->stats[0].lastUs.load(),
            s->stats[0].maxUs.load(), s->stats[0].faulted.load(), in.depth);
    }
    for (int pg = 0; pg < 4; ++pg) {
        const size_t n = std::strlen(kPageKeys[pg]);
        if (std::strncmp(k, kPageKeys[pg], n) == 0 && k[n] >= '1' && k[n] <= '8' && k[n + 1] == 0)
            return std::snprintf(buf, size_t(len), "%d", in.params.page[pg][k[n] - '1']);
    }
    return -1;
}

int getError(void* ptr, char* buf, int len)
{
    auto& in = *static_cast<Instance*>(ptr);
    if (!in.error[0]) return 0;
    return std::snprintf(buf, size_t(len), "%s", in.error);
}

// The callback's half of the clock. Returns the segment when the engine is live, nullptr otherwise.
shm::Segment* tick(Instance& in)
{
    auto* s = in.seg.load(std::memory_order_acquire);
    if (!s) return nullptr;
    const uint32_t gen = in.segGen.load(std::memory_order_acquire);
    if (gen != in.replayedGen && s->phase.load(std::memory_order_acquire) == uint32_t(shm::Phase::Ready)) {
        in.replayedGen = gen;
        replay(in);
    }
    if (g_host && g_host->get_bpm && (++in.bpmTick & 63) == 0) {   // ~6 times a second
        const float bpm = g_host->get_bpm();
        if (bpm > 0.f && std::fabs(bpm - in.lastBpm) > 0.01f) { in.lastBpm = bpm; push(in, shm::Op::SetBpm, 0, 0, int(std::lround(bpm * 100.f))); }
    }
    return s->phase.load(std::memory_order_acquire) == uint32_t(shm::Phase::Ready) ? s : nullptr;
}

inline int16_t toS16(int32_t v24)
{
    const int32_t v = (v24 + 128) >> 8;
    return int16_t(std::clamp<int32_t>(v, -32768, 32767));
}

// Plays block `b` from the output slots if the child has produced it, else silence.
void playBlock(shm::Segment& s, uint32_t b, int16_t* out, int frames)
{
    const uint32_t produced = s.produced.load(std::memory_order_acquire);
    if (int32_t(produced - b) <= 0 || int32_t(produced - b) > shm::kSlots) {
        std::memset(out, 0, size_t(frames) * 2 * sizeof(int16_t));
        s.underruns.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const int32_t* src = s.out[b & (shm::kSlots - 1)][0];
    for (int i = 0; i < frames * 2; ++i) out[i] = toS16(src[i]);
}

void advance(shm::Segment& s, uint32_t hb)
{
    s.host_block.store(hb + 1, std::memory_order_release);
    futexWake(&s.host_block);
}

#if !MNM_FX
void renderBlock(void* ptr, int16_t* out, int frames)
{
    auto& in = *static_cast<Instance*>(ptr);
    auto* s = tick(in);
    if (!s || frames != shm::kFrames) { std::memset(out, 0, size_t(frames) * 2 * sizeof(int16_t)); return; }
    const uint32_t hb = s->host_block.load(std::memory_order_relaxed);
    playBlock(*s, hb, out, frames);
    advance(*s, hb);
}

plugin_api_v2_t g_api = {
    MOVE_PLUGIN_API_VERSION_2, createInstance, destroyInstance, onMidi, setParam, getParam, getError, renderBlock,
};
#else
void processBlock(void* ptr, int16_t* io, int frames)
{
    auto& in = *static_cast<Instance*>(ptr);
    auto* s = tick(in);
    if (!s || frames != shm::kFrames) return;   // not booted: pass the audio through untouched
    const uint32_t hb = s->host_block.load(std::memory_order_relaxed);
    int32_t* dst = s->in[hb & (shm::kSlots - 1)];
    for (int i = 0; i < frames * 2; ++i) dst[i] = int32_t(io[i]) << 8;
    const uint32_t depth = s->depth.load(std::memory_order_relaxed);
    if (hb >= depth) playBlock(*s, hb - depth, io, frames);
    else std::memset(io, 0, size_t(frames) * 2 * sizeof(int16_t));
    advance(*s, hb);
}

audio_fx_api_v2_t g_fxApi = {
    AUDIO_FX_API_VERSION_2, createInstance, destroyInstance, processBlock, setParam, getParam, onMidi,
};
#endif

} // namespace

extern "C" {
#if !MNM_FX
__attribute__((visibility("default"))) plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t* host)
{
    g_host = host;
    return &g_api;
}
#else
__attribute__((visibility("default"))) audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t* host)
{
    g_host = host;
    return &g_fxApi;
}
#endif
}
