// Dead Space 2 United by Richard Hafer.
// What kind of scripted moment the local Isaac is in, read from the game's own state
// (17.10., user: "no band-aids, the game must know"; offline research: Xbox PDB, PC code,
// dumps). Each machine classifies its own Isaac, because a remote ghost has no PlayerMasterSM
// and no sequence, and sends the kind in its VISI packet. Every rule that cared about "a scene"
// (the necro's scene hold, watch and vent re-entry, the cutscene module's partner hiding) asks
// this instead of timers and names.
//   PlayerMasterSM [P+0x24C] (vtable 0x01B27598), state [+0xC]: 0 main, 1 scriptedSequence,
//     2 scriptedEventMode (walk and talk), 3 straitJacketMode.
//   PlayerLowerBodySM, a child (+0x20 first, +0x28 next) with vtable 0x01B27000; its state name
//     ([[sm+0x10]+4*state]+4): pairedAnim, swarmShakeOff, kong, eyePoke, stationaryShooting,
//     hackingStation, hanging, dragged.
//   Sequence: [P+0x240] bit 0 queued, bit 2 running; [P+0x238] the sequence (ScriptedEventSequence
//     0x01B5D308, ScriptedSequence 0x01B59A60), settings at [[seq+0xC]+0x74].
//   Button mash: the AlertVFXComponent's prompt count, byte +0x18 (component =
//     [[P+0x48] + 4*byte[[P+0x44] + byte[0x020A3C02] + 0xC]]).
//   Behaviour flags [P+0x298] (64 bit): 14 shaking off, 45 stationary shooting; the stationary
//     marker SafePtr [P+0x1764].
//   Player flags [P+0x3DC]/[P+0x3E0]: 29 SE mode, 32 playing paired anim, 38 strait jacket, 39
//     dementia nav help.
//   Camera: the viewport's blender ([[0x0202131C]+0x10] + [P+0x420]*0x2A0), top [b+0x94+8*[b+0xD4]]
//     minus 0x10; the chase (0x01B64654) and manual aim (0x01B62F10) cameras are gameplay.
namespace scenekind {
enum : uint32_t { kNone = 0, kCinematic = 1, kShort = 2, kQte = 3, kStationary = 4, kPlayable = 5, kKinds = 6, kUnknown = 0xFFu };
const char* const kNames[kKinds] = {"none", "cinematic", "short sequence", "QTE", "stationary aiming", "playable sequence"};
constexpr uint32_t kMasterVtableRva = 0x1727598, kLowerVtableRva = 0x1727000, kSesVtableRva = 0x175D308, kSsVtableRva = 0x1759A60;
constexpr uint32_t kCameraManagerRva = 0x1C2131C, kAlertTypeIndexRva = 0x1CA3C02;
constexpr uint32_t kChaseCamRva = 0x1764654, kManualAimCamRva = 0x1762F10;
struct CamName { uint32_t rva; const char* name; };
const CamName kCams[] = {{0x1764654, "chase"}, {0x1762F10, "manual aim"}, {0x1762CAC, "bone bound"}, {0x176324C, "spline"}, {0x16A9648, "paired anim"},
                         {0x1762DB8, "fixed"}, {0x1763E10, "orbit"}, {0x16A95E0, "pole"}, {0x17632F0, "two entity"}};
struct Reading {
    uint32_t seq = 0, seqVtable = 0, settings = 0, master = 0xFFFFFFFFu, camVtable = 0, bhv0 = 0, bhv1 = 0, f0 = 0, f1 = 0, marker = 0;
    uint8_t tflags = 0, mash = 0;
    char lower[24] = "";
};
uint32_t local = kNone;
// The Isaac `local` was read from, and whether a script had him: a scripted sequence running with the
// master in its scriptedSequence state, or a QTE inside one (runtime_coop_scenedamage.inc).
uint32_t localPlayer = 0;
// The local Isaac's camera is a gameplay one (chase, manual aim, or none known).
bool localGameplayCamera = true;
const char* cameraName = "none";
bool localScripted = false;
DWORD localSince = 0;
bool sendNow = false;   // streams::vis sends its packet at once
uint8_t partner[coop_peers::kMaxPeers]{};
DWORD partnerAt[coop_peers::kMaxPeers]{};
volatile LONG changes = 0, transitions = 0, readFaults = 0, partnerChanges = 0, counts[kKinds]{};

bool StateName(uint32_t sm, char* out, size_t size) {
    uint32_t state = 0, table = 0, entry = 0, name = 0;
    out[0] = 0;
    if (!Read(sm + 0xC, &state, 4) || state > 256 || !Read(sm + 0x10, &table, 4) || !table || !Read(table + 4 * state, &entry, 4) || !entry ||
        !Read(entry + 4, &name, 4) || !name || !Read(name, out, size - 1))
        return false;
    out[size - 1] = 0;
    return true;
}
bool ReadPlayer(uint32_t p, Reading& r) {
    uint32_t root = 0, vtable = 0;
    if (!Read(p + 0x24C, &root, 4) || !root || !Read(root, &vtable, 4) || vtable != base + kMasterVtableRva) return false;
    Read(root + 0xC, &r.master, 4);
    for (uint32_t child = 0, hops = 0; hops < 16; ++hops) {
        if (!Read(hops ? child + 0x28 : root + 0x20, &child, 4) || !child) break;
        uint32_t cv = 0;
        if (Read(child, &cv, 4) && cv == base + kLowerVtableRva) { StateName(child, r.lower, sizeof(r.lower)); break; }
    }
    Read(p + 0x240, &r.tflags, 1);
    Read(p + 0x238, &r.seq, 4);
    if (r.seq) {
        uint32_t attrs = 0;
        Read(r.seq, &r.seqVtable, 4);
        if (Read(r.seq + 0xC, &attrs, 4) && attrs) Read(attrs + 0x74, &r.settings, 4);
    }
    uint32_t comps = 0, map = 0, alert = 0;
    uint8_t typeIndex = 0, slot = 0;
    if (Read(p + 0x48, &comps, 4) && comps && Read(p + 0x44, &map, 4) && map && Read(base + kAlertTypeIndexRva, &typeIndex, 1) &&
        Read(map + typeIndex + 0xC, &slot, 1) && slot != 0xFF && Read(comps + 4 * slot, &alert, 4) && alert)
        Read(alert + 0x18, &r.mash, 1);
    Read(p + 0x298, &r.bhv0, 4);
    Read(p + 0x29C, &r.bhv1, 4);
    Read(p + 0x3DC, &r.f0, 4);
    Read(p + 0x3E0, &r.f1, 4);
    Read(p + 0x1764, &r.marker, 4);
    uint32_t manager = 0, blenders = 0, viewport = 0, top = 0, entry = 0;
    if (Read(base + kCameraManagerRva, &manager, 4) && manager && Read(manager + 0x10, &blenders, 4) && blenders && Read(p + 0x420, &viewport, 4) &&
        viewport < 4) {
        const uint32_t b = blenders + viewport * 0x2A0;
        if (Read(b + 0xD4, &top, 4) && top < 8 && Read(b + 0x94 + 8 * top, &entry, 4) && entry > 0x10) Read(entry - 0x10, &r.camVtable, 4);
    }
    return true;
}
const char* CameraName(uint32_t vtable) {
    for (const CamName& c : kCams) if (vtable == base + c.rva) return c.name;
    return vtable ? "other" : "none";
}
bool Lower(const Reading& r, const char* name) { return std::strcmp(r.lower, name) == 0; }
uint32_t Classify(const Reading& r, bool& transition) {
    transition = false;
    const bool running = (r.tflags & 4) && r.seq;
    // A QTE belongs to a scene only inside a scripted sequence or event (17.10. 22:30: a slasher's
    // grab in combat, paired anim with a mash prompt and no sequence, counted as a QTE; a necro at
    // the vents turned into Isaac to "watch" it and stayed 91 s).
    const bool scripted = running || r.master == 1 || r.master == 2;
    if (scripted && (r.mash || Lower(r, "pairedAnim") || Lower(r, "swarmShakeOff") || Lower(r, "kong") || Lower(r, "eyePoke") || (r.bhv0 & 0x4000) || (r.f1 & 1)))
        return kQte;
    if ((r.bhv1 & 0x2000) || r.marker || Lower(r, "stationaryShooting")) return kStationary;
    if (running && r.master == 1) {
        const bool gameplayCamera = !r.camVtable || r.camVtable == base + kChaseCamRva || r.camVtable == base + kManualAimCamRva;
        return gameplayCamera ? kShort : kCinematic;
    }
    if (running && r.master == 0) {
        if (r.tflags & 1) { transition = true; return kNone; }   // queued: the master switches next frame
        return kPlayable;
    }
    if (r.master == 2) return kShort;
    if (r.master == 3 || (r.f1 & 0x80)) return kPlayable;   // strait jacket, dementia nav help
    if (Lower(r, "hackingStation")) return kShort;
    return kNone;
}
const char* SeqClass(uint32_t vtable) { return vtable == base + kSesVtableRva ? "event sequence" : vtable == base + kSsVtableRva ? "sequence" : vtable ? "other" : "-"; }
// Game thread, every frame: plain reads only.
void Tick(DWORD now) {
    const uint32_t p = inLevel ? LocalCampaignPlayer() : 0;
    Reading r;
    bool transition = false;
    uint32_t k = kNone;
    if (p) {
        __try {
            if (!ReadPlayer(p, r)) k = kNone;
            else k = Classify(r, transition);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedIncrement(&readFaults);
            localPlayer = 0;
            localScripted = false;
            return;
        }
    }
    localGameplayCamera = !p || !r.camVtable || r.camVtable == base + kChaseCamRva || r.camVtable == base + kManualAimCamRva;
    cameraName = CameraName(r.camVtable);
    if (transition) { InterlockedIncrement(&transitions); return; }
    localPlayer = k == kNone ? 0 : p;
    localScripted = k != kNone && (k == kQte || ((r.tflags & 4) && r.seq && r.master == 1));
    static uint32_t previousCamera = 0;
    static unsigned cameraLines = 0;
    if (r.camVtable != previousCamera) {
        if (cameraLines < 200) {
            ++cameraLines;
            gate::Event("scene camera t=%lu player=%08X %s -> %s seq=%08X settings=%04X master=%u mash=%u",
                        static_cast<unsigned long>(now), p, CameraName(previousCamera), CameraName(r.camVtable),
                        r.seq, r.settings & 0xFFFF, r.master, r.mash);
        }
        previousCamera = r.camVtable;
    }
    if (k == local) return;
    gate::Event("test scene_kind INFO: %s (was %s for %lu ms; %s %08X settings %04X, master %d, lower %s, camera %s, mash %u, behaviour %08X:%08X, se %d, "
                "strait jacket %d, dementia %d, stationary marker %d)",
                kNames[k], kNames[local < kKinds ? local : 0], static_cast<unsigned long>(localSince ? now - localSince : 0), SeqClass(r.seqVtable), r.seq,
                r.settings & 0xFFFF, static_cast<int>(r.master), r.lower[0] ? r.lower : "-", CameraName(r.camVtable), r.mash, r.bhv1, r.bhv0,
                (r.f0 >> 29) & 1, (r.f1 >> 6) & 1, (r.f1 >> 7) & 1, r.marker ? 1 : 0);
    local = k;
    localSince = now ? now : 1;
    sendNow = true;
    InterlockedIncrement(&changes);
    InterlockedIncrement(&counts[k]);
}
// From the VISI packets.
void NotePartner(unsigned slot, uint32_t kind, DWORD now) {
    if (slot >= coop_peers::kMaxPeers || kind >= kKinds) return;
    if (partner[slot] != kind || !partnerAt[slot]) {
        InterlockedIncrement(&partnerChanges);
        if (InterlockedCompareExchange(&partnerChanges, 0, 0) <= 400) gate::Event("scene_kind partner %u: %s", slot, kNames[kind]);
    }
    partner[slot] = static_cast<uint8_t>(kind);
    partnerAt[slot] = now ? now : 1;
}
// A packet newer than the caller's now (a stamp from a later GetTickCount) is fresh, not 49 days old.
uint32_t PartnerKind(unsigned slot, DWORD now) {
    return slot < coop_peers::kMaxPeers && partnerAt[slot] && static_cast<LONG>(now - partnerAt[slot]) <= 3000 ? partner[slot] : kUnknown;
}
// A human in one of these holds the necros back; a playable sequence (Nicole's fight) does not.
bool HoldsNecros(uint32_t k) { return k == kCinematic || k == kShort || k == kQte || k == kStationary; }
// What a necro turns into Isaac for: a cinematic, and its QTE and aiming parts.
bool Watchable(uint32_t k) { return k == kCinematic || k == kQte || k == kStationary; }
// A short sequence (the kinesis panel) or a playable one (the dementia fight) hides nobody.
bool LocalHidesPartners() { return local == kCinematic || local == kQte || local == kStationary; }
void WriteDiagnostics(FILE* log) {
    std::fprintf(log, "scene_kind local=%s changes=%ld transitions=%ld faults=%ld seen none=%ld cinematic=%ld short=%ld qte=%ld stationary=%ld playable=%ld | partner changes=%ld",
                 kNames[local < kKinds ? local : 0], InterlockedCompareExchange(&changes, 0, 0), InterlockedCompareExchange(&transitions, 0, 0),
                 InterlockedCompareExchange(&readFaults, 0, 0), counts[0], counts[1], counts[2], counts[3], counts[4], counts[5],
                 InterlockedCompareExchange(&partnerChanges, 0, 0));
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s)
        if (partnerAt[s]) std::fprintf(log, " p%u=%s", s, kNames[partner[s] < kKinds ? partner[s] : 0]);
    std::fprintf(log, "\n");
}
}  // namespace scenekind
