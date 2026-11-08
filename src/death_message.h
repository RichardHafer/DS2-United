// Dead Space 2 United by Richard Hafer.
#pragma once
#include "selection.h"
#include "generated_death_messages.h"
#include <cstring>

namespace ds2 {
// What a replacement has to say for the level to notice it.
//
// A scripted room counts messages, not corpses. `CMD_SetOnDeathMessage` on the
// health component is the one the level listens for: the hospital lobby waits
// for five times `hospital_lobby_slasher_dead` (read out of the running game on
// 2026-09-16, where a `VariableOperator` adds one per message and a
// `VariableWatcher` opens up at a threshold of five). Names like
// `var_op_slasher_count_decre` say what the listener does with it. Three more
// fields behave the same way: the end of a spawn, a health change, and noticing
// the player. A donor carries its own, from its own level, so the count never
// completes and the door stays shut.
//
// The field is authored as a self-relative string pointer, which means the
// number in the archive is a file offset and worthless at runtime. The name is
// what survives, so a message is carried as both the name and the engine's hash
// of it, and the runtime looks for whichever the header actually holds.
//
// Nothing is written on a guess. The field is found through the component's
// class descriptor, and the donor's own message, when it has one, has to be
// standing there. Anything else leaves the enemy alone.
struct ScriptMessages {
    ScriptMessage death{}, healthModified{}, spawnDone{}, aware{};
    bool any() const {
        return death.hash || healthModified.hash || spawnDone.hash || aware.hash;
    }
};

inline ScriptMessages scriptMessagesFor(const Guid& guid) {
    size_t low = 0, high = sizeof(kDeathMessages) / sizeof(kDeathMessages[0]);
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (kDeathMessages[middle].guid < guid) low = middle + 1;
        else high = middle;
    }
    if (low >= sizeof(kDeathMessages) / sizeof(kDeathMessages[0]) ||
        !(kDeathMessages[low].guid == guid))
        return {};
    const auto& row = kDeathMessages[low];
    return {row.death, row.healthModified, row.spawnDone, row.aware};
}

inline unsigned deathMessageFor(const Guid& guid) {
    return scriptMessagesFor(guid).death.hash;
}

enum class DeathMessageResult : unsigned {
    NotNeeded, SourceHasNone, DonorHasNone, HeaderUnreadable, NotFound, Ambiguous,
    WrittenAsHash, WrittenAsPointer, WriteFailed, Cleared, ClearedUnregistered, SharedHolders
};

inline const char* deathMessageResultName(DeathMessageResult result) {
    switch (result) {
        case DeathMessageResult::NotNeeded: return "not_needed";
        case DeathMessageResult::SourceHasNone: return "source_has_none";
        case DeathMessageResult::DonorHasNone: return "donor_has_none";
        case DeathMessageResult::HeaderUnreadable: return "header_unreadable";
        case DeathMessageResult::NotFound: return "not_found";
        case DeathMessageResult::Ambiguous: return "ambiguous";
        case DeathMessageResult::WrittenAsHash: return "written_as_hash";
        case DeathMessageResult::WrittenAsPointer: return "written_as_pointer";
        case DeathMessageResult::WriteFailed: return "write_failed";
        case DeathMessageResult::Cleared: return "cleared_foreign";
        case DeathMessageResult::ClearedUnregistered: return "cleared_unregistered";
        case DeathMessageResult::SharedHolders: return "shared_holders";
    }
    return "unknown";
}

// Every component of one actor, as (address, size) pairs. The four messages do
// not live together: the death message and the health-change message are on the
// health component, but the spawn-done message is on the spawning component and
// MSG_AWARE on the awareness component. Searching only the health component
// would silently miss two of the four, so the search runs over all of them and
// still insists on exactly one hit in total.
struct MemoryRegion { uint32_t address = 0, size = 0; };

