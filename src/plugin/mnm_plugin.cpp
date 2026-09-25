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
#include <climits>
#include <strings.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

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
#include "generated/mnm_ui.h"
#include "library/MnmDump.h"

#include <dirent.h>
#include <string>
#include <unordered_set>
#include <vector>
#include "mnm_shm.h"

extern char** environ;

namespace {

using namespace mnm;

const host_api_v1_t* g_host = nullptr;

constexpr const char* kOsFile = "os/Elektron_SFX6-60_OS1.32B.syx";
constexpr int kDefaultDepth = 2;
constexpr int kDefaultFifo = 10;

// ---- parameters: generated from upstream's display spec (tools/gen/gen_ui.py -> generated/mnm_ui.h). Every
// knob is stored RAW (0..127, the kit byte) and converted to its display value (bipolar -64..63, list and
// readout names) at the set/get boundary. Keys: "machine", "level", "depth", "load" (read), the SYN keys of
// every machine ("sid_wave", ...), "amp_atk".., "filt_base".., "efx_eqf".., "lfo1_page".. (LFO DEST: one key
// per PAGE value, "lfo1_dest_ptch".., all the same raw slot).
#if MNM_FX
constexpr const ui::ParamDef* kDefs = ui::kFXParams;
constexpr int kDefCount = ui::kFXParamCount;
constexpr const ui::MachineDef* kMachineList = ui::kFXMachines;
constexpr int kMachineCount = ui::kFXMachineCount;
constexpr const char* kHierarchy = ui::kFXHierarchy;
#else
constexpr const ui::ParamDef* kDefs = ui::kONEParams;
constexpr int kDefCount = ui::kONEParamCount;
constexpr const ui::MachineDef* kMachineList = ui::kONEMachines;
constexpr int kMachineCount = ui::kONEMachineCount;
constexpr const char* kHierarchy = ui::kONEHierarchy;
#endif
constexpr int kMaxMachineId = 40;   // host::Machine values go up to 33 (DDRW / DENS)

struct Params {
    int machine = 0;              // host::Machine value
    int level = 100;
    int page[4][8] = {};          // raw SYN / AMP / FILT / EFX of the current machine
    int lfo[3][8] = {};           // raw LFO 1-3
    int synMem[kMaxMachineId][8] = {};   // each machine's last SYN values, restored when it is picked again
    bool synMemSet[kMaxMachineId] = {};
};

// ---- presets: an Init per machine, then every matching sound (kit track) of the user's .syx dumps.
// Built by the supervisor thread (it parses files and allocates); the callback only reads it, through an
// atomic pointer. A replaced catalog is freed a second later, long after any get_param reading it returned.
struct PresetSound {
    char name[28];
    int machine;
    int level;
    uint8_t params[56];   // SYN AMP FILT EFX (32) + LFO 1-3 (24), raw, as in a kit track
};
struct Catalog {
    std::vector<PresetSound> sounds;
    uint64_t signature = 0;   // of the dump files it was built from
};

struct Instance {
    char moduleDir[512];
    pthread_t supervisor{};
    bool supervisorStarted = false;
    std::atomic<shm::Segment*> seg{nullptr};
    std::atomic<uint32_t> segGen{0};        // bumped by the supervisor after each (re)spawn
    uint32_t replayedGen = 0;               // callback: last generation the table was replayed into
    std::atomic<uint32_t> stop{0};          // callback -> supervisor; also the supervisor's futex word
    std::atomic<uint32_t> respawns{0};
    uint32_t notesIn = 0;      // callback-only diagnostics, read by get_param("status") on the same thread
    int32_t peak = 0;          // max |sample| of the output since the last status read (int16)
    Params params;
    std::atomic<Catalog*> catalog{nullptr};
    int presetIndex = 0;
    int depth = kDefaultDepth;
    float lastBpm = 0.f;
    uint32_t bpmTick = 0;
    char error[200];
};

// ---------------------------------------------------------------------------------------------------- helpers

// The segment clock is shared with the child process: a shared futex. In-process words use private
// futexes, and a private FUTEX_WAKE never dereferences its address (destroy relies on that).
void futexWakeShared(std::atomic<uint32_t>* a)
{
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(a), FUTEX_WAKE, 1, nullptr, nullptr, 0);
}

