// Dumps upstream Monomodule's display spec (SpecData.cpp: every machine's knob labels, display types,
// defaults and value names, the shared AMP/FILT/EFX pages and the LFO page) as JSON, for gen_ui.py.
//   c++ -std=c++17 -I<upstream>/src/plugin/one spec_dump.cpp <upstream>/src/plugin/one/SpecData.cpp
#include <cstdio>
#include <string>
#include "SpecData.h"

using namespace mnm::uispec;

// RomArt.h binds its fonts to art() at static init; this tool draws nothing, so an empty Art will do.
namespace mnm::uispec { Art& art() { static Art a{}; return a; } }

static std::string q(const char* s)
{
    std::string o = "\"";
    for (; s && *s; ++s) { if (*s == '"' || *s == '\\') o += '\\'; o += *s; }
    return o + "\"";
}

static const char* displayName(Display d)
{
    switch (d) {
    case Display::Blank: return "blank";
    case Display::Numeric: return "numeric";
    case Display::Bipolar: return "bipolar";
    case Display::List: return "list";
    case Display::Readout: return "readout";
    }
    return "?";
}

static void param(const Param& p)
{
    std::printf("{\"label\":%s,\"display\":\"%s\",\"default\":%d,\"count\":%d", q(p.label).c_str(), displayName(p.display), p.defaultRaw, p.valueCount);
    if (p.values) {
        std::printf(",\"values\":[");
        for (int i = 0; i < p.valueCount; ++i) std::printf("%s%s", i ? "," : "", q(p.values[i]).c_str());
        std::printf("]");
    }
    std::printf("}");
}

int main()
{
    std::printf("{\"machines\":[");
    for (int m = 0; m < kNumMachines; ++m) {
        const auto& M = kMachines[m];
        std::printf("%s{\"index\":%d,\"group\":%s,\"name\":%s,\"display\":%s,\"fx\":%s,\"params\":[", m ? "," : "", M.index, q(M.group).c_str(),
                    q(M.name).c_str(), q(M.displayName).c_str(), M.isFx ? "true" : "false");
        for (int k = 0; k < 8; ++k) { if (k) std::printf(","); param(M.params[k]); }
        std::printf("]}");
    }
    std::printf("],\"shared\":[");
    for (int s = 0; s < 3; ++s) {
        const auto& P = kSharedPages[s];
        std::printf("%s{\"name\":%s,\"labels\":[", s ? "," : "", q(P.name).c_str());
        for (int k = 0; k < 8; ++k) std::printf("%s%s", k ? "," : "", q(P.labels[k]).c_str());
        std::printf("],\"defaults\":[");
        for (int k = 0; k < 8; ++k) std::printf("%s%d", k ? "," : "", P.defaults[k]);
        std::printf("],\"bipolarMask\":%d}", P.bipolarMask);
    }
    std::printf("],\"ampDefaultsFx\":[");
    for (int k = 0; k < 8; ++k) std::printf("%s%d", k ? "," : "", kAmpDefaultsFx[k]);
    std::printf("],\"lfo\":[");
    for (int k = 0; k < 8; ++k) { if (k) std::printf(","); param(kLfoParams[k]); }
    std::printf("],\"lfoDest\":[");
    for (int pg = 0; pg < 9; ++pg) {
        std::printf("%s[", pg ? "," : "");
        for (int d = 0; d < 8; ++d) std::printf("%s%s", d ? "," : "", q(kLfoDestNames[pg][d]).c_str());
        std::printf("]");
    }
    std::printf("]}\n");
    return 0;
}