// Two ways the header can hold a message, tried in that order.
//
// `matchesString(value, name)` answers whether the dword at hand is a pointer to
// that string; `stringAddressOf(name)` hands back an address holding the source
// name, so a pointer field can be repointed. Both are supplied by the caller so
// this stays testable without a game.
// Where a message was found, so the layout can be learned once and reused.
//
// Half the donors carry no message of their own, and then there is nothing to
// search for, `donor_has_none`. But a component of a given class always keeps
// the field in the same place, so one donor that does carry a message teaches
// the offset for every donor that does not.
struct MessageSite { uint32_t component = 0, offset = 0, vtable = 0; bool asPointer = false; };

template<class Read, class Write, class MatchesString, class StringAddressOf>
DeathMessageResult transplantScriptMessage(const MemoryRegion* regions, size_t regionCount,
                                           const ScriptMessage& wanted, const ScriptMessage& carried,
                                           Read read, Write write, MatchesString matchesString,
                                           StringAddressOf stringAddressOf,
                                           MessageSite* site = nullptr) {
    if (!wanted.hash) return DeathMessageResult::SourceHasNone;
    if (!carried.hash) return DeathMessageResult::DonorHasNone;
    if (wanted.hash == carried.hash) return DeathMessageResult::NotNeeded;
    if (!regions || !regionCount) return DeathMessageResult::HeaderUnreadable;

    unsigned hashHits = 0, hashAt = 0, pointerHits = 0, pointerAt = 0;
    uint32_t hashRegion = 0, pointerRegion = 0;
    bool readAnything = false;
    for (size_t index = 0; index < regionCount; ++index) {
        const auto& region = regions[index];
        if (!region.address || !region.size || region.address > UINT32_MAX - region.size) continue;
        for (unsigned offset = 0; offset + 4 <= region.size; offset += 4) {
            unsigned value = 0;
            // A component may be shorter than the window; an unreadable word ends
            // that component rather than the whole search.
            if (!read(region.address + offset, &value, 4)) break;
            readAnything = true;
            if (value == carried.hash) {
                ++hashHits; hashAt = region.address + offset; hashRegion = region.address;
            } else if (value && matchesString(value, carried.name)) {
                ++pointerHits; pointerAt = region.address + offset; pointerRegion = region.address;
            }
        }
    }
    if (!readAnything) return DeathMessageResult::HeaderUnreadable;
    if (hashHits == 1) {
        unsigned replacement = wanted.hash, back = 0;
        if (!write(hashAt, &replacement, 4) || !read(hashAt, &back, 4) || back != replacement)
            return DeathMessageResult::WriteFailed;
        if (site) {
            uint32_t vtable = 0;
            read(hashRegion, &vtable, 4);
            *site = {hashRegion, hashAt - hashRegion, vtable, false};
        }
        return DeathMessageResult::WrittenAsHash;
    }
    if (pointerHits == 1) {
        const unsigned address = stringAddressOf(wanted.name);
        if (!address) return DeathMessageResult::WriteFailed;
        unsigned back = 0;
        if (!write(pointerAt, &address, 4) || !read(pointerAt, &back, 4) || back != address)
            return DeathMessageResult::WriteFailed;
        if (site) {
            uint32_t vtable = 0;
            read(pointerRegion, &vtable, 4);
            *site = {pointerRegion, pointerAt - pointerRegion, vtable, true};
        }
        return DeathMessageResult::WrittenAsPointer;
    }
    if (hashHits > 1 || pointerHits > 1) return DeathMessageResult::Ambiguous;
    return DeathMessageResult::NotFound;
}