void futexWakePrivate(std::atomic<uint32_t>* a)
{
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(a), FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, nullptr, nullptr, 0);
}

void futexWaitPrivateMs(std::atomic<uint32_t>* a, uint32_t expected, int ms)
{
    timespec ts{ms / 1000, (ms % 1000) * 1000000L};
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(a), FUTEX_WAIT | FUTEX_PRIVATE_FLAG, expected, &ts, nullptr, 0);
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
    for (int l = 0; l < 3; ++l)
        for (int k = 0; k < 8; ++k) push(in, shm::Op::SetLfo, l, k, p.lfo[l][k]);
    push(in, shm::Op::SetLevel, 0, 0, p.level);
    if (in.lastBpm > 0.f) push(in, shm::Op::SetBpm, 0, 0, int(std::lround(in.lastBpm * 100.f)));
}

const ui::MachineDef* findMachine(int id)
{
    for (int i = 0; i < kMachineCount; ++i) if (kMachineList[i].id == id) return &kMachineList[i];
    return nullptr;
}

bool validMachine(int m) { return findMachine(m) != nullptr; }

// SYN defaults of a machine, and the shared pages / LFOs, from the generated table.
void machineSynDefaults(int machine, int out[8])
{
    for (int k = 0; k < 8; ++k) out[k] = 0;
    for (int i = 0; i < kDefCount; ++i)
        if (kDefs[i].page == 0 && kDefs[i].machine == machine) out[kDefs[i].index] = kDefs[i].defaultRaw;
}

void loadDefaults(Params& p)
{
    machineSynDefaults(p.machine, p.page[0]);
    for (int i = 0; i < kDefCount; ++i) {
        const auto& d = kDefs[i];
        if (d.page >= 1 && d.page <= 3) p.page[d.page][d.index] = d.defaultRaw;
        else if (d.page >= 4) p.lfo[d.page - 4][d.index] = d.defaultRaw;
    }
}

// A new machine: the old one's SYN values are kept, the new one's come back (or start at its defaults).
// The engine's SetMachine loads that machine's page defaults, so the SYN values are sent after it.
void switchMachine(Instance& in, int m)
{
    auto& p = in.params;
    if (p.machine >= 0 && p.machine < kMaxMachineId) {
        std::memcpy(p.synMem[p.machine], p.page[0], sizeof p.page[0]);
        p.synMemSet[p.machine] = true;
    }
    p.machine = m;
    if (m < kMaxMachineId && p.synMemSet[m]) std::memcpy(p.page[0], p.synMem[m], sizeof p.page[0]);
    else machineSynDefaults(m, p.page[0]);
    push(in, shm::Op::SetMachine, m);
    for (int pg = 0; pg < 4; ++pg)   // SetMachine reloads every page's defaults in the engine: resend them all
        for (int k = 0; k < 8; ++k) push(in, shm::Op::SetParam, pg, k, p.page[pg][k]);
}

bool parseMachine(const char* val, int& out)
{
    char* end = nullptr;
    const long idx = std::strtol(val, &end, 10);
    if (end != val && *end == 0) {   // an index into the enum's options
        if (idx < 0 || idx >= kMachineCount) return false;
        out = kMachineList[idx].id; return true;
    }
    for (int i = 0; i < kMachineCount; ++i) if (std::strcmp(val, kMachineList[i].label) == 0) { out = kMachineList[i].id; return true; }
    return false;
}

const ui::ParamDef* findDef(const char* key)
{
    for (int i = 0; i < kDefCount; ++i) if (std::strcmp(kDefs[i].key, key) == 0) return &kDefs[i];
    return nullptr;
}

constexpr int listIndex(int raw, int n) { return ((2 * raw + 1) * n) >> 8; }   // upstream SpecData.h
constexpr int listRawMid(int idx, int n) { return (idx * 256 + 128) / (2 * n); }

