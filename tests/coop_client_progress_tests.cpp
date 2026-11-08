#include "../src/coop_client_progress.h"

#include <cstdio>
#include <string>

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}
const char* const kGuns[] = {"PlasmaCutter", "PulseRifle", "LineGun", "Javelin"};

coop_client_progress::File Sample() {
    coop_client_progress::File f;
    f.host = 0x0123456789ABCDEFull;
    std::snprintf(f.map, sizeof(f.map), "%s", "ds_2");
    f.credits = 12000;
    f.nodes = 4;
    f.guns = (1u << 0) | (1u << 2);
    f.hasSuit = true;
    f.suit[0] = 0x38CB666F; f.suit[1] = 0xE989A6F7; f.suit[2] = 0x54524542; f.suit[3] = 0x314D4152;
    f.stackCount = 2;
    f.stacks[0] = {{0x39CABFB9, 0x12345678, 0x9ABCDEF0, 0x11223344}, 3};
    f.stacks[1] = {{0x44CA38A8, 0x13B97BEA, 0x4954414E, 0x41444954}, 75};
    f.treeCount = 2;
    f.trees[0].key = 0x1A2B3C4D;
    f.trees[0].count = 2;
    f.trees[0].nodes[0] = 0x5F6E7D8C;
    f.trees[0].nodes[1] = 0x01020304;
    f.trees[1].key = 0xCAFEBABE;
    f.trees[1].count = 1;
    f.trees[1].nodes[0] = 0xDEADBEEF;
    return f;
}

void RoundTrip() {
    const auto f = Sample();
    const std::string text = coop_client_progress::Write(f, kGuns, 4);
    coop_client_progress::File back;
    unsigned unknown = 0;
    const auto e = coop_client_progress::Parse(text.data(), text.size(), kGuns, 4, back, &unknown);
    Check(e == coop_client_progress::Error::None, "round trip parses");
    Check(back.host == f.host, "host");
    Check(std::string(back.map) == "ds_2", "map");
    Check(back.credits == 12000 && back.nodes == 4, "credits and nodes");
    Check(back.guns == f.guns, "guns");
    Check(back.hasSuit && back.suit[0] == 0x38CB666F && back.suit[3] == 0x314D4152, "suit");
    Check(back.stackCount == 2 && back.stacks[1].amount == 75 && back.stacks[1].guid[2] == 0x4954414E, "stacks");
    Check(back.treeCount == 2 && back.trees[0].count == 2 && back.trees[0].nodes[1] == 0x01020304 && back.trees[1].nodes[0] == 0xDEADBEEF, "trees");
    Check(unknown == 0, "no unknown guns");
}

void Tamper() {
    std::string text = coop_client_progress::Write(Sample(), kGuns, 4);
    const size_t at = text.find("credits 12000");
    text.replace(at, 13, "credits 99999");
    coop_client_progress::File back;
    Check(coop_client_progress::Parse(text.data(), text.size(), kGuns, 4, back) == coop_client_progress::Error::Checksum, "tampered file refused");
}

void UnknownGun() {
    const auto f = Sample();
    const std::string text = coop_client_progress::Write(f, kGuns, 4);
    const char* const fewer[] = {"PlasmaCutter", "PulseRifle", nullptr, "Javelin"};
    coop_client_progress::File back;
    unsigned unknown = 0;
    Check(coop_client_progress::Parse(text.data(), text.size(), fewer, 4, back, &unknown) == coop_client_progress::Error::None, "unknown gun still parses");
    Check(unknown == 1 && back.guns == 1u, "unknown gun skipped and counted");
}

void Refusals() {
    coop_client_progress::File back;
    Check(coop_client_progress::Parse("", 0, kGuns, 4, back) == coop_client_progress::Error::Empty, "empty");
    const char* other = "DS2BotSave 1\nend 00000000\n";
    Check(coop_client_progress::Parse(other, std::strlen(other), kGuns, 4, back) == coop_client_progress::Error::NotOurs, "not ours");
    std::string text = coop_client_progress::Write(Sample(), kGuns, 4);
    text.resize(text.size() / 2);
    Check(coop_client_progress::Parse(text.data(), text.size(), kGuns, 4, back) != coop_client_progress::Error::None, "truncated file refused");
}

void ReadForHost() {
    using namespace coop_client_progress;
    File held = Sample();
    const uint64_t host = held.host + 1;
    auto missing = [](unsigned, File&) { return 1; };
    Check(ReadForHost(host, held, missing) == 1 && held.host == 0 && held.guns == 0, "no inventory carried across hosts");
    auto wrong = [](unsigned copy, File& f) { f = Sample(); return copy ? 1 : 0; };
    Check(ReadForHost(host, held, wrong) == -1 && held.host == 0, "another host's file and missing backup");
    auto recovered = [&](unsigned copy, File& f) { f = Sample(); f.host = host; return copy ? 0 : -1; };
    Check(ReadForHost(host, held, recovered) == 0 && held.host == host && held.credits == 12000, "valid backup restores the requested host");
}
void Empty() {
    coop_client_progress::File f;
    f.host = 1;
    const std::string text = coop_client_progress::Write(f, kGuns, 4);
    coop_client_progress::File back;
    Check(coop_client_progress::Parse(text.data(), text.size(), kGuns, 4, back) == coop_client_progress::Error::None, "empty progress parses");
    Check(!back.hasSuit && !back.stackCount && !back.treeCount && !back.guns, "empty progress stays empty");
    Check(coop_client_progress::FileName(0x0123456789ABCDEFull) == "host_0123456789ABCDEF.ds2client", "file name");
}
}  // namespace

int main() {
    RoundTrip();
    Tamper();
    UnknownGun();
    Refusals();
    Empty();
    ReadForHost();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("coop_client_progress tests passed\n");
    return 0;
}
