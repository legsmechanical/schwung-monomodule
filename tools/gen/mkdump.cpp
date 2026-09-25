// Writes a small Monomachine kit dump for tests (upstream's own encoder): mkdump <out.syx>
// Kit "TESTKIT": track 1 SID (WAVE = SAW, TUNE +5), track 2 FM+ PAR, track 3 REVERB (DEC 100), rest GND.
// Kit "COPY" repeats track 1 exactly (must be listed once).
#include <cstdio>
#include <cstring>
#include <vector>
#include "library/MnmDump.h"

using namespace mnm::dump;

static void setName(Kit& k, const char* n)
{
    k.name = n;
    std::memset(k.nameRaw, 0, sizeof k.nameRaw);
    std::memcpy(k.nameRaw, n, std::strlen(n));
}

int main(int argc, char** argv)
{
    if (argc < 2) return 2;
    std::vector<uint8_t> out;
    for (int kitNo = 0; kitNo < 2; ++kitNo) {
        Kit k;
        k.position = kitNo;
        setName(k, kitNo == 0 ? "TESTKIT" : "COPY");
        for (auto& t : k.tracks) { t.model = 0; t.level = 100; for (int i = 0; i < 72; ++i) t.params[i] = 0; }
        auto& sid = k.tracks[0];
        sid.model = 3; sid.level = 110;
        const uint8_t sidSyn[8] = {10, 0, 96, 38 /* SAW */, 0, 0, 64, 69 /* TUNE +5 */};
        std::memcpy(sid.params, sidSyn, 8);
        const uint8_t amp[8] = {0, 0, 64, 64, 64, 64, 64, 0}, filt[8] = {0, 127, 0, 0, 0, 32, 64, 64}, efx[8] = {64, 64, 0, 64, 64, 28, 0, 127};
        std::memcpy(sid.params + 8, amp, 8); std::memcpy(sid.params + 16, filt, 8); std::memcpy(sid.params + 24, efx, 8);
        const uint8_t lfo[8] = {0, 64, 0, 0, 1, 64, 0, 0};
        for (int l = 0; l < 3; ++l) std::memcpy(sid.params + 32 + 8 * l, lfo, 8);
        if (kitNo == 0) {
            auto& fm = k.tracks[1];
            fm.model = 9; fm.level = 90;
            const uint8_t fmSyn[8] = {60, 64, 80, 64, 102, 80, 98, 64};
            std::memcpy(fm.params, fmSyn, 8); std::memcpy(fm.params + 8, amp, 8); std::memcpy(fm.params + 16, filt, 8); std::memcpy(fm.params + 24, efx, 8);
            auto& rev = k.tracks[2];
            rev.model = 13; rev.level = 100;
            const uint8_t revSyn[8] = {100, 2, 127, 32, 0, 127, 0, 64};
            std::memcpy(rev.params, revSyn, 8); std::memcpy(rev.params + 8, amp, 8);
        }
        const auto bytes = encodeKit(k);
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    FILE* f = std::fopen(argv[1], "wb");
    if (!f) return 1;
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    const auto back = parseDump(out.data(), out.size(), argv[1]);
    std::printf("wrote %zu bytes, %zu kits parse back: %s / %s, track 1 model %d\n", out.size(), back.kits.size(),
                back.kits.size() > 0 ? back.kits[0].name.c_str() : "?", back.kits.size() > 1 ? back.kits[1].name.c_str() : "?",
                back.kits.empty() ? -1 : back.kits[0].tracks[0].model);
    return 0;
}