// Display value -> raw. Lists take an option name or index; numbers are clamped to their range.
bool toRaw(const ui::ParamDef& d, const char* val, int& raw)
{
    if (d.kind == 2 || d.kind == 3) {
        const int n = d.kind == 3 ? 128 : d.count;
        int idx = -1;
        for (int i = 0; i < n && d.values; ++i) if (std::strcmp(val, d.values[i]) == 0) { idx = i; break; }
        if (idx < 0) {
            char* end = nullptr;
            const long v = std::strtol(val, &end, 10);
            if (end == val || *end != 0) return false;
            idx = int(std::clamp<long>(v, 0, n - 1));
        }
        raw = d.kind == 3 ? idx : listRawMid(idx, n);
        return true;
    }
    char* end = nullptr;
    const double v = std::strtod(val, &end);
    if (end == val) return false;
    const int iv = int(std::lround(v));
    raw = d.kind == 1 ? std::clamp(iv + 64, 0, 127) : std::clamp(iv, 0, 127);
    return true;
}

int fromRaw(const ui::ParamDef& d, int raw, char* buf, int len)
{
    if (d.kind == 3 && d.values) return std::snprintf(buf, size_t(len), "%s", d.values[std::clamp(raw, 0, 127)]);
    if (d.kind == 2 && d.values) return std::snprintf(buf, size_t(len), "%s", d.values[std::clamp(listIndex(raw, d.count), 0, d.count - 1)]);
    return std::snprintf(buf, size_t(len), "%d", d.kind == 1 ? raw - 64 : raw);
}

int* rawSlot(Params& p, const ui::ParamDef& d)
{
    if (d.page == 0) {
        if (d.machine == p.machine) return &p.page[0][d.index];
        if (d.machine >= 0 && d.machine < kMaxMachineId) {   // another machine's knob: its remembered value
            if (!p.synMemSet[d.machine]) { machineSynDefaults(d.machine, p.synMem[d.machine]); p.synMemSet[d.machine] = true; }
            return &p.synMem[d.machine][d.index];
        }
        return nullptr;
    }
    if (d.page <= 3) return &p.page[d.page][d.index];
    return &p.lfo[d.page - 4][d.index];
}

int presetCount(const Instance& in)
{
    const Catalog* c = in.catalog.load(std::memory_order_acquire);
    return kMachineCount + (c ? int(c->sounds.size()) : 0);
}

int presetName(const Instance& in, int i, char* buf, int len)
{
    if (i < 0 || i >= presetCount(in)) return -1;
    if (i < kMachineCount) return std::snprintf(buf, size_t(len), "Init %s", kMachineList[i].label);
    return std::snprintf(buf, size_t(len), "%s", in.catalog.load(std::memory_order_acquire)->sounds[size_t(i - kMachineCount)].name);
}

// Loads a whole sound: machine, every page, the LFOs and the level. Init = the machine's defaults.
void loadPreset(Instance& in, int i)
{
    if (i < 0 || i >= presetCount(in)) return;
    in.presetIndex = i;
    auto& p = in.params;
    if (i < kMachineCount) {
        const int m = kMachineList[i].id;
        if (p.machine >= 0 && p.machine < kMaxMachineId) p.synMemSet[p.machine] = false;
        p.machine = m;
        loadDefaults(p);
        p.level = 100;
    } else {
        const auto& snd = in.catalog.load(std::memory_order_acquire)->sounds[size_t(i - kMachineCount)];
        p.machine = snd.machine;
        for (int k = 0; k < 32; ++k) p.page[k / 8][k % 8] = snd.params[k];
        for (int k = 0; k < 24; ++k) p.lfo[k / 8][k % 8] = snd.params[32 + k];
        p.level = snd.level;
    }
    if (p.machine < kMaxMachineId) p.synMemSet[p.machine] = false;   // the preset's own SYN values are current
    replay(in);
}

