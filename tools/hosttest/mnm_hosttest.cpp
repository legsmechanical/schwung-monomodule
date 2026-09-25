// Loads a Monomodule plugin the way the Schwung chain host does and drives it from a simulated audio
// callback (128 frames every 2.9 ms), outside Move. Exercises boot, notes, machine switches, a killed
// engine process (respawn + state replay) and unload.
//
//   mnm-hosttest <module-dir> [seconds-per-phase=4]
//
// The module dir holds module.json's files: dsp.so or monomodule-fx.so, mnm-engine, os/<OS file>.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <dlfcn.h>
#include <signal.h>
#include "host_api/plugin_api_v1.h"
#include "host_api/audio_fx_api_v2.h"

using Clock = std::chrono::steady_clock;

static float bpm() { return 120.f; }

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: mnm-hosttest <module-dir> [seconds]\n"); return 2; }
    const std::string dir = argv[1];
    const double secs = argc > 2 ? std::atof(argv[2]) : 4.0;
    host_api_v1_t host{};
    host.get_bpm = bpm;

    void* so = dlopen((dir + "/dsp.so").c_str(), RTLD_NOW | RTLD_LOCAL);
    const bool fx = so == nullptr;
    if (fx) so = dlopen((dir + "/monomodule-fx.so").c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!so) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 3; }
    plugin_api_v2_t* syn = nullptr; audio_fx_api_v2_t* afx = nullptr;
    if (!fx) syn = reinterpret_cast<move_plugin_init_v2_fn>(dlsym(so, MOVE_PLUGIN_INIT_V2_SYMBOL))(&host);
    else afx = reinterpret_cast<audio_fx_init_v2_fn>(dlsym(so, AUDIO_FX_INIT_V2_SYMBOL))(&host);

    auto t0 = Clock::now();
    void* inst = fx ? afx->create_instance(dir.c_str(), nullptr) : syn->create_instance(dir.c_str(), nullptr);
    std::printf("create_instance: %.2f ms\n", std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    auto get = [&](const char* k) { static char b[512]; b[0] = 0; int n = fx ? afx->get_param(inst, k, b, sizeof b) : syn->get_param(inst, k, b, sizeof b); if (n < 0) b[0] = 0; return std::string(b); };
    auto set = [&](const char* k, const char* v) { fx ? afx->set_param(inst, k, v) : syn->set_param(inst, k, v); };
    auto midi = [&](uint8_t a, uint8_t b, uint8_t c) { uint8_t m[3] = {a, b, c}; fx ? afx->on_midi(inst, m, 3, 0) : syn->on_midi(inst, m, 3, 0); };

    std::vector<int16_t> buf(256);
    double worstCallUs = 0;
    long block = 0;
    auto next = Clock::now();
    auto run = [&](double seconds, const char* label) {
        double sum2 = 0; long n = 0; int nonzero = 0;
        const long blocks = long(seconds * 44100 / 128);
        for (long b = 0; b < blocks; ++b, ++block) {
            next += std::chrono::microseconds(2902);
            std::this_thread::sleep_until(next);
            if (fx) for (int i = 0; i < 256; ++i) buf[size_t(i)] = int16_t(8000 * std::sin(block * 128 * 0.05 + i * 0.025));
            const auto s = Clock::now();
            if (fx) afx->process_block(inst, buf.data(), 128); else syn->render_block(inst, buf.data(), 128);
            worstCallUs = std::max(worstCallUs, std::chrono::duration<double, std::micro>(Clock::now() - s).count());
            for (int i = 0; i < 256; ++i) { sum2 += double(buf[size_t(i)]) * buf[size_t(i)]; n++; if (buf[size_t(i)]) ++nonzero; }
        }
        std::printf("%-28s rms %7.1f  nonzero %5.1f%%  status %s\n", label, std::sqrt(sum2 / std::max(1L, n)), 100.0 * nonzero / std::max(1L, n), get("status").c_str());
    };

    t0 = Clock::now();
    while (get("status").find("\"phase\":3") == std::string::npos) {
        run(0.1, "");
        if (std::chrono::duration<double>(Clock::now() - t0).count() > 20) { std::printf("BOOT TIMEOUT: %s / error: %s\n", get("status").c_str(), ""); return 4; }
    }
    std::printf("boot to ready: %.0f ms\n", std::chrono::duration<double, std::milli>(Clock::now() - t0).count());

    if (!fx) {
        midi(0x90, 48, 100); run(secs, "FM+ PAR note on");
        midi(0x80, 48, 0); run(1, "note off (tail)");
        set("machine", "SID 6581"); midi(0x90, 45, 100); run(secs, "switch to SID, note");
        set("syn1", "100"); set("syn3", "20"); run(1, "SID params");
        const std::string st = get("state");
        std::printf("state: %s\n", st.c_str());
        const auto pidPos = get("status").find("\"pid\":");
        const int pid = std::atoi(get("status").c_str() + pidPos + 6);
        std::printf("killing engine pid %d\n", pid);
        kill(pid, SIGKILL);
        t0 = Clock::now();
        run(0.3, "just after kill");
        while (get("status").find("\"phase\":3") == std::string::npos) run(0.1, "");
        std::printf("respawn to ready: %.0f ms\n", std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
        midi(0x90, 45, 100); run(secs, "after respawn, note");
        std::printf("machine after respawn: %s  syn1 %s  (state %s)\n", get("machine").c_str(), get("syn1").c_str(), get("state") == st ? "unchanged" : "CHANGED");
    } else {
        run(secs, "CHORUS on a sine");
        set("machine", "REVERB"); run(secs, "REVERB on a sine");
    }
    std::printf("worst callback: %.1f us\n", worstCallUs);
    t0 = Clock::now();
    fx ? afx->destroy_instance(inst) : syn->destroy_instance(inst);
    std::printf("destroy_instance: %.1f ms\n", std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    dlclose(so);
    return 0;
}
