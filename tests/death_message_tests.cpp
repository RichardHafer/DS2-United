#include "../src/death_message.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <string>

using ds2::DeathMessageResult;
using ds2::ScriptMessage;

namespace {
struct Memory {
    std::map<uint32_t, unsigned> words;
    std::map<uint32_t, std::string> strings;

    // Dwords, plus the 16-bit halves the reference counts live in: the entry's
    // count at +0x14 and the owner's at +6 are both read and written as words.
    bool read(uint32_t address, void* out, size_t size) {
        if (size == 2 && address % 2 == 0) {
            const auto found = words.find(address & ~3u);
            if (found == words.end()) return false;
            const uint16_t half = static_cast<uint16_t>(found->second >> ((address & 2) * 8));
            std::memcpy(out, &half, 2);
            return true;
        }
        if (size != 4 || address % 4) return false;
        const auto found = words.find(address);
        if (found == words.end()) return false;
        std::memcpy(out, &found->second, 4);
        return true;
    }
    bool write(uint32_t address, const void* in, size_t size) {
        if (size == 2 && address % 2 == 0) {
            uint16_t half = 0;
            std::memcpy(&half, in, 2);
            unsigned& word = words[address & ~3u];
            const unsigned shift = (address & 2) * 8;
            word = (word & ~(0xFFFFu << shift)) | (static_cast<unsigned>(half) << shift);
            return true;
        }
        if (size != 4 || address % 4) return false;
        unsigned value = 0;
        std::memcpy(&value, in, 4);
        words[address] = value;
        return true;
    }
    uint16_t half(uint32_t address) const {
        const auto found = words.find(address & ~3u);
        return found == words.end() ? 0 : static_cast<uint16_t>(found->second >> ((address & 2) * 8));
    }
    bool matches(unsigned value, const char* name) const {
        const auto found = strings.find(value);
        return found != strings.end() && found->second == name;
    }
};

Memory filled(uint32_t base, unsigned window, unsigned filler) {
    Memory memory;
    for (unsigned offset = 0; offset < window; offset += 4) memory.words[base + offset] = filler;
    return memory;
}

constexpr uint32_t kBase = 0x10000;
constexpr unsigned kWindow = 0x40;
constexpr uint32_t kOther = 0x20000;   // a second component of the same actor
constexpr uint32_t kDonorString = 0x900004;
constexpr uint32_t kSourceString = 0x900104;
}  // namespace