// Signature of the dump folders: file names, sizes and mtimes. A change triggers a rebuild.
uint64_t scanDumps(const Instance& in, std::vector<std::string>* files)
{
    const char* dirs[] = {"%s/dumps", "%s/../../audio_fx/monomodule-fx/dumps", "%s/../../sound_generators/monomodule-one/dumps"};
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
    std::unordered_set<std::string> seenReal;
    for (const char* fmt : dirs) {
        char dir[700];
        std::snprintf(dir, sizeof dir, fmt, in.moduleDir);
        char real[PATH_MAX];
        if (!realpath(dir, real) || !seenReal.insert(real).second) continue;   // own dir == sibling's own
        DIR* d = opendir(real);
        if (!d) continue;
        while (dirent* e = readdir(d)) {
            const size_t n = std::strlen(e->d_name);
            if (n < 5 || strcasecmp(e->d_name + n - 4, ".syx") != 0) continue;
            std::string path = std::string(real) + "/" + e->d_name;
            struct stat st{};
            if (stat(path.c_str(), &st) != 0) continue;
            for (const char* c = e->d_name; *c; ++c) mix(uint8_t(*c));
            mix(uint64_t(st.st_size)); mix(uint64_t(st.st_mtime));
            if (files) files->push_back(path);
        }
        closedir(d);
    }
    return h;
}

Catalog* buildCatalog(const Instance& in, uint64_t signature, const std::vector<std::string>& files)
{
    auto* cat = new Catalog();
    cat->signature = signature;
    std::unordered_set<std::string> seen;   // the same sound in several dumps is listed once
    for (const auto& path : files) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) continue;
        std::vector<uint8_t> data;
        uint8_t buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && data.size() < (64u << 20)) data.insert(data.end(), buf, buf + n);
        std::fclose(f);
        mnm::dump::Dump dump;
        try { dump = mnm::dump::parseDump(data.data(), data.size(), path); } catch (...) { continue; }
        for (const auto& kit : dump.kits) {
            if (kit.isEmptySlot()) continue;
            for (int t = 0; t < 6; ++t) {
                const auto& tr = kit.tracks[t];
                if (!validMachine(tr.model)) continue;   // One lists synth sounds, FX lists FX sounds
                if (tr.model == int(host::Machine::GND)) continue;   // an unused track, not a sound
                std::string key(reinterpret_cast<const char*>(tr.params), 56);
                key += char(tr.model); key += char(tr.level);
                if (!seen.insert(key).second) continue;
                PresetSound snd{};
                std::snprintf(snd.name, sizeof snd.name, "%s %d", kit.name.c_str(), t + 1);
                snd.machine = tr.model;
                snd.level = std::min<int>(tr.level, 127);
                std::memcpy(snd.params, tr.params, 56);
                for (auto& b : snd.params) b = std::min<uint8_t>(b, 127);
                cat->sounds.push_back(snd);
            }
        }
    }
    return cat;
}

// ---------------------------------------------------------------------------------------------------- supervisor

bool spawnChild(Instance& in, int fd, const char* osPath, pid_t& pid)
{
    char exe[600], os[600], log[600], prio[8];
    std::snprintf(exe, sizeof exe, "%s/mnm-engine", in.moduleDir);
    std::snprintf(os, sizeof os, "%s", osPath);
    std::snprintf(log, sizeof log, "%s/mnm-engine.log", in.moduleDir);
    std::snprintf(prio, sizeof prio, "%d", kDefaultFifo);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd, 3);
    posix_spawn_file_actions_addopen(&fa, 1, log, O_WRONLY | O_CREAT | O_TRUNC, 0644);   // the emulator prints; one boot's worth
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
    while (!in.stop.load(std::memory_order_acquire)) futexWaitPrivateMs(&in.stop, 0, 1000);
    delete in.catalog.exchange(nullptr);   // destroy has returned: no callback reads it any more
    in.~Instance();
    std::free(&in);
}

