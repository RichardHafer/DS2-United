// Dead Space 2 United by Richard Hafer.
// The bots' per-save sidecar format, checked without a game.
#include <cassert>
#include <cstdio>
#include <string>
#include "../src/coop_bot_save.h"

namespace {
using namespace coop_bot_save;

const char* const kGuns[] = {"PlasmaCutter", "PulseRifle", "LineGun", "Javelin", "Ripper", "Detonator", "Flamethrower", "SeekerRifle", "ForceGun", "ContactBeam"};
constexpr int kGunCount = static_cast<int>(sizeof(kGuns) / sizeof(kGuns[0]));

File Sample() {
    File f;
    f.key.hash = 0x7B120DE1;
    f.key.bodyUsed = 0x3AC28;
    std::snprintf(f.key.map, sizeof(f.key.map), "ds_2");
    f.slot = 8;
    Bot& a = f.bots[0];
    a.present = true;
    std::snprintf(a.name, sizeof(a.name), "Vasquez");
    a.credits = 12345;
    a.nodes = 7;
    a.guns = (1u << 1) | (1u << 2) | (1u << 9);
    a.hasSuit = true;
    a.suit[0] = 0x38CB666F; a.suit[1] = 0xE989A6F7; a.suit[2] = 0x54524542; a.suit[3] = 0x314D4152;
    Bot& c = f.bots[2];
    c.present = true;
    std::snprintf(c.name, sizeof(c.name), "Harlan the Third");
    c.credits = 0;
    c.nodes = 0;
    return f;
}

void RoundTrip() {
    const File f = Sample();
    const std::string text = Write(f, kGuns, kGunCount);
    File back;
    unsigned unknown = 99;
    assert(Parse(text, kGuns, kGunCount, back, &unknown) == Error::None);
    assert(unknown == 0);
    assert(back.version == kVersion && back.slot == 8);
    assert(back.key.hash == f.key.hash && back.key.bodyUsed == f.key.bodyUsed && std::string(back.key.map) == "ds_2");
    assert(back.bots[0].present && back.bots[0].credits == 12345 && back.bots[0].nodes == 7 && back.bots[0].guns == f.bots[0].guns);
    assert(back.bots[0].hasSuit && back.bots[0].suit[1] == 0xE989A6F7 && back.bots[0].suit[3] == 0x314D4152);
    assert(std::string(back.bots[0].name) == "Vasquez");
    assert(!back.bots[1].present);
    assert(back.bots[2].present && !back.bots[2].hasSuit && back.bots[2].guns == 0 && back.bots[2].credits == 0);
    assert(std::string(back.bots[2].name) == "Harlan_the_Third");   // spaces would split the line
    // Writing what was read gives the same bytes.
    File again = back;
    std::snprintf(again.bots[2].name, sizeof(again.bots[2].name), "Harlan the Third");
    assert(Write(again, kGuns, kGunCount) == text);
}

void FileNameCarriesTheKey() {
    const File f = Sample();
    assert(FileName(8, f.key) == "slot08_7B120DE1_0003AC28.ds2bots");
    assert(FileName(1, f.key) == "slot01_7B120DE1_0003AC28.ds2bots");
}

void RejectsOtherVersions() {
    std::string text = Write(Sample(), kGuns, kGunCount);
    text.replace(0, 12, "DS2BotSave 2");
    File back;
    assert(Parse(text, kGuns, kGunCount, back) == Error::Version);
    assert(Parse("DS2BotSave x\nend 00000000\n", kGuns, kGunCount, back) == Error::Syntax);
}

void RejectsCorruptFiles() {
    const std::string good = Write(Sample(), kGuns, kGunCount);
    File back;
    assert(Parse("", kGuns, kGunCount, back) == Error::Empty);
    assert(Parse("[Bot]\nKey=1\n", kGuns, kGunCount, back) == Error::NotOurs);
    // One flipped digit: the checksum no longer fits.
    std::string flipped = good;
    const size_t at = flipped.find("credits=12345");
    flipped[at + 8] = '9';
    assert(Parse(flipped, kGuns, kGunCount, back) == Error::Checksum);
    // Cut off before the end line (a write that never finished).
    const std::string cut = good.substr(0, good.find("\nend ") + 1);
    assert(Parse(cut, kGuns, kGunCount, back) == Error::Checksum);
    // A name that contains "end " cannot pose as the end line.
    assert(Parse(good.substr(0, good.find("bot 3")) + "bot 3 name=Legend end 00000000\n", kGuns, kGunCount, back) == Error::Checksum);
    // A well formed checksum over a bad line is still refused.
    std::string body = "DS2BotSave 1\nkey 7B120DE1 0003AC28 ds_2\nslot 8\nbot 1 name=x credits=-5 nodes=0 guns=- suit=-\n";
    char end[32];
    std::snprintf(end, sizeof(end), "end %08X\n", static_cast<unsigned>(Fnv1a(body.data(), body.size())));
    assert(Parse(body + end, kGuns, kGunCount, back) == Error::Syntax);
    // Missing fields of a bot line, a second line for the same bot, a missing key line.
    body = "DS2BotSave 1\nkey 7B120DE1 0003AC28 ds_2\nslot 8\nbot 1 name=x credits=5 guns=- suit=-\n";
    std::snprintf(end, sizeof(end), "end %08X\n", static_cast<unsigned>(Fnv1a(body.data(), body.size())));
    assert(Parse(body + end, kGuns, kGunCount, back) == Error::Syntax);
    body = "DS2BotSave 1\nkey 7B120DE1 0003AC28 ds_2\nslot 8\nbot 1 name=x credits=5 nodes=1 guns=- suit=-\nbot 1 name=y credits=5 nodes=1 guns=- suit=-\n";
    std::snprintf(end, sizeof(end), "end %08X\n", static_cast<unsigned>(Fnv1a(body.data(), body.size())));
    assert(Parse(body + end, kGuns, kGunCount, back) == Error::Syntax);
    body = "DS2BotSave 1\nslot 8\n";
    std::snprintf(end, sizeof(end), "end %08X\n", static_cast<unsigned>(Fnv1a(body.data(), body.size())));
    assert(Parse(body + end, kGuns, kGunCount, back) == Error::Syntax);
    // A bad suit GUID.
    body = "DS2BotSave 1\nkey 7B120DE1 0003AC28 ds_2\nslot 8\nbot 1 name=x credits=5 nodes=1 guns=- suit=38CB666F:E989A6F7\n";
    std::snprintf(end, sizeof(end), "end %08X\n", static_cast<unsigned>(Fnv1a(body.data(), body.size())));
    assert(Parse(body + end, kGuns, kGunCount, back) == Error::Syntax);
}

void SkipsUnknownGuns() {
    std::string body = "DS2BotSave 1\nkey 00000001 00000002 -\nslot -1\nbot 2 name=- credits=10 nodes=2 guns=LineGun,RivetGun,Ripper suit=- extra=1\n";
    char end[32];
    std::snprintf(end, sizeof(end), "end %08X\n", static_cast<unsigned>(Fnv1a(body.data(), body.size())));
    File back;
    unsigned unknown = 0;
    assert(Parse(body + end, kGuns, kGunCount, back, &unknown) == Error::None);
    assert(unknown == 1);
    assert(back.bots[1].guns == ((1u << 2) | (1u << 4)));
    assert(back.slot == -1 && back.key.map[0] == 0);
}

// A wallet that never got its starting credits says so; an older file without the key had real wallets.
void KeepsTheStartingCreditsMark() {
    File f = Sample();
    f.bots[2].seeded = false;
    const std::string text = Write(f, kGuns, kGunCount);
    assert(text.find("seeded=0") != std::string::npos && text.find("seeded=1") != std::string::npos);
    File back;
    assert(Parse(text, kGuns, kGunCount, back) == Error::None);
    assert(back.bots[0].seeded && !back.bots[2].seeded);
    std::string body = "DS2BotSave 1\nkey 00000001 00000002 -\nslot 3\nbot 1 name=- credits=10 nodes=2 guns=- suit=-\n";
    char end[32];
    std::snprintf(end, sizeof(end), "end %08X\n", static_cast<unsigned>(Fnv1a(body.data(), body.size())));
    assert(Parse(body + end, kGuns, kGunCount, back) == Error::None);
    assert(back.bots[0].seeded && back.bots[0].credits == 10);
    body = "DS2BotSave 1\nkey 00000001 00000002 -\nslot 3\nbot 1 name=- credits=10 nodes=2 guns=- suit=- seeded=2\n";
    std::snprintf(end, sizeof(end), "end %08X\n", static_cast<unsigned>(Fnv1a(body.data(), body.size())));
    assert(Parse(body + end, kGuns, kGunCount, back) == Error::Syntax);
}

void CrLfStillReads() {
    std::string text = Write(Sample(), kGuns, kGunCount);
    // Only the trailing newline after the checksum may be CRLF; the body is covered by the checksum.
    text.insert(text.size() - 1, "\r");
    File back;
    assert(Parse(text, kGuns, kGunCount, back) == Error::None);
}
}  // namespace

int main() {
    RoundTrip();
    FileNameCarriesTheKey();
    RejectsOtherVersions();
    RejectsCorruptFiles();
    SkipsUnknownGuns();
    CrLfStillReads();
    KeepsTheStartingCreditsMark();
    std::printf("coop_bot_save_tests: all passed\n");
    return 0;
}
