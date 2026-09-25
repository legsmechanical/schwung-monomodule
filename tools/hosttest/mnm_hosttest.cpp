// Loads a Monomodule plugin the way the Schwung chain host does and drives it from a simulated audio
// callback (128 frames every 2.9 ms), outside Move. Exercises boot, notes, machine switches, a killed
// engine process (respawn + state replay) and unload.
//
//   mnm-hosttest <module-dir> [seconds-per-phase=4] [late-os | destroy-in-boot]
//
// late-os: the OS file is moved away before create and put back 3 s later (upload after adding the module).
// destroy-in-boot: destroy while the engine is still pre-warming.
// park: a note, 4 s of silence (the engine must sleep), then a note (it must wake and sound).
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

    const std::string mode = argc > 3 ? argv[3] : "";
    const std::string osf = dir + "/os/Elektron_SFX6-60_OS1.32B.syx";
    if (mode == "late-os") std::rename(osf.c_str(), (osf + ".away").c_str());
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

    if (mode == "destroy-in-boot") {
        run(0.2, "booting");
        const auto st = get("status");
        const int pid = std::atoi(st.c_str() + st.find("\"pid\":") + 6);
        fx ? afx->destroy_instance(inst) : syn->destroy_instance(inst);
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        std::printf("destroy during boot (pid %d): engine %s\n", pid, pid > 0 && kill(pid, 0) == 0 ? "STILL RUNNING" : "gone");
        return 0;
    }
    if (mode == "late-os") {
        char e[256] = {};
        run(3, "no OS file");
        (fx ? nullptr : syn->get_error) ? syn->get_error(inst, e, sizeof e) : 0;
        std::printf("error while missing: %s\n", e);
        std::rename((osf + ".away").c_str(), osf.c_str());
    }
    t0 = Clock::now();
    while (get("status").find("\"phase\":3") == std::string::npos) {
        run(0.1, "");
        if (std::chrono::duration<double>(Clock::now() - t0).count() > 20) { std::printf("BOOT TIMEOUT: %s / error: %s\n", get("status").c_str(), ""); return 4; }
    }
    std::printf("boot to ready: %.0f ms\n", std::chrono::duration<double, std::milli>(Clock::now() - t0).count());

    if (mode == "presets") {   // <dir>/dumps holds test_kits.syx (tools/gen/mkdump.cpp)
        int fails = 0;
        auto expect = [&](const char* k, const char* want) {
            const std::string got = get(k);
            const bool ok = got == want;
            if (!ok) ++fails;
            std::printf("  %-14s = %-16s %s\n", k, got.c_str(), ok ? "ok" : (std::string("WANT ") + want).c_str());
        };
        run(4, "waiting for the dump scan");
        std::printf("  preset_count = %s\n", get("preset_count").c_str());
        const int count = std::atoi(get("preset_count").c_str());
        for (int i = 0; i < count; ++i) { set("preset", std::to_string(i).c_str()); std::printf("   [%d] %s\n", i, get("preset_name").c_str()); }
        if (!fx) {
            if (count != 17) { ++fails; std::printf("  WANT 17 presets\n"); }
            set("preset", "15"); expect("preset_name", "TESTKIT 1"); expect("machine", "SID 6581");
            expect("sid_wave", "SAW"); expect("sid_tune", "5"); expect("level", "110");
            set("preset", "0"); expect("preset_name", "Init GND"); expect("machine", "GND");
            set("preset", "12"); expect("machine", "FM+ PAR"); expect("fmp_1frq", "1/2");
            midi(0x90, 48, 100); run(1, "Init FM+ PAR plays"); midi(0x80, 48, 0);
        } else {
            if (count != 8) { ++fails; std::printf("  WANT 8 presets\n"); }
            set("preset", "7"); expect("preset_name", "TESTKIT 3"); expect("machine", "REVERB"); expect("rev_dec", "100");
            run(1, "REVERB preset on a sine");
        }
        std::printf("%s\n", fails ? "PRESETS FAIL" : "PRESETS PASS");
        fx ? afx->destroy_instance(inst) : syn->destroy_instance(inst);
        return fails ? 1 : 0;
    }
    if (mode == "ui") {   // the generated parameter surface
        int fails = 0;
        auto expect = [&](const char* k, const char* want) {
            const std::string got = get(k);
            const bool ok = got == want;
            if (!ok) ++fails;
            std::printf("  %-18s = %-8s %s\n", k, got.c_str(), ok ? "ok" : (std::string("WANT ") + want).c_str());
        };
        {
            static char big[131072];
            const int n = fx ? afx->get_param(inst, "ui_hierarchy", big, sizeof big) : syn->get_param(inst, "ui_hierarchy", big, sizeof big);
            const bool ok = n > 1000 && std::strstr(big, "\"levels\"") != nullptr;
            if (!ok) ++fails;
            std::printf("  ui_hierarchy: %d bytes %s\n", n, ok ? "ok" : "BAD");
        }
        set("machine", "SID 6581"); expect("machine", "SID 6581");
        expect("sid_wave", "TRI");
        set("sid_wave", "PULS"); expect("sid_wave", "PULS");
        set("sid_wave", "1"); expect("sid_wave", "SAW");          // by index
        set("sid_tune", "-12"); expect("sid_tune", "-12");        // bipolar
        set("amp_pan", "-64"); expect("amp_pan", "-64");
        set("amp_pan", "99"); expect("amp_pan", "63");            // clamped
        set("machine", "FM+ PAR"); expect("machine", "FM+ PAR");
        expect("fmp_1frq", "1/2");                                  // FM+ PAR default ratio (raw 60)
        expect("sid_wave", "SAW");                                  // remembered while another machine plays
        set("machine", "SID 6581"); expect("sid_wave", "SAW"); expect("sid_tune", "-12");
        set("machine", "12");                                       // by index: 13th synth machine
        std::printf("  machine by index 12 -> %s\n", get("machine").c_str());
        expect("lfo1_page", "PTCH"); expect("lfo1_dest_ptch", "2OCT");
        set("lfo1_page", "AMP"); expect("lfo1_dest_amp", "DIST");   // same raw slot (64 = 5th name), AMP page names
        set("lfo1_dest_amp", "PAN"); expect("lfo1_dest_amp", "PAN"); expect("lfo1_dest_ptch", "8OCT");
        set("fmd_1frq", ".999"); set("fmd_1frq", "1.5"); expect("fmd_1frq", "1.5");   // readout
        set("machine", "DPRO DDRW"); set("ddrw_wav1", "SIN"); expect("ddrw_wav1", "SIN");   // Digibank slot 33 by name
        set("ddrw_wav1", "32"); expect("ddrw_wav1", "SIN"); set("ddrw_wav2", "D05"); expect("ddrw_wav2", "D05");
        std::printf("  load = %s\n", get("load").c_str());
        const std::string st = get("state");
        std::printf("state: %s\n", st.c_str());
        set("amp_pan", "0"); set("state", st.c_str()); expect("amp_pan", "63");
        std::printf("%s\n", fails ? "UI FAIL" : "UI PASS");
        fx ? afx->destroy_instance(inst) : syn->destroy_instance(inst);
        return fails ? 1 : 0;
    }
    if (mode == "park") {   // sleep after silence, wake on a note
        auto field = [&](const char* k) { const auto st = get("status"); const auto p = st.find(std::string("\"") + k + "\":"); return p == std::string::npos ? -1 : std::atoi(st.c_str() + p + std::strlen(k) + 3); };
        if (!fx) { midi(0x90, 48, 100); run(0.5, "note"); midi(0x80, 48, 0); }
        run(4, "silence 4 s");
        std::printf("parked %d, parked_blocks %d\n", field("parked"), field("parked_blocks"));
        auto cpuTicks = [&](int pid) {   // utime + stime of the engine process, in clock ticks
            char path[64]; std::snprintf(path, sizeof path, "/proc/%d/stat", pid);
            FILE* f = std::fopen(path, "r"); if (!f) return -1L;
            char buf[1024] = {}; std::fread(buf, 1, sizeof buf - 1, f); std::fclose(f);
            const char* p = std::strrchr(buf, ')'); long ut = 0, st = 0; int field = 2;
            for (const char* q = p + 1; *q && field < 15; ++q) if (*q == ' ') { ++field; if (field == 14) ut = std::atol(q + 1); if (field == 15) st = std::atol(q + 1); }
            return ut + st;
        };
        const int pid = field("pid");
        const long hz = sysconf(_SC_CLK_TCK);
        long c0 = cpuTicks(pid); run(5, "asleep 5 s"); long c1 = cpuTicks(pid);
        std::printf("engine CPU while asleep: %.1f%% of a core\n", 100.0 * (c1 - c0) / hz / 5);
        if (!fx) midi(0x90, 48, 100);
        c0 = cpuTicks(pid); run(5, "playing 5 s"); c1 = cpuTicks(pid);
        std::printf("engine CPU while playing: %.1f%% of a core\n", 100.0 * (c1 - c0) / hz / 5);
        if (!fx) midi(0x80, 48, 0);
        if (!fx) { midi(0x90, 52, 100); run(0.5, "note after sleep"); midi(0x80, 52, 0); }
        std::printf("parked %d after a note\n", field("parked"));
        fx ? afx->destroy_instance(inst) : syn->destroy_instance(inst);
        return 0;
    }
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
    const auto st = get("status");
    const int pid = std::atoi(st.c_str() + st.find("\"pid\":") + 6);
    t0 = Clock::now();
    fx ? afx->destroy_instance(inst) : syn->destroy_instance(inst);
    std::printf("destroy_instance: %.1f ms\n", std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    std::printf("engine pid %d after destroy: %s\n", pid, pid > 0 && kill(pid, 0) == 0 ? "STILL RUNNING" : "gone");
    dlclose(so);
    {   // the plugin pins itself (RTLD_NODELETE) so its supervisor thread can outlive dlclose
        FILE* f = std::fopen("/proc/self/maps", "r"); char line[512]; bool mapped = false;
        while (f && std::fgets(line, sizeof line, f)) if (std::strstr(line, fx ? "monomodule-fx.so" : "dsp.so")) mapped = true;
        if (f) std::fclose(f);
        std::printf("after dlclose the plugin is %s\n", mapped ? "still mapped (pinned)" : "UNMAPPED (pin failed)");
    }
    return 0;
}