// Sleeps up to ms, returning early (true) when destroy has asked the supervisor to stop.
bool sleepOrStop(Instance& in, int ms)
{
    if (!in.stop.load(std::memory_order_acquire)) futexWaitPrivateMs(&in.stop, 0, ms);
    return in.stop.load(std::memory_order_acquire) != 0;
}

// The OS file: this module's os/ folder, else the sibling Monomodule's, so it is uploaded once.
bool findOsFile(const Instance& in, char* out, size_t len)
{
    const char* candidates[] = {"%s/%s", "%s/../../audio_fx/monomodule-fx/%s", "%s/../../sound_generators/monomodule-one/%s"};
    struct stat st{};
    for (const char* fmt : candidates) {
        std::snprintf(out, len, fmt, in.moduleDir, kOsFile);
        if (stat(out, &st) == 0 && st.st_size > 0) return true;
    }
    return false;
}

void describeExit(Instance& in, int status)
{
    if (WIFSIGNALED(status)) std::snprintf(in.error, sizeof in.error, "engine process crashed (signal %d)", WTERMSIG(status));
    else if (WIFEXITED(status) && WEXITSTATUS(status) == 6) std::snprintf(in.error, sizeof in.error, "engine fault (restarted)");
    else if (WIFEXITED(status)) std::snprintf(in.error, sizeof in.error, "engine process exited (%d)", WEXITSTATUS(status));
}

