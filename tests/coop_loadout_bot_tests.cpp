#include "../src/coop_loadout_sync.h"

#include <cstdio>
#include <cstdlib>

namespace {
int failures = 0;
void Check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

coop_loadout::Entry Gun(uint32_t a) {
    coop_loadout::Entry e;
    e.guid[0] = a; e.guid[1] = 0x11; e.guid[2] = 0x22; e.guid[3] = 0x33;
    e.have = true;
    return e;
}

void RoundTrip() {
    coop_loadout::Sender sender;
    sender.token = 0xABCD;
    const coop_loadout::Entry carried[2] = {Gun(0x1000), Gun(0x2000)};
    sender.Publish(carried, 2, 100);
    coop_loadout::Receiver receiver;
    bool complete = false;
    uint32_t now = 200;
    while (sender.Due(now)) {
        const auto wire = coop_loadout::ToBot(sender.MakePacket(now), 2);
        Check(coop_loadout::ValidBot(wire), "a bot packet from the sender is valid");
        Check(wire.bot == 2, "the bot index travels");
        if (receiver.Accept(coop_loadout::FromBot(wire), 0, now)) complete = true;
        now += 60;
    }
    Check(complete, "the receiver completes from bot packets");
    Check(receiver.generation == 1, "one generation for one loadout");
    Check(receiver.items[1].guid[0] == 0x2000, "slot 1 keeps its GUID");
}

void Rejects() {
    coop_loadout::Sender sender;
    sender.token = 7;
    const coop_loadout::Entry carried[1] = {Gun(0x3000)};
    sender.Publish(carried, 1, 0);
    auto wire = coop_loadout::ToBot(sender.MakePacket(100), 0);
    auto bad = wire;
    bad.magic = coop_loadout::kMagic;
    Check(!coop_loadout::ValidBot(bad), "a peer magic is not a bot packet");
    bad = wire;
    bad.bot = coop_loadout::kMaxBots;
    Check(!coop_loadout::ValidBot(bad), "a bot index out of range is refused");
    bad = wire;
    bad.slot = bad.slots;
    Check(!coop_loadout::ValidBot(bad), "a slot past the count is refused");
}

void SameEpochResweep() {
    // A periodic re-sweep keeps the epoch: a receiver that already has it gains
    // no new generation, a fresh one completes.
    coop_loadout::Sender sender;
    sender.token = 9;
    const coop_loadout::Entry carried[1] = {Gun(0x4000)};
    sender.Publish(carried, 1, 0);
    coop_loadout::Receiver early, late;
    early.Accept(coop_loadout::FromBot(coop_loadout::ToBot(sender.MakePacket(100), 1)), 0, 100);
    sender.sweeps = 1;
    sender.cursor = 0;
    const auto again = coop_loadout::FromBot(coop_loadout::ToBot(sender.MakePacket(5200), 1));
    Check(!early.Accept(again, 0, 5200), "a re-sweep is no new generation");
    Check(early.generation == 1, "still generation 1");
    Check(late.Accept(again, 0, 5200), "a late receiver completes from the re-sweep");
}
}  // namespace

int main() {
    RoundTrip();
    Rejects();
    SameEpochResweep();
    if (failures) {
        std::printf("bot loadout tests: %d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("bot loadout tests passed\n");
    return EXIT_SUCCESS;
}
