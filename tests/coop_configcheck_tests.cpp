// Tests for coop_configcheck.h: build_configcheck_tests.cmd [an INI to fingerprint]
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include "../src/coop_configcheck.h"

using namespace coop_config;

namespace {
const char* kIni =
    "; header\n"
    "[DS2Randomizer]\n"
    "Seed=70\n"
    "Chance=50\n"
    "Seed=99\n"
    "InstallDelayMs=3000\n"
    "[Coop]\n"
    "JoinAddress=1.2.3.4\n"
    "Network=1\n"
    "CatchUpKey=119\n"
    "PeerTimeoutMs=15000\n"
    "[MpMap]\n"
    "BlockTeamLogic=1\n"
    "LogSpawnLines=1\n"
    "GapTrace=0\n"
    "[Zone]\n"
    "RingEffect=stasis\n"
    "OpenObjectives=1\n"
    "[Turret]\n"
    "BodyGuid=4BC78C36-9D67757C-544E4543-53574F4B\n"
    "[DS2Randomizer]\n"
    "Chance=10\n";

void Reading() {
    const auto settings = ReadIni(kIni);
    // the first [DS2Randomizer] and its first Seed; the second section is not read
    unsigned seeds = 0, chances = 0;
    for (const auto& s : settings) {
        if (s.key == "Seed") { ++seeds; assert(s.value == "70"); }
        if (s.key == "Chance") { ++chances; assert(s.value == "50"); }
    }
    assert(seeds == 1 && chances == 1);
    std::printf("reading ok\n");
}

void Personal_() {
    assert(Personal("coop", "joinaddress") && Personal("coop", "network") && Personal("coop", "catchupkey") && Personal("nameplate", "name"));
    assert(Personal("mpmap", "logspawnlines") && Personal("mpmap", "gaptrace") && Personal("zone", "ringeffect") && Personal("ds2randomizer", "installdelayms"));
    assert(!Personal("mpmap", "blockteamlogic") && Personal("ds2randomizer", "seed") && !Personal("coop", "peertimeoutms") && !Personal("zone", "openobjectives"));
    const Fingerprint f = Make(kIni);
    // Chance, PeerTimeoutMs, BlockTeamLogic, OpenObjectives, BodyGuid (the seed is the host's in a session)
    assert(f.entries.size() == 5 && f.personal == 8);
    std::printf("personal ok\n");
}

void Severities() {
    assert(Severity("objective", "prophealthfactor") == 1 && Severity("objective", "holdperplayerpercent") == 1);
    assert(Severity("objective", "defendradius") == 0 && Severity("mercenaries", "killms") == 0);
    std::printf("severity ok\n");
}

void Digest() {
    const Fingerprint a = Make(kIni);
    std::string personal = kIni;
    personal.replace(personal.find("JoinAddress=1.2.3.4"), 19, "JoinAddress=9.9.9.9");
    personal.replace(personal.find("CatchUpKey=119"), 14, "CatchUpKey=120");
    assert(Make(personal).digest == a.digest);
    std::string lower = kIni;
    lower.replace(lower.find("4BC78C36-9D67757C"), 17, "4bc78c36-9d67757c");
    assert(Make(lower).digest == a.digest);   // GUIDs and hex read the same in either case
    std::string seed = kIni;
    seed.replace(seed.find("Seed=70"), 7, "Seed=71");
    assert(Make(seed).digest == a.digest);   // a client plays the host's seed
    const std::string bench = "[BenchRandomizer]\nStatShuffle=1\n";
    assert(Make(seed + bench).digest != Make(personal + bench).digest);
    assert(Make(seed + bench).severity.at(Fnv("ds2randomizer.seed")) == 2);
    assert(Make(seed + bench + "Seed=0\n").digest == Make(personal + bench + "Seed=0\n").digest);
    assert(Make("[Coop]\nSeedSync=0\n[DS2Randomizer]\nSeed=1\n").digest != Make("[Coop]\nSeedSync=0\n[DS2Randomizer]\nSeed=0\n").digest);
    std::string chance = kIni;
    chance.replace(chance.find("Chance=50"), 9, "Chance=51");
    const Fingerprint b = Make(chance);
    assert(b.digest != a.digest);
    const auto diff = Compare(a.entries, b.entries);
    assert(diff.size() == 1 && diff[0].kind == 0 && a.names.at(diff[0].key) == "[DS2Randomizer] Chance = 50");
    std::string extra = kIni;
    extra += "[Marks]\nEnabled=1\n";
    const auto more = Compare(a.entries, Make(extra).entries);
    assert(more.size() == 1 && more[0].kind == 2);
    const auto fewer = Compare(Make(extra).entries, a.entries);
    assert(fewer.size() == 1 && fewer[0].kind == 1);
    std::printf("digest ok\n");
}

void Parts() {
    std::string big = "[Big]\n";
    for (int i = 0; i < 130; ++i) big += "Value" + std::to_string(i) + "=" + std::to_string(i * 7) + "\n";
    const Fingerprint f = Make(big);
    assert(f.entries.size() == 130 && PartsFor(f) == 3);
    Assembly a;
    const unsigned order[] = {2, 0, 2, 1};
    bool whole = false;
    for (unsigned part : order) whole = a.Add(MakePart(f, part, 0x1234, 0xABCD));
    assert(whole && a.build == 0xABCD && a.entries.size() == 130 && Compare(f.entries, a.entries).empty());
    // a part of another list starts over
    std::string changed = big;
    changed.replace(changed.find("Value0=0"), 8, "Value0=1");
    const Fingerprint g = Make(changed);
    assert(!a.Add(MakePart(g, 0, 0x1234, 0xABCD)) && a.digest == g.digest && !a.Whole());
    Packet bad = MakePart(f, 0, 1, 1);
    bad.part = 9;
    Assembly b;
    assert(!b.Add(bad) && !b.Whole());
    std::printf("parts ok\n");
}

void RealFile(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) { std::printf("real file: %s not found, skipped\n", path); return; }
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const Fingerprint f = Make(text);
    std::printf("real file: %zu shared settings, %u personal, %u parts of %zu bytes, digest %08X\n", f.entries.size(), f.personal, PartsFor(f), sizeof(Packet),
                f.digest);
    assert(PartsFor(f) < kMaxParts);
}
}  // namespace

int main(int argc, char** argv) {
    Reading();
    Personal_();
    Severities();
    Digest();
    Parts();
    if (argc > 1) RealFile(argv[1]);
    std::printf("configcheck tests passed\n");
    return 0;
}
