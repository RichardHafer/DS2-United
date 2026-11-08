// Dead Space 2 United by Richard Hafer.
// Room lights switched by script travel as a state (analysis/SIMGROUP_INTERCEPT_2026-10-17.md).
// A light is in the world only with both of its flags: +0xE9 stream visible (set by the
// LightingManager on iMsgStreamVisible) and +0xEA script visible, which only an AnimateLight
// sets, on the level's lt_*_on / lt_*_off messages. Those come from triggers that answer to the
// local Isaac, and the mirror leaves AnimateLight alone (MirrorLights=0: the animated ones are
// re-sent every frame), so a room the host lit stayed dark for the client.
//   AnimateLight::HandleEvents 0x00BADC50 (thiscall(msg), ret 4, vtable 0x01B413A4): the message
//   id at msg+0 against its attributes ([this+0xC]): on +0x19C/+0x1A8/+0x1B4/+0x1C0, off
//   +0x1CC..+0x1F0, fade on +0x1FC..+0x220, fade off +0x22C..+0x250 (stride 0xC). The light
//   list [this+0x58]: entries at +8, 0x18 each, cached light at entry+0x10, count at +0x188.
// Each machine sends the "on" and "fade on" messages its AnimateLights took (by GUID, at most
// every 500 ms per light); the receiver hands the same message to its own copy unless that copy
// already shows it. A light whose room is not loaded yet keeps the wanted message and gets it
// once the room is there.
// 17.10. test: nearly only "off" travelled (levels switch lights off behind the player, which
// darkened rooms the partner still stood in) and the necro player saw lights flash. So "off"
// never travels; an "on" is sent only after it held 1.5 s here, a partner's is skipped while
// this copy changed within 1.5 s, and a light that changed 4 times within 10 s here is animated:
// neither sent nor applied for the rest of the level.
namespace lights {
constexpr uint32_t kMagic = 0x5448474Cu;   // 'LGHT'
constexpr uint32_t kVersion = 1;
constexpr unsigned kPerPacket = 8, kTable = 256;
constexpr uint32_t kHandleEventsRva = 0x7ADC50, kVtableRva = 0x17413A4;
constexpr uint32_t kAttributes = 0x0C, kLightList = 0x58, kListEntries = 8, kListEntrySize = 0x18, kEntryLight = 0x10, kListCount = 0x188;
constexpr uint32_t kLightFlags = 0xE8, kInWorldBit = 0x80, kStreamVisible = 0xE9, kScriptVisible = 0xEA;
constexpr DWORD kPerLightMs = 500, kSendMs = 200, kRetryMs = 1000, kKeepMs = 120000;
constexpr DWORD kHoldMs = 1500, kFloodMs = 10000;
constexpr unsigned kFloodChanges = 4;
struct Entry { uint32_t guid[4]; uint32_t message; };
struct Packet { uint32_t magic, version, token, sequence, count; Entry entries[kPerPacket]; };
static_assert(sizeof(Packet) == 180, "Unexpected light wire layout");
bool Valid(const Packet& p) { return p.magic == kMagic && p.version == kVersion && p.token && p.count && p.count <= kPerPacket; }

enum Kind : uint8_t { kNoKind = 0, kOn = 1, kOff = 2, kFadeOn = 3, kFadeOff = 4 };
const char* const kKindNames[5] = {"-", "on", "off", "fade on", "fade off"};
bool IsOn(uint8_t kind) { return kind == kOn || kind == kFadeOn; }
// One light: the last state we took or applied, the "on" still to send, what a partner wants
// that could not be applied yet, and how often it changed here.
struct Light {
    uint32_t guid[4];
    uint32_t object, vtable;
    uint8_t state;           // 0 unknown, kOn or kOff: the last one taken here or applied
    bool pending;            // a local "on" to send once it has held kHoldMs
    bool animated;           // changed kFloodChanges times within kFloodMs here
    uint8_t changeIndex;
    uint32_t onMessage;      // the message of that "on"
    DWORD stateSince;        // the state taken here last changed or was first seen (the send waits kHoldMs)
    DWORD changedAt;         // the last real change taken here, not the first sight and not a partner's
    DWORD changeTimes[kFloodChanges];
    unsigned changes;
    DWORD sentAt;
    uint32_t wantMessage;    // from a partner, not applied yet
    unsigned wantSlot;
    DWORD wantSince, lastTry;
    DWORD usedAt;
};
Light table[kTable]{};
bool enabled = true, installed = false;
safetyhook::InlineHook hook;
unsigned applying = 0;
uint32_t sequence = 0;
DWORD lastSend = 0, lastRetry = 0;
volatile LONG taken[5]{}, replayedSkips = 0, sentPackets = 0, sentEntries = 0, heldByRate = 0, received = 0, applied = 0, already = 0, notLoaded = 0,
              appliedLate = 0, gaveUp = 0, wrongClass = 0, faults = 0, tableFull = 0, lines = 0;
volatile LONG offsRefused = 0, skippedRecent = 0, skippedAnimated = 0, animatedLights = 0, animatedLines = 0, unsentDropped = 0;
DWORD Age(DWORD now, DWORD stamp) { return TicksSince(now, stamp); }
bool ChangedRecently(const Light& l, DWORD now) { return l.changedAt && Age(now, l.changedAt) < kHoldMs; }
// A change of state taken here: stamps the hold and counts toward the flood limit.
void NoteChange(Light& l, DWORD now) {
    l.stateSince = Stamp(now);
    if (!l.state) return;   // the first state seen (a room streaming in) is no change
    l.changedAt = Stamp(now);
    l.changeTimes[l.changeIndex % kFloodChanges] = Stamp(now);
    l.changeIndex = static_cast<uint8_t>((l.changeIndex + 1) % kFloodChanges);
    ++l.changes;
    if (l.animated || l.changes < kFloodChanges || Age(now, l.changeTimes[l.changeIndex]) > kFloodMs) return;
    l.animated = true;
    l.pending = false;
    l.wantMessage = 0;
    InterlockedIncrement(&animatedLights);
    if (InterlockedIncrement(&animatedLines) <= 20)
        gate::Event("lights: the light %08X-%08X changed %u times within %lu s here: animated, neither sent nor applied for the rest of the level",
                    l.guid[0], l.guid[1], kFloodChanges, static_cast<unsigned long>(kFloodMs / 1000));
}

// One read of the 16 message slots (every AnimateLight takes the per-frame update message too).
uint8_t Classify(uint32_t attributes, uint32_t id) {
    uint32_t block[0xC0 / 4];
    if (!id || !Read(attributes + 0x19C, block, sizeof(block))) return kNoKind;
    static const uint8_t kinds[4] = {kOn, kOff, kFadeOn, kFadeOff};
    for (unsigned group = 0; group < 4; ++group)
        for (unsigned i = 0; i < 4; ++i)
            if (block[(group * 0x30 + i * 0xC) / 4] == id) return kinds[group];
    return kNoKind;
}
bool IsAnimateLight(uint32_t object, uint32_t* vtable = nullptr) {
    uint32_t vt = 0;
    if (!object || !Read(object, &vt, 4) || vt != base + kVtableRva) return false;
    if (vtable) *vtable = vt;
    return true;
}
// The lights of one AnimateLight: how many, how many script visible, stream visible, in the world.
struct Census { unsigned lights = 0, script = 0, stream = 0, world = 0; };
Census Count(uint32_t object) {
    Census c;
    uint32_t list = 0;
    int32_t count = 0;
    if (!Read(object + kLightList, &list, 4) || !list || !Read(list + kListCount, &count, 4) || count <= 0 || count > 64) return c;
    for (int32_t i = 0; i < count; ++i) {
        uint32_t light = 0;
        unsigned char flags[3]{};
        if (!Read(list + kListEntries + i * kListEntrySize + kEntryLight, &light, 4) || !light || !Read(light + kLightFlags, flags, 3)) continue;
        ++c.lights;
        if (flags[2]) ++c.script;
        if (flags[1]) ++c.stream;
        if (flags[0] & kInWorldBit) ++c.world;
    }
    return c;
}
Light* Find(const uint32_t* guid, bool add, DWORD now) {
    Light* oldest = nullptr;
    for (Light& l : table) {
        if (l.usedAt && std::memcmp(l.guid, guid, 16) == 0) return &l;
        if (!oldest || l.usedAt < oldest->usedAt) oldest = &l;
    }
    if (!add) return nullptr;
    if (oldest->usedAt) InterlockedIncrement(&tableFull);
    *oldest = Light{};
    std::memcpy(oldest->guid, guid, 16);
    oldest->usedAt = Stamp(now);
    return oldest;
}

void __fastcall OnHandleEvents(void* self, void*, const uint32_t* msg) {
    const uint32_t object = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    uint32_t id = 0, attributes = 0;
    uint8_t kind = kNoKind;
    if (enabled && msg && Read(reinterpret_cast<uintptr_t>(msg), &id, 4) && Read(object + kAttributes, &attributes, 4) && attributes)
        kind = Classify(attributes, id);
    hook.thiscall<void>(self, msg);
    if (!kind || applying) return;
    InterlockedIncrement(&taken[kind]);
    // A partner's relay (the mirror) is no news for him, but it is a change here all the same.
    const bool relay = gate::mirroring != 0;
    if (relay) InterlockedIncrement(&replayedSkips);
    uint32_t guid[4]{};
    if (!Read(object + 0x24, guid, 16) || !(guid[0] | guid[1] | guid[2] | guid[3])) return;
    const DWORD now = GetTickCount();
    Light* l = Find(guid, true, now);
    l->object = object;
    l->vtable = base + kVtableRva;
    l->usedAt = Stamp(now);
    const uint8_t next = IsOn(kind) ? kOn : kOff;
    if (next != l->state) {
        NoteChange(*l, now);
        l->pending = next == kOn && !relay && !l->animated;
    }
    if (next == kOn && l->pending) l->onMessage = id;
    l->state = next;
    l->wantMessage = 0;   // what happened here wins over what a partner asked for earlier
}

void Apply(Light& l, uint32_t object, bool late, DWORD now) {
    const uint32_t message = l.wantMessage;
    uint32_t attributes = 0;
    const uint8_t kind = Read(object + kAttributes, &attributes, 4) && attributes ? Classify(attributes, message) : kNoKind;
    l.wantMessage = 0;
    if (!kind) { InterlockedIncrement(&wrongClass); return; }
    if (!IsOn(kind)) { InterlockedIncrement(&offsRefused); return; }   // an older build's "off"
    if (l.animated) { InterlockedIncrement(&skippedAnimated); return; }
    if (ChangedRecently(l, now)) { InterlockedIncrement(&skippedRecent); return; }
    const Census before = Count(object);
    l.object = object;
    l.vtable = base + kVtableRva;
    l.state = kOn;
    l.pending = false;   // nothing to send back
    if (before.lights && before.script == before.lights) { InterlockedIncrement(&already); return; }
    uint32_t raw[8]{message};
    ++applying;
    ++gate::mirroring;
    bool ok = false;
    __try {
        hook.thiscall<void>(reinterpret_cast<void*>(object), raw);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
    --gate::mirroring;
    --applying;
    if (!ok) return;
    InterlockedIncrement(late ? &appliedLate : &applied);
    const Census after = Count(object);
    const LONG n = InterlockedIncrement(&lines);
    if (n == 1)
        gate::Event("test lights INFO: partner %u's %s for the light %08X-%08X applied%s (script visible %u -> %u of %u, in the world %u -> %u)", l.wantSlot,
                    kKindNames[kind], l.guid[0], l.guid[1], late ? " once its room was here" : "", before.script, after.script, after.lights, before.world, after.world);
    else if (n <= 40)
        gate::Event("lights: partner %u's %s for the light %08X-%08X applied%s (script visible %u -> %u of %u, in the world %u -> %u)", l.wantSlot,
                    kKindNames[kind], l.guid[0], l.guid[1], late ? " once its room was here" : "", before.script, after.script, after.lights, before.world, after.world);
}
void OnPacket(const Packet& p, unsigned slot) {
    if (!installed || !inLevel) return;
    const DWORD now = GetTickCount();
    for (uint32_t i = 0; i < p.count; ++i) {
        const Entry& e = p.entries[i];
        if (!(e.guid[0] | e.guid[1] | e.guid[2] | e.guid[3]) || !e.message) continue;
        InterlockedIncrement(&received);
        Light* l = Find(e.guid, true, now);
        l->usedAt = Stamp(now);
        if (l->animated) { InterlockedIncrement(&skippedAnimated); continue; }
        if (ChangedRecently(*l, now)) { InterlockedIncrement(&skippedRecent); continue; }   // this copy is in the middle of its own change
        l->wantMessage = e.message;
        l->wantSlot = slot;
        l->wantSince = Stamp(now);
        l->lastTry = Stamp(now);
        const uint32_t object = mirror::ObjectByGuid(e.guid);
        if (IsAnimateLight(object)) Apply(*l, object, false, now);
        else InterlockedIncrement(&notLoaded);
    }
}

// Game thread, every frame.
void Tick(DWORD now) {
    if (!installed) return;
    static bool wasInLevel = false;
    if (!inLevel) {
        if (wasInLevel) for (Light& l : table) l = Light{};
        wasInLevel = false;
        return;
    }
    wasInLevel = true;
    if (peerTable.Count() && now - lastSend >= kSendMs) {
        lastSend = now;
        Packet p{kMagic, kVersion, peerReadiness.token, 0, 0, {}};
        for (Light& l : table) {
            if (!l.usedAt || !l.pending) continue;
            if (l.animated || l.state != kOn || !l.onMessage) { l.pending = false; InterlockedIncrement(&unsentDropped); continue; }
            if (!l.stateSince || Age(now, l.stateSince) < kHoldMs) continue;   // not held long enough yet
            if (l.sentAt && Age(now, l.sentAt) < kPerLightMs) { InterlockedIncrement(&heldByRate); continue; }
            if (p.count >= kPerPacket) break;
            std::memcpy(p.entries[p.count].guid, l.guid, 16);
            p.entries[p.count].message = l.onMessage;
            ++p.count;
            l.pending = false;
            l.sentAt = Stamp(now);
        }
        if (p.count) {
            p.sequence = ++sequence;
            if (SendToPeers(&p, sizeof(p)) > 0) { InterlockedIncrement(&sentPackets); InterlockedExchangeAdd(&sentEntries, static_cast<LONG>(p.count)); }
        }
    }
    if (now - lastRetry < kRetryMs) return;
    lastRetry = now;
    for (Light& l : table) {
        if (!l.usedAt || !l.wantMessage) continue;
        if (Age(now, l.wantSince) > kKeepMs) { l.wantMessage = 0; InterlockedIncrement(&gaveUp); continue; }
        const uint32_t object = mirror::ObjectByGuid(l.guid);
        if (IsAnimateLight(object)) Apply(l, object, true, now);
    }
}

void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Coop", "LightSync", 1, ini.c_str()) != 0;
    if (!enabled) {
        std::fprintf(log, "lights sync off ([Coop] LightSync=0)\n");
        return;
    }
    // push ebx / mov ebx,[esp+8] / push esi / push edi / mov edi,ecx / mov esi,[edi+0Ch] / push ebx / call
    static const unsigned char kBytes[] = {0x53, 0x8B, 0x5C, 0x24, 0x08, 0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x77, 0x0C, 0x53, 0xE8};
    installed = hook_site::Install(log, "lights AnimateLight::HandleEvents", base + kHandleEventsRva, kBytes, sizeof(kBytes),
                                   reinterpret_cast<void*>(OnHandleEvents), hook);
    std::fprintf(log, "lights sync %s: script switched room lights travel by GUID, \"on\" only, after holding %lu ms; %u changes within %lu s mark a light animated\n",
                 installed ? "on" : "NOT installed", static_cast<unsigned long>(kHoldMs), kFloodChanges, static_cast<unsigned long>(kFloodMs / 1000));
}
void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    const LONG sum = taken[1] + taken[2] * 3 + taken[3] * 5 + taken[4] * 7 + sentEntries * 11 + received * 13 + applied * 17 + already * 19 + notLoaded * 23 +
                     appliedLate * 29 + gaveUp * 31 + wrongClass * 37 + faults * 41 + replayedSkips * 43 + offsRefused * 47 + skippedRecent * 53 +
                     skippedAnimated * 59 + animatedLights * 61;
    if (sum == last) return;
    last = sum;
    unsigned known = 0, waiting = 0, dark = 0, lit = 0, animated = 0, pending = 0;
    for (const Light& l : table) {
        if (!l.usedAt) continue;
        ++known;
        if (l.wantMessage) ++waiting;
        if (l.animated) ++animated;
        if (l.pending) ++pending;
        if (l.object && IsAnimateLight(l.object)) {
            const Census c = Count(l.object);
            if (c.lights && c.world == 0) ++dark;
            else if (c.world) ++lit;
        }
    }
    std::fprintf(log, "lights taken on=%ld off=%ld fade_on=%ld fade_off=%ld replayed=%ld | sent packets=%ld entries=%ld held_by_rate=%ld | received=%ld applied=%ld "
                      "late=%ld already=%ld not_loaded=%ld gave_up=%ld wrong_class=%ld faults=%ld | known=%u waiting=%u lit=%u dark=%u table_full=%ld\n",
                 taken[1], taken[2], taken[3], taken[4], replayedSkips, sentPackets, sentEntries, heldByRate, received, applied, appliedLate, already, notLoaded,
                 gaveUp, wrongClass, faults, known, waiting, lit, dark, tableFull);
    std::fprintf(log, "lights filters: offs_refused=%ld skipped_recent_change=%ld skipped_animated=%ld animated=%ld (now %u) unsent_dropped=%ld pending=%u\n",
                 offsRefused, skippedRecent, skippedAnimated, animatedLights, animated, unsentDropped, pending);
}
}  // namespace lights