void* supervise(void* arg)
{
    auto& in = *static_cast<Instance*>(arg);
    // Threads inherit the callback's SCHED_FIFO 70: demote first, and keep off Move's audio core.
    sched_param sp{}; sp.sched_priority = 0;
    sched_setscheduler(0, SCHED_OTHER, &sp);
    cpu_set_t cpus; CPU_ZERO(&cpus); CPU_SET(0, &cpus); CPU_SET(1, &cpus); CPU_SET(2, &cpus);
    sched_setaffinity(0, sizeof cpus, &cpus);

    // Wait for the OS file: the user may upload it after adding the module.
    char osPath[700];
    while (!findOsFile(in, osPath, sizeof osPath)) {
        std::snprintf(in.error, sizeof in.error, "Monomachine OS file missing: upload %s (free download from Elektron)", kOsFile + 3);
        if (sleepOrStop(in, 2000)) { freeWhenStopped(in); return nullptr; }
    }
    in.error[0] = 0;

    const int fd = int(syscall(SYS_memfd_create, "mnm-shm", 1u /* MFD_CLOEXEC: other children must not inherit it */));
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

    Catalog* retired = nullptr;
    int retiredPolls = 0, dumpPolls = 0;
    auto refreshPresets = [&] {
        if (retired && ++retiredPolls >= 10) { delete retired; retired = nullptr; }   // ~1 s after it was replaced
        if (dumpPolls++ % 30 != 0) return;                                            // look every ~3 s
        std::vector<std::string> files;
        const uint64_t sig = scanDumps(in, &files);
        Catalog* cur = in.catalog.load();
        if (cur && cur->signature == sig) return;
        if (!cur && files.empty()) return;
        if (retired) { delete retired; retired = nullptr; }
        Catalog* next = buildCatalog(in, sig, files);
        in.catalog.store(next, std::memory_order_release);
        retired = cur; retiredPolls = 0;
    };

    constexpr int kMaxQuickFailures = 5;       // consecutive failures before giving up
    constexpr int kBootTimeoutPolls = 300;     // 30 s to reach Ready (0.4 s on a CM5)
    const bool fx = MNM_FX != 0;
    pid_t pid = -1;
    uint32_t lastHeartbeat = 0, lastHostBlock = 0;
    int stalledPolls = 0, bootPolls = 0, failures = 0;
    bool published = false, gaveUp = false;
    while (!in.stop.load(std::memory_order_acquire)) {
        if (pid <= 0) {
            if (gaveUp || failures >= kMaxQuickFailures) { gaveUp = true; if (sleepOrStop(in, 1000)) break; continue; }
            if (failures > 1 && sleepOrStop(in, 1000 * (failures - 1))) break;   // first restart at once, then back off
            resetSegment(*seg, fx, in.depth);
            if (!spawnChild(in, fd, osPath, pid)) { pid = -1; ++failures; continue; }
            if (!published) { in.seg.store(seg, std::memory_order_release); published = true; }
            in.segGen.fetch_add(1, std::memory_order_release);   // the callback replays the table
            stalledPolls = 0; bootPolls = 0;
        }
        if (sleepOrStop(in, 100)) break;
        refreshPresets();

        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            const bool bootFailed = seg->phase.load() == uint32_t(shm::Phase::Failed);
            seg->phase.store(uint32_t(shm::Phase::Spawning), std::memory_order_release);   // callback: silence, not underruns
            if (bootFailed) {
                std::snprintf(in.error, sizeof in.error, "%s", seg->error);
                gaveUp = true;   // a boot failure (bad OS file) will not fix itself
            } else {
                describeExit(in, status);
                ++failures;
            }
            pid = -1;
            in.respawns.fetch_add(1);
            continue;
        }
        const bool ready = seg->phase.load() == uint32_t(shm::Phase::Ready);
        const uint32_t hb = seg->heartbeat.load(), hostBlock = seg->host_block.load();
        bool hung = false;
        if (!ready) {
            hung = ++bootPolls >= kBootTimeoutPolls;   // stuck loading or pre-warming
            if (hung) std::snprintf(in.error, sizeof in.error, "engine did not start within 30 s");
        } else if (hb == lastHeartbeat && hostBlock != lastHostBlock) {
            hung = ++stalledPolls >= 10;   // ~1 s: the callback advances, the child's loop does not
            if (hung) std::snprintf(in.error, sizeof in.error, "engine stopped responding (restarted)");
        } else {
            stalledPolls = 0;
            if (failures && hostBlock - lastHostBlock > 0) failures = 0;   // healthy again
        }
        if (hung) {
            seg->phase.store(uint32_t(shm::Phase::Spawning), std::memory_order_release);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            pid = -1;
            ++failures;
            in.respawns.fetch_add(1);
            continue;
        }
        if (ready && !failures && std::strncmp(in.error, "engine", 6) == 0) in.error[0] = 0;
        lastHeartbeat = hb; lastHostBlock = hostBlock;
    }

    if (pid > 0) {
        seg->shutdown.store(1, std::memory_order_release);
        futexWakeShared(&seg->host_block);
        int status = 0;
        for (int i = 0; i < 20 && waitpid(pid, &status, WNOHANG) != pid; ++i) usleep(5000);
        if (waitpid(pid, &status, WNOHANG) == 0) { kill(pid, SIGKILL); waitpid(pid, &status, 0); }
    }
    in.seg.store(nullptr, std::memory_order_release);
    munmap(seg, sizeof(shm::Segment));
    close(fd);
    delete retired;
    freeWhenStopped(in);   // frees the instance and the current catalog
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
    in->params.level = 100;
    loadDefaults(in->params);
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
    // Hand the instance to the supervisor: it stops the child, unmaps, and frees. Nothing here waits, and
    // nothing touches the instance after the store (the supervisor may free it at once; a private
    // FUTEX_WAKE does not dereference the address).
    in->stop.store(1, std::memory_order_release);
    futexWakePrivate(&in->stop);
}

void onMidi(void* p, const uint8_t* msg, int len, int)
{
    auto& in = *static_cast<Instance*>(p);
    if (len < 1) return;
    const uint8_t type = msg[0] & 0xF0;
    if (type == 0x90 && len >= 3 && msg[2] > 0) { push(in, shm::Op::NoteOn, msg[1]); ++in.notesIn; }
    else if ((type == 0x80 && len >= 3) || (type == 0x90 && len >= 3)) push(in, shm::Op::NoteOff, msg[1]);
    else if (type == 0xB0 && len >= 3 && (msg[1] == 123 || msg[1] == 120)) push(in, shm::Op::AllNotesOff);
}

