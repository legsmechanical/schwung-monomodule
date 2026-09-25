// Disassembles DSP program memory of an initialised engine: mnm-disasm <os.syx> <from-hex> <count>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <functional>
#include "MonoVoice.h"
#include "firmware/Firmware.h"
#include "dsp56kEmu/disasm.h"
#include "dsp56kEmu/opcodes.h"
#include "dsp56kEmu/dsp.h"

// Lists every DO/REP whose body is nothing but NOPs (or nested such loops): padding the Monomachine kernel
// burns to keep its timing on the real chip. Candidates only: whether each is code or data is checked by hand.
static int scanIdleLoops(const char* os)
{
    const auto fw = mnm::fw::loadFirmware(os);
    mnm::MonoVoice v(fw);
    dsp56k::Opcodes ops;
    dsp56k::Disassembler d(ops);
    auto P = [&](uint32_t a) { return v.engine().peek(mnm::fw::Space::P, a); };
    auto text = [&](uint32_t a, uint32_t& len) { std::string s; len = d.disassemble(s, P(a), P(a + 1), 0, 0, a); if (!len) len = 1; return s; };
    // body [from, to) contains only nops and pure DO loops
    std::function<bool(uint32_t, uint32_t)> pure = [&](uint32_t from, uint32_t to) {
        for (uint32_t a = from; a < to;) {
            if (P(a) == 0) { ++a; continue; }
            uint32_t len; const auto t = text(a, len);
            const auto gt = t.find(">$");
            if (t.rfind("do ", 0) == 0 && gt != std::string::npos) {
                const uint32_t end = uint32_t(std::strtoul(t.c_str() + gt + 2, nullptr, 16));
                if (end <= a + len || end > to || !pure(a + len, end)) return false;
                a = end; continue;
            }
            return false;
        }
        return true;
    };
    for (const auto* img : {&fw.kernelA, &fw.payload})
        for (const auto& r : img->records) {
            if (r.space != mnm::fw::Space::P) continue;
            for (uint32_t a = r.addr; a < r.addr + r.words.size(); ++a) {
                uint32_t len; const auto t = text(a, len);
                const auto gt = t.find(">$");
                if (t.rfind("do ", 0) == 0 && gt != std::string::npos) {
                    const uint32_t end = uint32_t(std::strtoul(t.c_str() + gt + 2, nullptr, 16));
                    if (end > a + len && end - a < 0x200 && pure(a + len, end))
                        std::printf("DO  %06x: %06x %06x  %-28s body %u words\n", a, P(a), P(a + 1), t.c_str(), end - a - len);
                } else if (t.rfind("rep ", 0) == 0 && P(a + len) == 0) {
                    std::printf("REP %06x: %06x         %s ; nop\n", a, P(a), t.c_str());
                }
            }
        }
    return 0;
}

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[2]) == "--idle-loops") return scanIdleLoops(argv[1]);
    if (argc < 4) { std::fprintf(stderr, "usage: mnm-disasm <os.syx> <from-hex> <count>   |   mnm-disasm <os.syx> --idle-loops\n"); return 2; }
    const auto fw = mnm::fw::loadFirmware(argv[1]);
    mnm::MonoVoice v(fw);
    dsp56k::Opcodes ops;
    dsp56k::Disassembler d(ops);
    uint32_t pc = uint32_t(std::strtoul(argv[2], nullptr, 16));
    const uint32_t end = pc + uint32_t(std::atoi(argv[3]));
    while (pc < end) {
        const uint32_t a = v.engine().peek(mnm::fw::Space::P, pc), b = v.engine().peek(mnm::fw::Space::P, pc + 1);
        std::string s;
        const uint32_t n = d.disassemble(s, a, b, 0, 0, pc);
        std::printf("%06x: %06x  %s\n", pc, a, s.c_str());
        pc += n ? n : 1;
    }
}