// Write at a place learned earlier from another enemy of the same component
// class. Used when the donor carries no message at all, so there is nothing to
// search for. The component is identified by its vtable, and exactly one of the
// actor's components may match.
template<class Read, class Write, class StringAddressOf>
DeathMessageResult writeLearnedMessage(const MemoryRegion* regions, size_t regionCount,
                                       const ScriptMessage& wanted, const MessageSite& site,
                                       Read read, Write write, StringAddressOf stringAddressOf) {
    if (!wanted.hash) return DeathMessageResult::SourceHasNone;
    if (!site.vtable || !regions || !regionCount) return DeathMessageResult::NotFound;
    uint32_t target = 0;
    unsigned matches = 0;
    for (size_t index = 0; index < regionCount; ++index) {
        uint32_t vtable = 0;
        if (!read(regions[index].address, &vtable, 4) || vtable != site.vtable) continue;
        if (++matches > 1) return DeathMessageResult::Ambiguous;
        target = regions[index].address + site.offset;
    }
    if (!matches) return DeathMessageResult::NotFound;
    unsigned replacement = site.asPointer ? stringAddressOf(wanted.name) : wanted.hash, back = 0;
    if (!replacement) return DeathMessageResult::WriteFailed;
    if (!write(target, &replacement, 4) || !read(target, &back, 4) || back != replacement)
        return DeathMessageResult::WriteFailed;
    return site.asPointer ? DeathMessageResult::WrittenAsPointer : DeathMessageResult::WrittenAsHash;
}

// Where each authored attribute lives, read from the engine's own tables.
//
// A class descriptor holds an attribute table at +0x1C: 16-byte rows of
// (default, attribute slot, index:offset, flags), and the offset is counted from
// the data area a component points at. `[component+0x0C]` holds that address, so
// an attribute is simply `[component+0x0C] + offset`.
//
// Confirmed against a measurement in the running game: the death message read
// index 1, offset 4, and the hash of `hospital_lobby_slasher_dead` sat at
// header+0x14, which is the same place because the header is 0x10 below the data
// area. `tools/dump_attribute_offsets.py` prints the whole table for any class.
//
// The header also identifies the component: its first dword is the class
// descriptor, which is how the right component is picked out of an actor without
// knowing a single vtable.
struct MessageField {
    uint32_t descriptorRva;  // which component carries it
    uint32_t offset;         // from [component+0x0C]
};
constexpr uint32_t kComponentAttributes = 0x0C;
constexpr uint32_t kAttributesBias = 0x10;

// Death: both health components keep it in the same place.
constexpr MessageField kDeathFields[] = {{0x1CA4208, 0x04}, {0x1CA3970, 0x04}};
constexpr MessageField kHealthModifiedFields[] = {{0x1CA4208, 0x10}, {0x1CA3970, 0x10}};
constexpr MessageField kSpawnDoneFields[] = {{0x1CA4078, 0x48}};
constexpr MessageField kAwareFields[] = {{0x1CA4D98, 0x20}};

// Where the engine keeps its message names, relative to the module.
inline constexpr uint32_t kMessageTableRva = 0x01C4CD8C;
inline constexpr uint32_t kMessageBuckets = 0x1000;
inline constexpr uint32_t kMessageKeyOffset = 0x10;

// Is this name in the engine's table right now?
//
// The release function at 0x00756470 walks the bucket chain and then decrements
// the entry it ended on. It leaves the loop the same way whether it found the
// name or ran off the end, so a name that is not there means `add word ptr
// [null+0x14], -1`. Two crash reports on this machine, one before and one after
// the pool trim, both landed on that exact instruction.
//
// Writing zero is safe because the caller checks for it first. Writing a hash
// is only safe if the name is registered, so ask before writing.
inline constexpr uint32_t kMessageCountOffset = 0x14;   // 16-bit, on the table entry
inline constexpr uint32_t kOwnerCountOffset = 0x06;     // 16-bit, on the field that is written

// The table entry for this name, or 0 when the engine does not know it.
template<class Read>
uint32_t messageEntry(uint32_t moduleBase, uint32_t hash, Read read) {
    if (!hash) return 0;
    uint32_t table = 0;
    if (!read(moduleBase + kMessageTableRva, &table, 4) || !table) return 0;
    uint32_t entry = 0;
    if (!read(table + (hash & (kMessageBuckets - 1)) * 4, &entry, 4)) return 0;
    // The chain is short; a bound keeps a corrupt list from spinning here.
    for (unsigned step = 0; entry && step < 64; ++step) {
        uint32_t key = 0;
        if (!read(entry + kMessageKeyOffset, &key, 4)) return 0;
        if (key == hash) return entry;
        if (!read(entry, &entry, 4)) return 0;
    }
    return 0;
}