// "state": the current sound, self-contained. m=<machine>;l=<level>;d=<depth>;p=<32 raw page values>;f=<24 raw LFO>
int writeState(const Instance& in, char* buf, int len)
{
    const auto& p = in.params;
    int n = std::snprintf(buf, size_t(len), "{\"mnm\":\"m=%d;l=%d;d=%d;p=", p.machine, p.level, in.depth);
    for (int i = 0; i < 32 && n < len; ++i) n += std::snprintf(buf + n, size_t(len - n), "%d%s", p.page[i / 8][i % 8], i == 31 ? "" : ",");
    if (n < len) n += std::snprintf(buf + n, size_t(len - n), ";f=");
    for (int i = 0; i < 24 && n < len; ++i) n += std::snprintf(buf + n, size_t(len - n), "%d%s", p.lfo[i / 8][i % 8], i == 23 ? "" : ",");
    if (n < len) n += std::snprintf(buf + n, size_t(len - n), "\"}");
    return n < len ? n : -1;
}

bool readInts(const char* v, int* out, int count)
{
    for (int i = 0; i < count; ++i) {
        char* end = nullptr;
        const long x = std::strtol(v, &end, 10);
        if (end == v) return false;
        out[i] = int(std::clamp<long>(x, 0, 127));
        v = *end == ',' ? end + 1 : end;
    }
    return true;
}

void readState(Instance& in, const char* val)
{
    const char* s = std::strstr(val, "m=");
    if (!s) return;
    Params p = in.params;
    int depth = in.depth;
    if (std::sscanf(s, "m=%d;l=%d;d=%d;p=", &p.machine, &p.level, &depth) != 3) return;
    if (!validMachine(p.machine)) return;
    p.level = std::clamp(p.level, 0, 127);
    const char* v = std::strstr(s, "p=");
    int pages[32];
    if (!v || !readInts(v + 2, pages, 32)) return;   // malformed: keep the current table
    for (int i = 0; i < 32; ++i) p.page[i / 8][i % 8] = pages[i];
    if (const char* f = std::strstr(s, ";f=")) {     // LFOs (absent in states from before they were exposed)
        int lfo[24];
        if (readInts(f + 3, lfo, 24)) for (int i = 0; i < 24; ++i) p.lfo[i / 8][i % 8] = lfo[i];
    }
    in.params = p;
    in.depth = std::clamp(depth, 1, shm::kMaxDepth);
    if (auto* sg = in.seg.load()) sg->depth.store(uint32_t(in.depth));
    replay(in);
}

int loadPercent(const Instance& in)
{
    auto* s = in.seg.load(std::memory_order_acquire);
    if (!s || s->phase.load() != uint32_t(shm::Phase::Ready)) return 0;
    if (s->stats[0].parked.load()) return 0;
    return int(std::lround(100.0 * s->stats[0].lastUs.load() / (1e6 * shm::kFrames / 44100.0)));
}

void setParam(void* ptr, const char* key, const char* val)
{
    auto& in = *static_cast<Instance*>(ptr);
    if (!key || !val) return;
    const char* k = std::strrchr(key, ':'); k = k ? k + 1 : key;   // "<prefix>:state"
    if (std::strcmp(k, "state") == 0) { readState(in, val); return; }
    if (std::strcmp(k, "preset") == 0) { loadPreset(in, std::atoi(val)); return; }
    if (std::strcmp(k, "machine") == 0) {
        int m;
        if (parseMachine(val, m) && m != in.params.machine) switchMachine(in, m);
        return;
    }
    if (std::strcmp(k, "level") == 0) { in.params.level = std::clamp(std::atoi(val), 0, 127); push(in, shm::Op::SetLevel, 0, 0, in.params.level); return; }
    if (std::strcmp(k, "depth") == 0) {
        in.depth = std::clamp(std::atoi(val), 1, shm::kMaxDepth);
        if (auto* s = in.seg.load()) s->depth.store(uint32_t(in.depth));
        return;
    }
    const auto* d = findDef(k);
    if (!d) return;
    int raw;
    if (!toRaw(*d, val, raw)) return;
    int* slot = rawSlot(in.params, *d);
    if (!slot) return;
    *slot = raw;
    if (d->page == 0 && d->machine != in.params.machine) return;   // remembered for when that machine is picked
    if (d->page <= 3) push(in, shm::Op::SetParam, d->page, d->index, raw);
    else push(in, shm::Op::SetLfo, d->page - 4, d->index, raw);
}