int main() {
    // The actor's components: the message may sit on any of them.
    ds2::MemoryRegion regions[] = {{kBase, kWindow}, {kOther, kWindow}};
    size_t regionCount = 2;

    const ScriptMessage wanted{"hospital_lobby_slasher_dead", 0x9F2E78E7u};
    const ScriptMessage carried{"mm_ellie_kill_01", 0x12345678u};

    auto memory = filled(kBase, kWindow, 0x1111);
    auto read = [&](uint32_t a, void* o, size_t s) { return memory.read(a, o, s); };
    auto write = [&](uint32_t a, const void* i, size_t s) { return memory.write(a, i, s); };
    auto matches = [&](unsigned v, const char* n) { return memory.matches(v, n); };
    auto address = [&](const char* name) -> unsigned {
        return std::strcmp(name, wanted.name) == 0 ? kSourceString : 0u;
    };

    // The header holds the hash: it is replaced with the one the level wants.
    memory.words[kBase + 0x18] = carried.hash;
    assert(ds2::transplantScriptMessage(regions, regionCount, wanted, carried, read, write, matches, address) == DeathMessageResult::WrittenAsHash);
    assert(memory.words[kBase + 0x18] == wanted.hash);
    for (unsigned offset = 0; offset < kWindow; offset += 4)
        if (offset != 0x18) assert(memory.words[kBase + offset] == 0x1111);

    // The header holds a pointer to the name: it is repointed at the source's.
    memory = filled(kBase, kWindow, 0x1111);
    memory.strings[kDonorString] = carried.name;
    memory.strings[kSourceString] = wanted.name;
    memory.words[kBase + 0x0C] = kDonorString;
    assert(ds2::transplantScriptMessage(regions, regionCount, wanted, carried, read, write, matches, address) == DeathMessageResult::WrittenAsPointer);
    assert(memory.words[kBase + 0x0C] == kSourceString);

    // The hash wins when both shapes are present, because it needs no allocation.
    memory = filled(kBase, kWindow, 0x1111);
    memory.strings[kDonorString] = carried.name;
    memory.words[kBase + 0x04] = kDonorString;
    memory.words[kBase + 0x24] = carried.hash;
    assert(ds2::transplantScriptMessage(regions, regionCount, wanted, carried, read, write, matches, address) == DeathMessageResult::WrittenAsHash);
    assert(memory.words[kBase + 0x04] == kDonorString);

    // A message that lives on another component of the same actor is found too -
    // MSG_AWARE and the spawn-done message do not sit on the health component.
    memory = filled(kBase, kWindow, 0x1111);
    for (unsigned offset = 0; offset < kWindow; offset += 4) memory.words[kOther + offset] = 0x2222;
    memory.words[kOther + 0x10] = carried.hash;
    assert(ds2::transplantScriptMessage(regions, regionCount, wanted, carried, read, write, matches,
                                        address) == DeathMessageResult::WrittenAsHash);
    assert(memory.words[kOther + 0x10] == wanted.hash);

    // Twice is not identifiable, so nothing is written.
    memory = filled(kBase, kWindow, 0x1111);
    for (unsigned offset = 0; offset < kWindow; offset += 4) memory.words[kOther + offset] = 0x2222;
    memory.words[kBase + 0x08] = carried.hash;
    memory.words[kOther + 0x20] = carried.hash;
    assert(ds2::transplantScriptMessage(regions, regionCount, wanted, carried, read, write, matches, address) == DeathMessageResult::Ambiguous);
    assert(memory.words[kBase + 0x08] == carried.hash && memory.words[kOther + 0x20] == carried.hash);

    // Neither shape present: the layout is not what we think, leave it alone.
    memory = filled(kBase, kWindow, 0x1111);
    assert(ds2::transplantScriptMessage(regions, regionCount, wanted, carried, read, write, matches, address) == DeathMessageResult::NotFound);

    // Nothing to do at all.
    assert(ds2::transplantScriptMessage(regions, regionCount, {}, carried, read, write, matches, address)
           == DeathMessageResult::SourceHasNone);
    assert(ds2::transplantScriptMessage(regions, regionCount, wanted, {}, read, write, matches, address)
           == DeathMessageResult::DonorHasNone);
    assert(ds2::transplantScriptMessage(regions, regionCount, wanted, wanted, read, write, matches, address)
           == DeathMessageResult::NotNeeded);

    // Refusals rather than half-done work.
    Memory empty;
    auto readEmpty = [&](uint32_t a, void* o, size_t s) { return empty.read(a, o, s); };
    assert(ds2::transplantScriptMessage(regions, regionCount, wanted, carried, readEmpty, write, matches, address) == DeathMessageResult::HeaderUnreadable);
    assert(ds2::transplantScriptMessage(nullptr, 0, wanted, carried, read, write, matches, address)
           == DeathMessageResult::HeaderUnreadable);
    memory = filled(kBase, kWindow, 0x1111);
    memory.words[kBase + 0x10] = carried.hash;
    auto refuse = [&](uint32_t, const void*, size_t) { return false; };
    assert(ds2::transplantScriptMessage(regions, regionCount, wanted, carried, read, refuse, matches, address) == DeathMessageResult::WriteFailed);
    // A pointer field with nowhere to point is refused too.
    memory = filled(kBase, kWindow, 0x1111);
    memory.strings[kDonorString] = carried.name;
    memory.words[kBase + 0x14] = kDonorString;
    auto noAddress = [](const char*) { return 0u; };
    assert(ds2::transplantScriptMessage(regions, regionCount, wanted, carried, read, write, matches, noAddress) == DeathMessageResult::WriteFailed);

    // What was learned from one enemy is reused for the next, which is the only
    // way to correct a donor that carries no message of its own.
    memory = filled(kBase, kWindow, 0x1111);
    for (unsigned offset = 0; offset < kWindow; offset += 4) memory.words[kOther + offset] = 0x2222;
    memory.words[kBase] = 0xAABBCCDD;          // the component's vtable
    memory.words[kOther] = 0x99887766;
    memory.words[kBase + 0x1C] = carried.hash;
    ds2::MessageSite site{};
    assert(ds2::transplantScriptMessage(regions, regionCount, wanted, carried, read, write, matches,
                                        address, &site) == DeathMessageResult::WrittenAsHash);
    assert(site.vtable == 0xAABBCCDD && site.offset == 0x1C && !site.asPointer);

    // A second enemy of the same class, whose donor has no message at all.
    memory = filled(kBase, kWindow, 0x1111);
    for (unsigned offset = 0; offset < kWindow; offset += 4) memory.words[kOther + offset] = 0x2222;
    memory.words[kBase] = 0xAABBCCDD;
    memory.words[kOther] = 0x99887766;
    assert(ds2::writeLearnedMessage(regions, regionCount, wanted, site, read, write, address) ==
           DeathMessageResult::WrittenAsHash);
    assert(memory.words[kBase + 0x1C] == wanted.hash);

    // No component of that class: nothing is written.
    memory.words[kBase] = 0x11223344;
    assert(ds2::writeLearnedMessage(regions, regionCount, wanted, site, read, write, address) ==
           DeathMessageResult::NotFound);
    // Two of that class cannot be told apart.
    memory.words[kBase] = 0xAABBCCDD;
    memory.words[kOther] = 0xAABBCCDD;
    assert(ds2::writeLearnedMessage(regions, regionCount, wanted, site, read, write, address) ==
           DeathMessageResult::Ambiguous);

    // The layout read from the engine's own attribute tables: a component points
    // at its data area, the authored header sits 0x10 below it and names the
    // class, and the message is at the table's offset from the data area.
    {
        constexpr uint32_t kModule = 0x00400000;
        constexpr uint32_t kHealth = 0x30000, kSpawning = 0x31000, kAwareness = 0x32000;
        constexpr uint32_t kHealthData = 0x40010, kSpawningData = 0x41010, kAwarenessData = 0x42010;
        Memory layout;
        auto component = [&](uint32_t address, uint32_t data, uint32_t descriptorRva) {
            layout.words[address + 0x0C] = data;
            layout.words[data - 0x10] = kModule + descriptorRva;
        };
        component(kHealth, kHealthData, 0x1CA4208);
        component(kSpawning, kSpawningData, 0x1CA4078);
        component(kAwareness, kAwarenessData, 0x1CA4D98);
        ds2::MemoryRegion parts[] = {{kHealth, 0}, {kSpawning, 0}, {kAwareness, 0}};
        auto readIt = [&](uint32_t a, void* o, size_t n) { return layout.read(a, o, n); };
        auto writeIt = [&](uint32_t a, const void* i, size_t n) { return layout.write(a, i, n); };

        // The engine's message table, as the release function walks it: a bucket
        // array indexed by the low 12 bits of the hash, entries chained through
        // their first word, the key at +0x10. Only a name that is in here may be
        // written - the release function decrements whatever the walk ends on
        // and has no branch for "not found".
        constexpr uint32_t kBuckets = 0x50000, kEntry = 0x51000;
        layout.words[kModule + ds2::kMessageTableRva] = kBuckets;
        layout.words[kBuckets + (wanted.hash & 0xFFF) * 4] = kEntry;
        layout.words[kEntry] = 0;
        layout.words[kEntry + 0x10] = wanted.hash;
        assert(ds2::messageRegistered(kModule, wanted.hash, readIt));
        assert(!ds2::messageRegistered(kModule, carried.hash, readIt));
        assert(ds2::messageEntry(kModule, wanted.hash, readIt) == kEntry);
        // The entry starts with the level's own holders on it, and the owner
        // fields carry the donor's reference from creation.
        layout.words[kEntry + 0x14] = 3;
        for (uint32_t owner : {kHealthData + 0x04, kHealthData + 0x10, kSpawningData + 0x48, kAwarenessData + 0x20})
            layout.words[owner + 0x04] = 1u << 16;     // count at owner+6 == 1

        // Each message lands on its own component, at its own offset.
        layout.words[kHealthData + 0x04] = carried.hash;
        layout.words[kHealthData + 0x10] = carried.hash;
        layout.words[kSpawningData + 0x48] = carried.hash;
        layout.words[kAwarenessData + 0x20] = carried.hash;
        uint32_t header = 0;
        assert(ds2::writeScriptMessage(parts, 3, ds2::kDeathFields, std::size(ds2::kDeathFields),
               kModule, wanted, carried, readIt, writeIt, &header) == DeathMessageResult::WrittenAsHash);
        assert(header == kHealthData - 0x10 && layout.words[kHealthData + 0x04] == wanted.hash);
        assert(ds2::writeScriptMessage(parts, 3, ds2::kHealthModifiedFields,
               std::size(ds2::kHealthModifiedFields), kModule, wanted, carried, readIt, writeIt) ==
               DeathMessageResult::WrittenAsHash);
        assert(layout.words[kHealthData + 0x10] == wanted.hash);
        assert(ds2::writeScriptMessage(parts, 3, ds2::kSpawnDoneFields,
               std::size(ds2::kSpawnDoneFields), kModule, wanted, carried, readIt, writeIt) ==
               DeathMessageResult::WrittenAsHash);
        assert(layout.words[kSpawningData + 0x48] == wanted.hash);
        assert(ds2::writeScriptMessage(parts, 3, ds2::kAwareFields, std::size(ds2::kAwareFields),
               kModule, wanted, carried, readIt, writeIt) == DeathMessageResult::WrittenAsHash);
        assert(layout.words[kAwarenessData + 0x20] == wanted.hash);
        // Four hashes written, four references taken on the entry, and every
        // owner holds exactly one - what the release function will undo.
        assert(layout.half(kEntry + 0x14) == 7);
        for (uint32_t owner : {kHealthData + 0x04, kHealthData + 0x10, kSpawningData + 0x48, kAwarenessData + 0x20})
            assert(layout.half(owner + 6) == 1);

        // A source name the table does not know is never written. On teardown the
        // release function would walk an empty bucket and then decrement whatever
        // it ended on, which is null - the 0x007564B9 access violation. Zero is
        // the one value it checks for, so that goes in and the message is lost
        // rather than the process.
        {
            const ds2::ScriptMessage stranger{"not_registered_in_this_level", 0x5A5A1234u};
            assert(!ds2::messageRegistered(kModule, stranger.hash, readIt));
            layout.words[kHealthData + 0x04] = carried.hash;
            assert(ds2::writeScriptMessage(parts, 3, ds2::kDeathFields, std::size(ds2::kDeathFields),
                   kModule, stranger, carried, readIt, writeIt) ==
                   DeathMessageResult::ClearedUnregistered);
            assert(layout.words[kHealthData + 0x04] == 0);
            // Nothing was written, so no reference was taken either.
            assert(layout.half(kEntry + 0x14) == 7);
            // A donor that arrived without a message of its own has an owner
            // count of zero; taking the reference sets it to one, so the release
            // will find a reference to undo rather than a hash with none.
            layout.words[kHealthData + 0x08] = 0;
            assert(ds2::writeScriptMessage(parts, 3, ds2::kDeathFields, std::size(ds2::kDeathFields),
                   kModule, wanted, {}, readIt, writeIt) == DeathMessageResult::WrittenAsHash);
            assert(layout.half(kHealthData + 0x04 + 6) == 1 && layout.half(kEntry + 0x14) == 8);
            // And with no table at all, nothing is ever written.
            layout.words[kModule + ds2::kMessageTableRva] = 0;
            assert(!ds2::messageRegistered(kModule, wanted.hash, readIt));
            layout.words[kModule + ds2::kMessageTableRva] = kBuckets;
        }
        layout.words[kHealthData + 0x04] = wanted.hash;

        // A donor with no message of its own is written anyway - that is half of
        // them, and the reason searching could never help.
        layout.words[kHealthData + 0x04] = 0xDEADBEEF;
        assert(ds2::writeScriptMessage(parts, 3, ds2::kDeathFields, std::size(ds2::kDeathFields),
               kModule, wanted, {}, readIt, writeIt) == DeathMessageResult::WrittenAsHash);
        assert(layout.words[kHealthData + 0x04] == wanted.hash);
        // Already right.
        assert(ds2::writeScriptMessage(parts, 3, ds2::kDeathFields, std::size(ds2::kDeathFields),
               kModule, wanted, {}, readIt, writeIt) == DeathMessageResult::NotNeeded);
        // The donor's value has to be standing there when it has one.
        layout.words[kHealthData + 0x04] = 0x11112222;
        assert(ds2::writeScriptMessage(parts, 3, ds2::kDeathFields, std::size(ds2::kDeathFields),
               kModule, wanted, carried, readIt, writeIt) == DeathMessageResult::NotFound);
        // No component of that class at all.
        ds2::MemoryRegion only[] = {{kAwareness, 0}};
        assert(ds2::writeScriptMessage(only, 1, ds2::kSpawnDoneFields,
               std::size(ds2::kSpawnDoneFields), kModule, wanted, {}, readIt, writeIt) ==
               DeathMessageResult::NotFound);
        // Two of that class cannot be told apart.
        component(kSpawning + 0x100, kSpawningData + 0x1000, 0x1CA4078);
        ds2::MemoryRegion twice[] = {{kSpawning, 0}, {kSpawning + 0x100, 0}};
        assert(ds2::writeScriptMessage(twice, 2, ds2::kSpawnDoneFields,
               std::size(ds2::kSpawnDoneFields), kModule, wanted, {}, readIt, writeIt) ==
               DeathMessageResult::Ambiguous);
        // A source with no message clears the donor's instead of leaving it.
        //
        // A name from another chapter is not in the engine's message table once
        // that chapter is gone, and the function that takes a reference has no
        // branch for "not found" - it writes to null. Zero is the value it does
        // check for, so zero is what has to be left behind.
        layout.words[kHealthData + 0x04] = carried.hash;
        assert(ds2::writeScriptMessage(parts, 3, ds2::kDeathFields, std::size(ds2::kDeathFields),
               kModule, {}, carried, readIt, writeIt) == DeathMessageResult::Cleared);
        assert(layout.words[kHealthData + 0x04] == 0);
        // Once cleared there is nothing left to do: the field already holds the
        // zero the source asks for, which the "already right" check answers
        // before the donor check gets a say.
        assert(ds2::writeScriptMessage(parts, 3, ds2::kDeathFields, std::size(ds2::kDeathFields),
               kModule, {}, carried, readIt, writeIt) == DeathMessageResult::NotNeeded);
        // Neither side has one: nothing to do, and nothing written.
        layout.words[kHealthData + 0x04] = 0x33334444;
        assert(ds2::writeScriptMessage(parts, 3, ds2::kDeathFields, std::size(ds2::kDeathFields),
               kModule, {}, {}, readIt, writeIt) == DeathMessageResult::SourceHasNone);
        assert(layout.words[kHealthData + 0x04] == 0x33334444);
        // Nowhere to write it.
        assert(ds2::writeScriptMessage(nullptr, 0, ds2::kDeathFields, std::size(ds2::kDeathFields),
               kModule, wanted, carried, readIt, writeIt) == DeathMessageResult::HeaderUnreadable);
    }

    // The embedded table: sorted, searchable, and the hashes match the names.
    const size_t count = sizeof(ds2::kDeathMessages) / sizeof(ds2::kDeathMessages[0]);
    assert(count > 500);
    for (size_t i = 1; i < count; ++i)
        assert(ds2::kDeathMessages[i - 1].guid < ds2::kDeathMessages[i].guid);
    auto engineHash = [](const char* text) {
        unsigned value = 0;
        for (const char* c = text; *c; ++c) {
            unsigned char lowered = static_cast<unsigned char>(*c);
            if (lowered >= 'A' && lowered <= 'Z') lowered = static_cast<unsigned char>(lowered + 0x20);
            value = value * 0x1003Fu + lowered;
        }
        return value;
    };
    unsigned withAware = 0, withSpawnDone = 0, withHealth = 0, checked = 0;
    for (size_t i = 0; i < count; ++i) {
        const auto& row = ds2::kDeathMessages[i];
        for (const auto* message : {&row.death, &row.healthModified, &row.spawnDone, &row.aware}) {
            if (!message->hash) { assert(!message->name[0]); continue; }
            assert(message->name[0] && engineHash(message->name) == message->hash);
            ++checked;
        }
        withAware += row.aware.hash != 0;
        withSpawnDone += row.spawnDone.hash != 0;
        withHealth += row.healthModified.hash != 0;
    }
    // Measured from the authored data; a drop means the catalog changed.
    assert(withAware == 35 && withSpawnDone == 39 && withHealth == 39);
    assert(!ds2::scriptMessagesFor(ds2::Guid{1, 2, 3, 4}).any());
    // The hospital lobby, where this was found: all four slashers agree.
    const auto lobby = ds2::deathMessageFor(ds2::Guid{0x36CB09BBu, 0xEDE1C02Du, 0x45544150u, 0x3230314Cu});
    assert(lobby == ds2::deathMessageFor(ds2::Guid{0x36CB09BBu, 0xF80AF6C6u, 0x45544150u, 0x3230314Cu}));
    assert(lobby == engineHash("hospital_lobby_slasher_dead"));

    std::printf("script message tests passed (%zu enemies, %u messages hashed)\n", count, checked);
    return 0;
}