template<class Read>
bool messageRegistered(uint32_t moduleBase, uint32_t hash, Read read) {
    return messageEntry(moduleBase, hash, read) != 0;
}

// What the engine's own reference function (0x00756380) does after it has
// found the entry: one on the entry's count, one on the owner's. The field
// written here is that owner (the hash sits at +0, the 16-bit count at +6), and the
// release function undoes exactly this pair. A hash that was written without
// this step is released against an entry that was never told about it; when
// the level's own holders let go and the entry is freed, that release walks an
// empty bucket and decrements null. That was `cafe_necro_dead` on a checkpoint
// load: 0xC0000005 writing address 0x14, esi=0, eax=-1.
//
// The owner's count is set to one rather than incremented: the donor's own
// reference, if it had one, is being replaced, not joined. The donor's entry
// keeps a count it will never get back. One leaked reference is the safe side
// of this exchange, an unbalanced release is not.
template<class Read, class Write>
bool takeMessageReference(uint32_t entry, uint32_t owner, Read read, Write write) {
    uint16_t count = 0;
    if (!read(entry + kMessageCountOffset, &count, 2) || count == UINT16_MAX) return false;
    ++count;
    if (!write(entry + kMessageCountOffset, &count, 2)) return false;
    const uint16_t one = 1;
    return write(owner + kOwnerCountOffset, &one, 2);
}

// Take a message away from an actor, wherever it stands. Every component
// whose authored header names one of the classes gets its field zeroed when
// it holds `expected` (or anything at all when `expected` is 0). Zero is the
// one value the engine's release function checks for, so a zeroed field costs
// at most the one reference the constructor took, never a walk into null.
// Used for the copies of a horde: the actor arrives already carrying the
// source's death message (writeScriptMessage reported not_needed on every
// swarmer of a horde, 15.09. 13:12), and a room that counts kills through
// that message counted six for one spawn. Returns how many fields were zeroed.
template<class Read, class Write>
unsigned clearScriptMessages(const MemoryRegion* components, size_t componentCount,
                             const MessageField* fields, size_t fieldCount,
                             uint32_t moduleBase, uint32_t expected, Read read, Write write) {
    unsigned cleared = 0;
    if (!components) return 0;
    for (size_t index = 0; index < componentCount; ++index) {
        uint32_t data = 0, descriptor = 0;
        if (!read(components[index].address + kComponentAttributes, &data, 4) ||
            data <= kAttributesBias)
            continue;
        if (!read(data - kAttributesBias, &descriptor, 4)) continue;
        for (size_t field = 0; field < fieldCount; ++field) {
            if (descriptor != moduleBase + fields[field].descriptorRva) continue;
            const uint32_t target = data + fields[field].offset;
            uint32_t before = 0;
            if (!read(target, &before, 4) || !before) continue;
            if (expected && before != expected) continue;
            const uint32_t zero = 0;
            if (write(target, &zero, 4)) ++cleared;
        }
    }
    return cleared;
}

