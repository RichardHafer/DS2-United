// Every script message the engine sends, one line each, when MessageTrace=1.
//
// The level logic is a web of these messages: a quarantine counts deaths, a
// door waits for an unlock, an elevator waits for a ride to end. Every hang
// so far was a message that never came, and every time the question was
// which one. The send function at 0x00756C60 takes the message object as its
// first argument, hash at +0, a flag byte at +4 and the sender at +8, looks
// the hash up in the 4096 bucket registry and calls every listener on the
// entry. A hook on its first instruction sees all of that before it happens.
//
// Off by default. It is a diagnostic, and a busy room sends hundreds of
// messages a second, so the lines go through a ring the log thread drains,
// with a hard cap per second and a counter for what the cap dropped.
namespace message_trace {
safetyhook::MidHook sendHook;
bool enabled = false;
constexpr unsigned kRing = 2048;
constexpr LONG kPerSecond = 1500;
struct Entry {
    volatile LONG ready = 0;
    uint32_t hash = 0, flags = 0, source = 0, tick = 0;
    int registered = 0, listeners = 0;
};
Entry ring[kRing];
volatile LONG published = 0, dropped = 0, secondStart = 0, inSecond = 0;

int CountListeners(uint32_t entry) {
    // Mirrors the walk in the send function: entry+4 is the first list, each
    // list holds its head at +8, a node's next is at +4, the next list at +0.
    uint32_t list = 0;
    if (!Read(entry + 4, &list, 4)) return 0;
    int count = 0;
    for (unsigned lists = 0; list && lists < 8; ++lists) {
        uint32_t node = 0;
        if (!Read(list + 8, &node, 4)) break;
        for (unsigned nodes = 0; node && nodes < 64; ++nodes) {
            ++count;
            if (!Read(node + 4, &node, 4)) break;
        }
        if (!Read(list, &list, 4)) break;
    }
    return count;
}

// 0x00756C60 on whichever thread sends. Nothing here blocks or allocates.
void OnSend(SafetyHookContext& ctx) {
    uint32_t message = 0, hash = 0, flagsAndMore = 0, source = 0;
    if (!Read(ctx.esp + 4, &message, 4) || !message ||
        !Read(message, &hash, 4) || !Read(message + 4, &flagsAndMore, 4) || !Read(message + 8, &source, 4)) return;
    const LONG now = static_cast<LONG>(GetTickCount());
    if (now - InterlockedCompareExchange(&secondStart, 0, 0) >= 1000) {
        InterlockedExchange(&secondStart, now);
        InterlockedExchange(&inSecond, 0);
    }
    if (InterlockedIncrement(&inSecond) > kPerSecond) { InterlockedIncrement(&dropped); return; }
    const uint32_t entry = ds2::messageEntry(static_cast<uint32_t>(base), hash,
        [](uint32_t address, void* out, size_t size) { return Read(address, out, size); });
    const LONG sequence = InterlockedIncrement(&published);
    auto& slot = ring[static_cast<unsigned>(sequence - 1) % kRing];
    InterlockedExchange(&slot.ready, 0);
    slot.hash = hash;
    slot.flags = flagsAndMore & 0xFF;
    slot.source = source;
    slot.tick = static_cast<uint32_t>(now);
    slot.registered = entry != 0;
    slot.listeners = entry ? CountListeners(entry) : 0;
    InterlockedExchange(&slot.ready, sequence);
}

// The names the plugin already carries: the four messages of every enemy in
// the catalog. Everything else prints as its hash and is resolved offline with
// tools/resolve_messages.py against the game's own files.
const char* NameFor(uint32_t hash) {
    if (!hash) return "none";
    for (const auto& row : ds2::kDeathMessages) {
        for (const auto* message : {&row.death, &row.healthModified, &row.spawnDone, &row.aware})
            if (message->hash == hash && message->name[0]) return message->name;
    }
    return "?";
}

bool Install(FILE* log) {
    // push ecx / mov edx,[esp+8] / mov ecx,[edx] / push esi / mov esi,ecx / push edi / mov edi,[0x204CD8C]
    const unsigned char expected[] = {0x51,0x8B,0x54,0x24,0x08,0x8B,0x0A,0x56,0x8B,0xF1,0x57,0x8B,0x3D};
    unsigned char actual[sizeof(expected)]{};
    uint32_t table = 0;
    if (!Read(base + 0x356C60, actual, sizeof(actual)) || std::memcmp(actual, expected, sizeof(expected)) ||
        !Read(base + 0x356C6D, &table, 4) || table != base + ds2::kMessageTableRva) {
        std::fprintf(log, "message_trace send signature mismatch; trace off\n");
        return false;
    }
    sendHook = safetyhook::create_mid(reinterpret_cast<void*>(base + 0x356C60), OnSend);
    if (!sendHook) {
        std::fprintf(log, "message_trace hook failed; trace off\n");
        return false;
    }
    std::fprintf(log, "message_trace installed at %08X ring=%u cap_per_second=%ld\n",
        static_cast<unsigned>(base + 0x356C60), kRing, kPerSecond);
    return true;
}

// Log thread. Repeats of one hash inside a quarter second fold into one
// line, which keeps the per-frame chatter readable without hiding it.
LONG next = 1;
void Drain(FILE* log) {
    if (!sendHook) return;
    uint32_t lastHash = 0, lastTick = 0;
    unsigned repeats = 0;
    char line[160]{};
    auto flush = [&]() {
        if (!line[0]) return;
        if (repeats) std::fprintf(log, "%s repeats=%u\n", line, repeats);
        else std::fprintf(log, "%s\n", line);
        line[0] = 0;
    };
    for (;;) {
        auto& slot = ring[static_cast<unsigned>(next - 1) % kRing];
        const LONG ready = InterlockedCompareExchange(&slot.ready, 0, 0);
        if (ready < next) break;
        if (ready > next) {
            flush();
            std::fprintf(log, "message_overrun skipped=%ld\n", ready - next);
            next = ready;
        }
        Entry copy;
        copy.hash = slot.hash; copy.flags = slot.flags; copy.source = slot.source; copy.tick = slot.tick;
        copy.registered = slot.registered; copy.listeners = slot.listeners;
        if (InterlockedCompareExchange(&slot.ready, 0, 0) != ready) continue;
        if (copy.hash == lastHash && copy.tick - lastTick < 250) {
            ++repeats;
        } else {
            flush();
            repeats = 0;
            std::snprintf(line, sizeof(line), "message tick=%u hash=%08X name=%s flags=%u source=%08X registered=%d listeners=%d",
                copy.tick, copy.hash, NameFor(copy.hash), copy.flags, copy.source, copy.registered, copy.listeners);
        }
        lastHash = copy.hash;
        lastTick = copy.tick;
        ++next;
    }
    flush();
    const LONG lost = InterlockedExchange(&dropped, 0);
    if (lost) std::fprintf(log, "message_trace dropped=%ld over_cap=%ld\n", lost, kPerSecond);
}
}
