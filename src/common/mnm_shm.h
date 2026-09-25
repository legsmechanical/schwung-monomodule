// Shared memory between a module instance (the plugin .so, running in MoveOriginal on the audio callback) and
// its engine process (mnm-engine, spawned per instance). One segment per instance, created with memfd_create
// by the plugin and handed to the child as fd 3.
//
// Timing: the callback owns `host_block` (the number of Move blocks it has consumed). The child keeps up to
// `depth` blocks rendered ahead of it: block b lives in slot b % kSlots and is published by storing
// `produced = b + 1` (release). The callback plays block `host_block` if `produced > host_block`, else it
// plays silence and counts an underrun. A child that falls behind skips its output index forward (the engine
// timeline stays continuous; the listener hears a gap, not a slowdown). Latency = depth x 128 frames.
//
// Commands: an SPSC ring written by the callback (MIDI, parameter changes) and drained by the child before
// each block it renders.
#pragma once
#include <atomic>
#include <cstdint>

namespace mnm::shm {

constexpr uint32_t kMagic = 0x4D4E4D31;   // "MNM1"
constexpr uint32_t kVersion = 2;   // 2: EngineStats.parked, parkedBlocks
constexpr int kMaxEngines = 8;
constexpr int kFrames = 128;               // one Move block
constexpr int kSlots = 8;                  // power of two, > max depth
constexpr int kMaxDepth = 4;
constexpr int kCmdSlots = 1024;            // power of two

enum class Op : uint16_t {
    None = 0,
    NoteOn,        // a = note
    NoteOff,       // a = note
    AllNotesOff,
    SetMachine,    // a = machine index (host::Machine)
    SetParam,      // a = page (0 SYN 1 AMP 2 FILT 3 EFX), b = index 0..7, c = value 0..127
    SetLfo,        // a = lfo 0..2, b = index 0..7, c = value
    SetLevel,      // c = 0..127
    SetBpm,        // c = bpm x 100
    SetRouting,    // c = routing bits (host::fxInputBits / dspInputBits)
    Reset,         // back to a fresh voice (compiled code kept)
};

struct Cmd {
    uint16_t op;
    uint8_t engine;
    uint8_t a;
    uint8_t b;
    uint8_t pad[3];
    int32_t c;
};
static_assert(sizeof(Cmd) == 12);

enum class Phase : uint32_t { Spawning = 0, Loading, Prewarming, Ready, Failed };

struct EngineStats {
    std::atomic<uint32_t> lastUs, maxUs;   // per 128-frame block
    std::atomic<uint32_t> blocks;
    std::atomic<uint32_t> faulted;
    std::atomic<uint32_t> parked;          // 1 while the engine sleeps (idle: DSP skipped)
    std::atomic<uint32_t> parkedBlocks;    // total blocks skipped
};

struct Segment {
    // ---- header, written once by the plugin before spawning
    uint32_t magic, version;
    uint32_t engines;                 // 1..kMaxEngines
    uint32_t hasInput;                // FX: the callback writes input slots
    std::atomic<uint32_t> depth;      // blocks rendered ahead, 1..kMaxDepth (live-adjustable)

    // ---- clock: the callback increments host_block once per block and futex-wakes the child on it
    alignas(64) std::atomic<uint32_t> host_block;
    alignas(64) std::atomic<uint32_t> produced;   // child: blocks published (index of next block + 1)
    alignas(64) std::atomic<uint32_t> underruns;  // callback
    std::atomic<uint32_t> skips;                  // child: blocks skipped to catch up
    std::atomic<uint32_t> heartbeat;              // child: bumped every loop iteration
    std::atomic<uint32_t> shutdown;               // plugin -> child

    // ---- boot status (child)
    std::atomic<uint32_t> phase;
    std::atomic<uint32_t> child_pid;
    char error[160];

    // ---- commands (callback -> child)
    alignas(64) std::atomic<uint32_t> cmd_write;
    alignas(64) std::atomic<uint32_t> cmd_read;
    Cmd cmds[kCmdSlots];

    EngineStats stats[kMaxEngines];

    // ---- audio: 24-bit samples in int32, L/R interleaved
    alignas(64) int32_t out[kSlots][kMaxEngines][kFrames * 2];
    int32_t in[kSlots][kFrames * 2];              // FX input for block b, written by the callback before
                                                  // it bumps host_block past b - depth
};

// Callback side: never blocks. Returns false (and drops the command) when the ring is full.
inline bool pushCmd(Segment& s, const Cmd& c)
{
    const uint32_t w = s.cmd_write.load(std::memory_order_relaxed);
    if (w - s.cmd_read.load(std::memory_order_acquire) >= uint32_t(kCmdSlots)) return false;
    s.cmds[w & (kCmdSlots - 1)] = c;
    s.cmd_write.store(w + 1, std::memory_order_release);
    return true;
}

} // namespace mnm::shm