// One message into one actor. `components` is every component the actor owns;
// the one whose authored header names the right class is the target, and there
// must be exactly one of it.
template<class Read, class Write>
DeathMessageResult writeScriptMessage(const MemoryRegion* components, size_t componentCount,
                                      const MessageField* fields, size_t fieldCount,
                                      uint32_t moduleBase, const ScriptMessage& wanted,
                                      const ScriptMessage& carried, Read read, Write write,
                                      uint32_t* headerOut = nullptr) {
    // Clearing matters as much as writing.
    //
    // A donor from another chapter carries its own message, and outside its own
    // level that name is not registered anywhere. The engine keeps its messages
    // in a 4096-bucket table at 0x0204CD8C, and the function that takes a
    // reference to one walks the bucket chain and then increments the entry it
    // ended on, with no branch for "not found", so an unregistered name means a
    // write to null+0x14. A crash dump caught it doing exactly that on the hash
    // of `mm_mining_camp_intro_damaged`, chapters away from the mining camp.
    //
    // Zero is the one value that function does check for, and it skips the
    // lookup entirely. So a source without a message does not mean "leave the
    // donor's alone", it means the donor's has to be cleared.
    if (!wanted.hash && !carried.hash) return DeathMessageResult::SourceHasNone;
    if (!components || !componentCount) return DeathMessageResult::HeaderUnreadable;
    uint32_t target = 0, header = 0;
    unsigned matches = 0;
    for (size_t index = 0; index < componentCount; ++index) {
        uint32_t data = 0, descriptor = 0;
        if (!read(components[index].address + kComponentAttributes, &data, 4) ||
            data <= kAttributesBias)
            continue;
        if (!read(data - kAttributesBias, &descriptor, 4)) continue;
        for (size_t field = 0; field < fieldCount; ++field) {
            if (descriptor != moduleBase + fields[field].descriptorRva) continue;
            if (++matches > 1) return DeathMessageResult::Ambiguous;
            header = data - kAttributesBias;
            target = data + fields[field].offset;
        }
    }
    if (!matches) return DeathMessageResult::NotFound;
    if (headerOut) *headerOut = header;
    uint32_t before = 0;
    if (!read(target, &before, 4)) return DeathMessageResult::HeaderUnreadable;
    // Already right, so there is nothing to confirm and nothing to do. This has
    // to be asked before the donor check below, not after.
    //
    // Two actors can share one authored attribute block when the randomizer
    // sends two spawn points to the same pool record. The first writes the
    // source's hash; the second then finds neither its donor's hash nor an
    // empty field and used to be counted as `not_found`. Nothing was wrong (the
    // field held exactly what the second one wanted, and it did send the right
    // message), but the log said `refused=1`, and a refusal in the log is what
    // a stuck door gets blamed on. Seen on 2026-09-16 with spawns 15 and 16
    // of the hospital lobby, both pointing at header 38C08F58, in a run whose
    // quarantine counter reached its full five.
    if (before == wanted.hash) return DeathMessageResult::NotNeeded;
    // When the donor carries a message of its own, that is what has to be
    // standing there. It confirms the offset on every single use.
    if (carried.hash && before != carried.hash) return DeathMessageResult::NotFound;
    // The field sits in the attribute block every spawn of this donor shares, and
    // the count at +6 says how many live actors hold it. A new name written under
    // other holders becomes theirs too: in the ch03 funeral chapel an infector
    // from slot 6 would report slot 15. Only the actor built just now may hold it;
    // otherwise the death proxy says the message for this one.
    uint16_t holders = 0;
    if (wanted.hash && (!read(target + kOwnerCountOffset, &holders, 2) || holders > 1))
        return DeathMessageResult::SharedHolders;
    // The source's own name should be registered while its level is up, but
    // "should" is what the release function does not check either. If it is not
    // there, zero is the only value that cannot crash on teardown. The message
    // is lost, the game is not.
    const uint32_t entry = messageEntry(moduleBase, wanted.hash, read);
    uint32_t replacement = entry ? wanted.hash : 0u, back = 0;
    if (!write(target, &replacement, 4) || !read(target, &back, 4) || back != replacement)
        return DeathMessageResult::WriteFailed;
    if (!replacement)
        return wanted.hash ? DeathMessageResult::ClearedUnregistered : DeathMessageResult::Cleared;
    // The hash alone is a claim the release function will collect on. Back it
    // with the reference the engine would have taken, or take the hash out again.
    if (!takeMessageReference(entry, target, read, write)) {
        replacement = 0;
        write(target, &replacement, 4);
        return DeathMessageResult::WriteFailed;
    }
    return DeathMessageResult::WrittenAsHash;
}
}