int getParam(void* ptr, const char* key, char* buf, int len)
{
    auto& in = *static_cast<Instance*>(ptr);
    if (!key || !buf || len <= 0) return -1;
    const char* k = std::strrchr(key, ':'); k = k ? k + 1 : key;
    if (std::strcmp(k, "state") == 0) return writeState(in, buf, len);
    if (std::strcmp(k, "ui_hierarchy") == 0) {
        const int n = int(std::strlen(kHierarchy));
        if (n >= len) return -1;
        std::memcpy(buf, kHierarchy, size_t(n) + 1);
        return n;
    }
    if (std::strcmp(k, "machine") == 0) {
        const auto* m = findMachine(in.params.machine);
        return m ? std::snprintf(buf, size_t(len), "%s", m->label) : -1;
    }
    if (std::strcmp(k, "level") == 0) return std::snprintf(buf, size_t(len), "%d", in.params.level);
    if (std::strcmp(k, "preset") == 0) return std::snprintf(buf, size_t(len), "%d", in.presetIndex);
    if (std::strcmp(k, "preset_count") == 0) return std::snprintf(buf, size_t(len), "%d", presetCount(in));
    if (std::strcmp(k, "preset_name") == 0) return presetName(in, in.presetIndex, buf, len);
    if (std::strcmp(k, "depth") == 0) return std::snprintf(buf, size_t(len), "%d", in.depth);
    if (std::strcmp(k, "load") == 0) return std::snprintf(buf, size_t(len), "%d", loadPercent(in));
    if (std::strcmp(k, "status") == 0) {   // diagnostics
        auto* s = in.seg.load();
        if (!s) return std::snprintf(buf, size_t(len), "{\"phase\":\"none\",\"error\":\"%s\"}", in.error);
        return std::snprintf(buf, size_t(len),
            "{\"notes\":%u,\"peak\":%d,\"phase\":%u,\"pid\":%u,\"underruns\":%u,\"skips\":%u,\"respawns\":%u,\"last_us\":%u,\"max_us\":%u,\"faulted\":%u,\"parked\":%u,\"parked_blocks\":%u,\"depth\":%d}",
            in.notesIn, std::exchange(in.peak, 0), s->phase.load(), s->child_pid.load(), s->underruns.load(), s->skips.load(),
            in.respawns.load(), s->stats[0].lastUs.load(), s->stats[0].maxUs.load(), s->stats[0].faulted.load(),
            s->stats[0].parked.load(), s->stats[0].parkedBlocks.load(), in.depth);
    }
    const auto* d = findDef(k);
    if (!d) return -1;
    const int* slot = rawSlot(in.params, *d);
    return slot ? fromRaw(*d, *slot, buf, len) : -1;
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

void notePeak(Instance& in, const int16_t* out, int frames)
{
    for (int i = 0; i < frames * 2; ++i) in.peak = std::max<int32_t>(in.peak, std::abs(int32_t(out[i])));
}

void advance(shm::Segment& s, uint32_t hb)
{
    s.host_block.store(hb + 1, std::memory_order_release);
    futexWakeShared(&s.host_block);
}

#if !MNM_FX
void renderBlock(void* ptr, int16_t* out, int frames)
{
    auto& in = *static_cast<Instance*>(ptr);
    auto* s = tick(in);
    if (!s || frames != shm::kFrames) { std::memset(out, 0, size_t(frames) * 2 * sizeof(int16_t)); return; }
    const uint32_t hb = s->host_block.load(std::memory_order_relaxed);
    playBlock(*s, hb, out, frames);
    notePeak(in, out, frames);
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
    notePeak(in, io, frames);
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
