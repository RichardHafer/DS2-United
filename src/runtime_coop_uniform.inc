// Dead Space 2 United by Richard Hafer.
// Retail uniform lifecycle. Addresses and layouts are verified from PC memory.
namespace coop_uniform {
constexpr uint32_t kManager = 0x1C1C0DC, kManagerVtable = 0x175CA78;
constexpr uint32_t kChange = 0xAE0610, kAttach = 0xAB4E10, kLoad = 0xAC6310;
// Skin bone scale pass (cdecl, skin): writes a 4x4 matrix per bone to [skin+0x120] + index * 64.
// Nine call sites, among them the deferred skin queue entry 0x010767C0.
constexpr uint32_t kSkinScale = 0xC741F0;
struct UniformEntry {
    unsigned char loaded, dlc, activations, padding;
    Guid guid;
    uint32_t stream;
};
struct PlayerEntry {
    unsigned char waiting;
    signed char index;
    unsigned short padding;
    uint32_t safePlayer, safeNext;
};
struct Array { uint32_t data, count, capacity; };
struct Body { uint32_t player = 0, view = 0, bones = 0, mmi = 0, uniform = 0, buffer = 0, count = 0; };
struct Donor { Body body; UniformEntry entry{}; };
static_assert(sizeof(UniformEntry) == 24 && sizeof(PlayerEntry) == 12);
safetyhook::InlineHook changeHook, attachHook, loadHook, skinScaleHook;
bool installed = false;
// Signatures verified and configuration read; the hooks themselves wait for
// the first connected partner (Arm). Solo, ChangeToUniform keeps its retail
// bytes: the debug menu (DS2_Enemy_Spawner) checks exactly those before it
// changes a suit and refused with binding_mismatch whenever these hooks were
// in place (its log, 16.09.-20.09.2026). Once armed they stay for the
// session; unhooking under a running call is not worth a suit change.
bool armable = false;
DWORD armedAt = 0;
volatile LONG armAttempts = 0;
volatile LONG stopped = 0, changes = 0, attachments = 0, loads = 0, repairs = 0;
volatile LONG rejected = 0, overflows = 0;
volatile LONG polls = 0, noDonor = 0, notReady = 0, notLinked = 0;
volatile LONG poseBound = 0, poseRebound = 0, poseMissing = 0, poseInvalid = 0;
// Ghosts between a detach and their next attach: the detach frees the pose buffer
// but +0x1E0 keeps the pointer, and an animation update then wrote into freed memory
// (19.10. client crash 42200 and host crash 7908: the corrupted chunk was that buffer).
// An entry was cleared only by the next attach, so ghosts deleted while detached (rejoins, respawns)
// stayed; 24.10. the 8 slots were full at a Game Over restart (host crash 22384, overflow 0->1) and the
// ninth ghost went unguarded. Entries that are no longer a ghost Player are dropped before an overflow.
uint32_t detachedGhosts[16]{};
volatile LONG detaches = 0, detachedSkips = 0, detachedOverflow = 0, detachedReclaimed = 0;
bool Player(uint32_t p);
void RememberSkin(uint32_t skin, uint32_t player);
bool Detached(uint32_t player) {
    for (uint32_t d : detachedGhosts) if (d && d == player) return true;
    return false;
}
void MarkDetached(uint32_t player, bool detached) {
    for (uint32_t& d : detachedGhosts) if (d == player) d = 0;
    if (!detached) return;
    for (uint32_t& d : detachedGhosts) if (!d) { d = player; return; }
    for (uint32_t& d : detachedGhosts)
        if (!Player(d) || !coop::IsGhostPlayer(d)) { d = 0; InterlockedIncrement(&detachedReclaimed); }
    for (uint32_t& d : detachedGhosts) if (!d) { d = player; return; }
    InterlockedIncrement(&detachedOverflow);
}
std::mutex logMutex;
char messages[64][768]{};
unsigned messageCount = 0;
thread_local unsigned nesting = 0;

bool Player(uint32_t p) {
    uint32_t vt = 0;
    return p && Read(p, &vt, 4) && vt == base + placement::kPlayerPrimaryVtableRva;
}
Body ReadBody(uint32_t p) {
    Body b;
    if (!Player(p)) return b;
    b.player = p;
    Read(p + 0x194, &b.view, 4);
    Read(p + 0x140, &b.mmi, 4);
    Read(p + 0x1688, &b.uniform, 4);
    Read(p + 0x1E0, &b.buffer, 4);
    if (b.view) {
        Read(b.view + 0xB8, &b.bones, 4);
        Read(b.view + 0xB0, &b.count, 4);
    }
    return b;
}
bool Complete(const Body& b) { return b.player && b.view && b.bones && b.mmi && b.uniform && b.count == 132; }
bool Manager(uint32_t m, Array& uniforms, Array& players) {
    uint32_t singleton = 0, vt = 0;
    return Read(base + kManager, &singleton, 4) && singleton == m && m &&
        Read(m, &vt, 4) && vt == base + kManagerVtable &&
        Read(m + 0x10, &uniforms, sizeof(uniforms)) &&
        Read(m + 0x1C, &players, sizeof(players)) &&
        uniforms.data && players.data && uniforms.count <= uniforms.capacity &&
        uniforms.capacity <= 128 && players.count <= players.capacity && players.capacity <= 16;
}
bool FindDonor(uint32_t m, uint32_t target, Donor& out) {
    Array uniforms{}, players{};
    if (!Manager(m, uniforms, players)) return false;
    for (uint32_t i = 0; i < players.count; ++i) {
        PlayerEntry entry{};
        if (!Read(players.data + i * 12, &entry, 12) || entry.waiting || entry.index < 0 ||
            static_cast<unsigned>(entry.index) >= uniforms.count || entry.safePlayer < 0x10010) continue;
        const uint32_t p = entry.safePlayer - 0x10;
        // A Player deleted by the necromorph hand-over keeps its vtable bytes for
        // a while; only a body still registered with the PlayerManager may donate.
        if (p == target || !Player(p) || coop::IsGhostPlayer(p) || !coop::InPlayerManager(p) || coop::IsBotEntity(p)) continue;
        const Body body = ReadBody(p);
        UniformEntry suit{};
        if (!Complete(body) || !Read(uniforms.data + entry.index * 24, &suit, 24) ||
            suit.loaded != 1 || !suit.activations || !suit.stream) continue;
        out.body = body;
        out.entry = suit;
        return true;
    }
    return false;
}
volatile LONG peerSuitApplied = 0, peerSuitMissing = 0, peerSuitSelected = 0, noDonorBirths = 0;
bool streamPeerSuit = false;  // [Coop] SuitStream
void Record(const char* event, uint32_t m, uint32_t p, const Guid* guid, const Donor* donor = nullptr);

// A peer suit that is already loaded here, with a stream. PeerSuit decides
// what happens to one that is not (SuitStream).
bool ResidentSuit(uint32_t m, const Guid& want, UniformEntry& out) {
    Array uniforms{}, players{};
    if (!Manager(m, uniforms, players)) return false;
    for (uint32_t i = 0; i < uniforms.count; ++i) {
        UniformEntry suit{};
        if (!Read(uniforms.data + i * 24, &suit, 24)) continue;
        if (suit.loaded != 1 || !suit.stream) continue;
        if (std::memcmp(suit.guid.data(), want.data(), 16) != 0) continue;
        out = suit;
        return true;
    }
    return false;
}
// The suit of the ACTUAL local campaign player.
//
// Publishing must not use FindDonor: that returns "the first complete non-ghost
// player", and when IsGhostPlayer misses the ghost it returns the ghost itself.
// The suit the peer gave the local ghost is then announced as the local one,
// both sides converge on one suit and a real change never travels. Observed 2026-09-16:
// on the host the donor GUID alternated between the local suit (38CB666F...)
// and the ghost's (39CAC0A5...), 124 against 139 samples, and the last value
// published was the ghost's. LocalCampaignPlayer comes from the native
// PlayerManager slot, so it cannot be the ghost.
bool LocalSuit(uint32_t m, Guid& out) {
    const uint32_t local = coop::LocalCampaignPlayer();
    if (!local) return false;
    Array uniforms{}, players{};
    if (!Manager(m, uniforms, players)) return false;
    for (uint32_t i = 0; i < players.count; ++i) {
        PlayerEntry entry{};
        if (!Read(players.data + i * 12, &entry, 12) || entry.waiting || entry.index < 0 ||
            static_cast<unsigned>(entry.index) >= uniforms.count || entry.safePlayer < 0x10010) continue;
        if (entry.safePlayer - 0x10 != local) continue;
        UniformEntry suit{};
        if (!Read(uniforms.data + entry.index * 24, &suit, 24) ||
            suit.loaded != 1 || !suit.stream) return false;
        out = suit.guid;
        return true;
    }
    return false;
}

// The suit a peer announced, if it is usable here.
// `wanted` is the suit of one particular partner, taken from that partner's own
// receiver. It used to read the single global one, which with four players
// would have given every ghost whichever suit arrived last.
bool PeerSuit(uint32_t m, const uint32_t wanted[4], Guid& out) {
    if (!coop::suitSync || !wanted) return false;
    Guid want{};
    for (unsigned i = 0; i < 4; ++i) want[i] = wanted[i];
    if (!want[0] && !want[1] && !want[2] && !want[3]) return false;
    UniformEntry resident{};
    if (ResidentSuit(m, want, resident)) { out = want; return true; }
    // unregistered here: ChangeToUniform would strip the ghost's suit and leave it bare
    // (the MP suit before this machine registered it, analysis/MP_SUITS_APPLY_2026-10-15.md)
    if (SuitIndex(want.data()) < 0) { InterlockedIncrement(&coop::mpsuit::ghostRefusals); return false; }
    // the MP suit takes its skin at the model build: wait for the partner's skin stream
    if (coop::mpsuit::IsMpSuit(want.data()) && !coop::mpsuit::PartnersReady()) return false;
    // Not resident. With streaming enabled, hand it to the native
    // ChangeToUniform anyway: that is the routine which registers the uniform,
    // counts the activation and loads the stream when it is missing, exactly as
    // it does for a local store change. Refusing here was what stopped the suit
    // from ever arriving: the peer's suit is commonly one this side unloaded
    // when its own activation count dropped to zero.
    if (!streamPeerSuit) return false;
    out = want;
    return true;
}
// Peer suits are brought in by the native ChangeToUniform itself (see
// PeerSuit): it registers the uniform, counts the activation and loads the
// stream in the right order. The earlier direct FindUniform/RegisterUniform/
// LoadUniformStream calls were removed after the 2026-09-16 crashes.
uint32_t FindGhost(uint32_t m) {
    Array uniforms{}, players{};
    if (!Manager(m, uniforms, players)) return 0;
    for (uint32_t i = 0; i < players.count; ++i) {
        PlayerEntry entry{};
        if (!Read(players.data + i * 12, &entry, 12) || entry.safePlayer < 0x10010) continue;
        const uint32_t p = entry.safePlayer - 0x10;
        if (Player(p) && coop::IsGhostPlayer(p)) return p;
    }
    return 0;
}

void Record(const char* event, uint32_t m, uint32_t p, const Guid* guid, const Donor* donor) {
    const Body b = ReadBody(p);
    const Body d = donor ? ReadBody(donor->body.player) : Body{};
    Array uniforms{}, players{};
    const bool valid = Manager(m, uniforms, players);
    PlayerEntry slot{};
    UniformEntry suit{};
    int slotIndex = -1;
    if (valid && p) {
        for (uint32_t i = 0; i < players.count; ++i) {
            PlayerEntry candidate{};
            if (!Read(players.data + i * 12, &candidate, 12) || candidate.safePlayer != p + 0x10) continue;
            slot = candidate;
            slotIndex = static_cast<int>(i);
            if (slot.index >= 0 && static_cast<unsigned>(slot.index) < uniforms.count)
                Read(uniforms.data + slot.index * 24, &suit, sizeof(suit));
            break;
        }
    }
    Guid g{};
    if (guid) Read(reinterpret_cast<uintptr_t>(guid), g.data(), 16);
    std::lock_guard<std::mutex> guard(logMutex);
    if (messageCount == std::size(messages)) { InterlockedIncrement(&overflows); return; }
    std::snprintf(messages[messageCount++], sizeof(messages[0]),
        "coop_uniform event=%s thread=%lu manager=%08X valid=%d entries=%u players=%u player=%08X ghost=%d guid=%08X-%08X-%08X-%08X stream=%08X slot=%d index=%d waiting=%u loaded=%u activations=%u view=%08X bones=%08X count=%u buffer=%08X mmi=%08X uniform=%08X donor=%08X donor_bones=%08X donor_mmi=%08X donor_uniform=%08X",
        event, GetCurrentThreadId(), m, valid, uniforms.count, players.count, p, coop::IsGhostPlayer(p),
        g[0], g[1], g[2], g[3], donor ? donor->entry.stream : suit.stream,
        slotIndex, slotIndex < 0 ? -1 : slot.index, slot.waiting, suit.loaded, suit.activations,
        b.view, b.bones, b.count, b.buffer, b.mmi, b.uniform,
        d.player, d.bones, d.mmi, d.uniform);
}
void CheckResult(uint32_t m, uint32_t p, const Donor& donor) {
    const Body after = ReadBody(p), local = ReadBody(donor.body.player);
    if (!Complete(local) || local.mmi != donor.body.mmi || local.bones != donor.body.bones ||
        (after.mmi && after.mmi == local.mmi) || (after.bones && after.bones == local.bones) ||
        (after.view && after.view == local.view) || (after.buffer && after.buffer == local.buffer) ||
        (after.uniform && after.uniform == local.uniform)) {
        InterlockedExchange(&stopped, 1);
        Record("ownership_failure_stopped", m, p, &donor.entry.guid, &donor);
    } else {
        Record(Complete(after) ? "independent_body_ready" : "body_pending", m, p, &donor.entry.guid, &donor);
    }
}
void __fastcall OnAttach(void* self, void*, uint32_t player, const Guid* guid) {
    InterlockedIncrement(&attachments);
    const auto manager = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    Donor donor{};
    const bool check = guid && coop::IsGhostPlayer(player) && FindDonor(manager, player, donor);
    Record(guid ? "attach_begin" : "detach_begin", manager, player, guid);
    if (!guid && coop::IsGhostPlayer(player)) { MarkDetached(player, true); InterlockedIncrement(&detaches); }
    attachHook.thiscall<void>(self, player, guid);
    if (guid) MarkDetached(player, false);
    Record(guid ? "attach_end" : "detach_end", manager, player, guid);
    if (check) CheckResult(manager, player, donor);
}
bool __fastcall OnLoad(void* self, void*, UniformEntry* entry) {
    InterlockedIncrement(&loads);
    const auto manager = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    UniformEntry copy{};
    Read(reinterpret_cast<uintptr_t>(entry), &copy, sizeof(copy));
    Donor context{}; context.entry = copy;
    Record("stream_request", manager, 0, &copy.guid, &context);
    const bool requested = loadHook.thiscall<bool>(self, entry);
    Record(requested ? "stream_requested" : "stream_already_present", manager, 0, &copy.guid, &context);
    return requested;
}
// A body we spawn is born in the suit we want (17.10.; Mark: MP players spawn with their suit
// resident and never change it). Player::Init (0x008A8270) dresses every new non-ghost Player
// after Restore with ChangeToUniform(mgr, this, this+0x5A0); that call returns to 0x008A8670.
// The spawner arms the GUID right before Finalize and disarms it after. The swap writes the
// GUID to Player+0x5A0 as well, so the body never wears the record's suit (the patient suit
// after a necro, the watch suit 3 s later) and needs no second change.
constexpr uint32_t kInitDressReturnRva = 0x4A8670, kPlayerSuitField = 0x5A0;
struct Birth { bool armed; uint32_t guid[4]; const char* why; };
Birth birth{};
// A body born in a suit that still streams. 02.11. client: the arena body born in [Arena] Suit
// was deleted by a checkpoint restart while that stream was in flight; the partner's ghost then
// wore the same suit with bone indices 0x8080 (pose_invalid_index, A-pose) and the local Isaac
// changed into it later (big head). Watched so the log names that order when it happens again.
struct Streaming { uint32_t player = 0; uint32_t guid[4]{}; DWORD at = 0; };
Streaming streaming;
uint32_t interruptedSuit[4]{};
volatile LONG birthStreamsInterrupted = 0, birthStreamsDone = 0;
volatile LONG birthsResident = 0, birthsStreamed = 0, birthsUnregistered = 0, birthsMissed = 0, birthLines = 0;
void BirthLine(const char* result, const char* text, uint32_t player) {
    if (InterlockedIncrement(&birthLines) > 40) return;
    coop::gate::Event("test birth_suit %s: %s %08X: suit %08X-%08X %s", result, birth.why ? birth.why : "a body", player, birth.guid[0], birth.guid[1], text);
}
bool ArmBirth(const uint32_t* guid, const char* why) {
    if (!installed || !guid || !(guid[0] | guid[1] | guid[2] | guid[3])) return false;
    std::memcpy(birth.guid, guid, 16);
    birth.why = why;
    birth.armed = true;
    coop::lifecycle::birthPending = true;
    return true;
}
// A mode's level start arms before the level request (coop::lifecycle::birthLoadOnly): only a body
// made while the level loads or while a checkpoint restart lands takes it (the restart's Isaac is
// made before the level manager leaves RUNNING), never a bot. A story join suit keeps priority in
// story co-op.
volatile LONG birthsRefusedRunning = 0;
bool BirthTakes(uint32_t player) {
    if (coop::play::StoryCoop() && coop::loadsync::JoinSuitPending()) return false;
    if (coop::lifecycle::birthLoadOnly && coop::IsBotEntity(player)) return false;
    if (coop::lifecycle::birthLoadOnly && coop::mpmap::LmState(nullptr) == coop_mpmap::kRunning && !coop::lifecycle::RestartBodyWindow()) {
        if (InterlockedIncrement(&birthsRefusedRunning) <= 8)
            coop::gate::Event("test birth_suit INFO: %s: Player::Init dressed %08X while the level runs and no restart lands; the birth suit stays armed", birth.why ? birth.why : "?",
                              player);
        return false;
    }
    return true;
}
void DisarmBirth(bool countMiss) {
    coop::lifecycle::birthLoadOnly = false;
    coop::lifecycle::birthPending = false;
    if (!birth.armed) return;
    birth.armed = false;
    if (!countMiss) return;
    InterlockedIncrement(&birthsMissed);
    BirthLine("FAIL", "never reached Player::Init's dress through the hook; the record's suit stays", 0);
}

uint32_t joinSuitWorn[4]{}, joinSuitPlayer = 0;
int JoinSuitState(char* why, size_t whySize);   // below, for the loading barrier
void __fastcall OnChange(void* self, void*, uint32_t player, const Guid* guid) {
    InterlockedIncrement(&changes);
    const auto manager = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    if (birth.armed && guid && static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress())) == base + kInitDressReturnRva && Player(player) &&
        !coop::IsGhostPlayer(player) && BirthTakes(player)) {
        birth.armed = false;
        coop::lifecycle::birthPending = coop::lifecycle::birthLoadOnly = false;
        coop::lifecycle::bornPlayer = player;
        if (SuitIndex(birth.guid) < 0) {
            InterlockedIncrement(&birthsUnregistered);
            BirthLine("FAIL", "is not registered here; the record's suit stays", player);
        } else {
            Guid want{};
            for (unsigned i = 0; i < 4; ++i) want[i] = birth.guid[i];
            UniformEntry resident{};
            const bool here = ResidentSuit(manager, want, resident);
            InterlockedIncrement(here ? &birthsResident : &birthsStreamed);
            BirthLine(here ? "PASS" : "INFO", here ? "is resident: born dressed" : "streams in first (born in it, bare until it is here)", player);
            if (!here) { streaming.player = player; std::memcpy(streaming.guid, birth.guid, 16); streaming.at = coop::Stamp(GetTickCount()); }
            WriteBytes(player + kPlayerSuitField, birth.guid, 16);
            Record("birth_suit", manager, player, &want);
            ++nesting;
            changeHook.thiscall<void>(self, player, reinterpret_cast<const Guid*>(static_cast<uintptr_t>(player + kPlayerSuitField)));
            --nesting;
            return;
        }
    }
    // A story joiner's first body during the load: born in the suit armed for the join
    // (coop::loadsync::ArmJoinSuit), not in the level template's patient suit.
    uint32_t joinGuid[4]{};
    const char* joinWhy = "";
    if (!birth.armed && guid && static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress())) == base + kInitDressReturnRva && Player(player) &&
        !coop::IsGhostPlayer(player) && !coop::IsBotEntity(player) && coop::loadsync::TakeJoinSuit(player, joinGuid, &joinWhy)) {
        if (SuitIndex(joinGuid) < 0) {
            InterlockedIncrement(&birthsUnregistered);
            coop::gate::Event("coop_barrier join suit %08X-%08X is not registered here: Isaac %08X keeps %08X-%08X (%s)", joinGuid[0], joinGuid[1], player,
                              (*guid)[0], (*guid)[1], joinWhy);
        } else {
            Guid want{};
            for (unsigned i = 0; i < 4; ++i) want[i] = joinGuid[i];
            UniformEntry resident{};
            const bool here = ResidentSuit(manager, want, resident);
            InterlockedIncrement(here ? &birthsResident : &birthsStreamed);
            coop::gate::Event("coop_barrier Isaac %08X born in the join suit %08X-%08X instead of %08X-%08X (%s): %s", player, joinGuid[0], joinGuid[1],
                              (*guid)[0], (*guid)[1], joinWhy, here ? "resident" : "streams in under the loading screen");
            WriteBytes(player + kPlayerSuitField, joinGuid, 16);
            std::memcpy(joinSuitWorn, joinGuid, 16);
            joinSuitPlayer = player;
            Record("join_suit", manager, player, &want);
            ++nesting;
            changeHook.thiscall<void>(self, player, reinterpret_cast<const Guid*>(static_cast<uintptr_t>(player + kPlayerSuitField)));
            --nesting;
            return;
        }
    }
    Donor donor{};
    const bool ghostChange = installed && !stopped && !nesting && guid && Player(player) && coop::IsGhostPlayer(player);
    const bool redirect = ghostChange && FindDonor(manager, player, donor);
    if (guid && !redirect) InterlockedIncrement(&rejected);
    // Prefer the suit the peer actually wears. Falling back to the donor suit
    // keeps the previous behaviour when the peer is unknown or its suit is not
    // resident here.
    Guid peer{};
    // Whose ghost is this? The sub-key's top byte says so, and that picks the
    // right partner's suit out of the table. When the body has no sub-key yet
    // (it is being built and has not been registered), fall back to the single
    // receiver, which is what two players always used.
    const uint32_t owner = coop::MachineOf(player);
    const uint32_t* wanted = coop::suitReceiver.have ? coop::suitReceiver.guid : nullptr;
    if (owner != coop::kNoMachine) {
        for (unsigned slot = 0; slot < coop_peers::kMaxPeers; ++slot) {
            const auto& entry = coop::peerTable.peers[slot];
            if (!entry.used || entry.machine != owner) continue;
            const auto& incoming = coop::peerRuntime[slot].suit;
            wanted = incoming.have ? incoming.guid : nullptr;
            break;
        }
        // A bot's ghost (machine 0xF0 + bot) wears what the host's bot wears.
        if (owner >= 0xF0u && owner < 0xF0u + coop::kMaxBots && coop::botnet::botSuits[owner - 0xF0u].have)
            wanted = coop::botnet::botSuits[owner - 0xF0u].guid;
    }
    // Without a local Isaac (the necro side of a 1v1 Versus round) there is no donor,
    // but the ghost is still born in the partner's suit rather than Engineering.
    const bool usePeer = ghostChange && wanted && PeerSuit(manager, wanted, peer);
    if (redirect && !usePeer && coop::suitSync && wanted)
        InterlockedIncrement(&peerSuitMissing);
    if (usePeer) InterlockedIncrement(&peerSuitSelected);
    if (usePeer && !redirect) InterlockedIncrement(&noDonorBirths);
    const Guid* chosen = usePeer ? &peer : redirect ? &donor.entry.guid : guid;
    Record(redirect || usePeer ? "ghost_uniform_selected" : "native_change", manager, player, guid, redirect ? &donor : nullptr);
    if (redirect || usePeer) Record(usePeer ? "selected_peer_guid" : "selected_guid", manager, player, chosen, redirect ? &donor : nullptr);
    ++nesting;
    changeHook.thiscall<void>(self, player, chosen);
    --nesting;
    if (redirect) CheckResult(manager, player, donor);
}

// Only the current intrusive player list can authorize a delayed repair.
bool Linked(uint32_t donor, uint32_t target) {
    uint32_t p = donor;
    for (unsigned i = 0; i < 16 && Player(p); ++i) {
        if (p == target) return true;
        uint32_t prevLink = 0, next = 0;
        if (!Read(p + 0x50, &prevLink, 4) || prevLink < 0x10054) break;
        const uint32_t previous = prevLink - 0x54;
        if (!Player(previous) || !Read(previous + 0x54, &next, 4) || next != p) break;
        p = previous;
    }
    for (unsigned i = 0; i < 16 && Player(p); ++i) {
        if (p == target) return true;
        uint32_t next = 0, prevLink = 0;
        if (!Read(p + 0x54, &next, 4) || !Player(next) ||
            !Read(next + 0x50, &prevLink, 4) || prevLink != p + 0x54) break;
        p = next;
    }
    return false;
}
// No caller in the plugin since SecondIsaac was removed; kept with its tests.
void Repair(uint32_t manager, uint32_t p, const Donor& donor) {
    const Body before = ReadBody(p);
    if (!before.player || p == donor.body.player || before.mmi || before.uniform) return;
    uint32_t animation = 0;
    if (!before.view || before.count != 132 || !Read(p + 0x188, &animation, 4) || !animation) {
        InterlockedIncrement(&notReady);
        return;
    }
    if (!Linked(donor.body.player, p)) { InterlockedIncrement(&notLinked); return; }
    InterlockedIncrement(&repairs);
    Record("repair_begin", manager, p, &donor.entry.guid, &donor);
    changeHook.thiscall<void>(reinterpret_cast<void*>(manager), p, &donor.entry.guid);
    CheckResult(manager, p, donor);
    if (!Complete(ReadBody(p))) {
        InterlockedExchange(&stopped, 1);
        Record("resident_repair_incomplete_stopped", manager, p, &donor.entry.guid, &donor);
    }
}
// A ghost body that is complete and has worn another suit than wanted for 1 s.
// Two changes on top of each other left a body hidden for good (12.10.), so a
// partner's ghost is changed at most once every kRedressGapMs; a body rebuilt at
// the same address is checked like any other (13.10.: one kept Engineering 72 s).
constexpr DWORD kWrongSuitMs = 1000, kRedressGapMs = 5000;
bool WrongSuitSettled(uint32_t ghost, const uint32_t* wanted, DWORD& since) {
    uint32_t worn[4]{};
    if (!Complete(ReadBody(ghost)) || !coop::botnet::WornSuit(ghost, worn)) { since = 0; return false; }
    if (std::memcmp(worn, wanted, 16) == 0) { since = 0; return false; }
    const DWORD now = GetTickCount();
    // not `now | 1`: an even tick seen again next frame read as 49 days and redressed at once
    if (!since) { since = coop::Stamp(now); return false; }
    return static_cast<LONG>(now - since) >= static_cast<LONG>(kWrongSuitMs);
}
volatile LONG noDonorDress = 0, ghostRedress = 0;
void Poll() {
    if (!installed || stopped || nesting) return;
    InterlockedIncrement(&polls);
    uint32_t manager = 0;
    Donor donor{};
    if (!Read(base + kManager, &manager, 4) || !manager) return;
    if (streaming.player) {
        Guid want{};
        for (unsigned i = 0; i < 4; ++i) want[i] = streaming.guid[i];
        UniformEntry resident{};
        if (ResidentSuit(manager, want, resident)) {
            InterlockedIncrement(&birthStreamsDone);
            if (InterlockedIncrement(&birthLines) <= 40)
                coop::gate::Event("test birth_suit INFO: suit %08X-%08X of %08X is in after %lu ms", streaming.guid[0], streaming.guid[1], streaming.player,
                                  static_cast<unsigned long>(GetTickCount() - streaming.at));
            streaming = Streaming{};
        } else if (!Player(streaming.player) || GetTickCount() - streaming.at > 60000) {
            const bool gone = !Player(streaming.player);
            InterlockedIncrement(&birthStreamsInterrupted);
            if (gone) std::memcpy(interruptedSuit, streaming.guid, 16);
            coop::gate::Event("test birth_suit WARN: %08X %s while its suit %08X-%08X was still streaming (%lu ms after the birth); a body that wears that suit "
                              "later may show broken bones (pose_invalid_index)", streaming.player, gone ? "was deleted (a restart or a respawn)" : "waited 60 s",
                              streaming.guid[0], streaming.guid[1], static_cast<unsigned long>(GetTickCount() - streaming.at));
            streaming = Streaming{};
        }
    }
    // No local Isaac to borrow from: the partners' and the bots' suits are still
    // put on their ghosts below.
    const bool haveDonor = FindDonor(manager, 0, donor);
    if (!haveDonor) InterlockedIncrement(&noDonor);
    static DWORD lastState = 0;
    if (haveDonor && GetTickCount() - lastState >= 1000) {
        lastState = GetTickCount();
        Record("resident_state", manager, donor.body.player, &donor.entry.guid, &donor);
    }
    // Publish the local suit, read from the local campaign player rather than
    // the donor, so the ghost's suit can never be announced as the local one.
    if (coop::suitSync) {
        Guid mine{};
        if (LocalSuit(manager, mine)) coop::suitSender.Publish(mine.data(), GetTickCount());
    }

    // Deliberately no manual stream request here any more. Calling
    // LoadUniformStream directly on an entry nobody had activated is the best
    // explanation for the two crashes on 2026-09-16 (21:48 exe+0xB712B1 reading
    // 0x224, 22:00 inside d3d9.dll reading 1, a half-built model reaching the
    // renderer): in that build the suit was never applied, so the raw stream
    // request was the only mutation. The native ChangeToUniform performs
    // register, activation count and stream load in the right order, and
    // PeerSuit now hands it the GUID even when the suit is not resident.

    // Re-dress the ghost once per peer suit change. Without this the ghost keeps
    // whatever suit it was given when it was created.
    // One pending generation per partner, not one for everybody.
    //
    // FindGhost returns whichever ghost comes first, which was the same thing as
    // "the partner's ghost" while there was exactly one. With four players it
    // would dress the wrong Isaac, so the body is looked up by the machine that
    // owns it: every sub-key is (machine << 24) | counter, so a ghost's owner is
    // simply the top byte of the key at entity+0x40.
    static uint32_t appliedGeneration[coop_peers::kMaxPeers]{}, appliedGhost[coop_peers::kMaxPeers]{};
    static DWORD wrongSince[coop_peers::kMaxPeers]{}, dressedAt[coop_peers::kMaxPeers]{};
    for (unsigned slot = 0; slot < coop_peers::kMaxPeers; ++slot) {
        const auto& entry = coop::peerTable.peers[slot];
        const auto& incoming = coop::peerRuntime[slot].suit;
        if (!entry.used || entry.machine == 0xFFFFFFFFu) continue;
        if (!coop::suitSync || !incoming.have) continue;
        Guid peer{};
        const uint32_t ghost = coop::GhostForMachine(entry.machine);
        if (incoming.generation == appliedGeneration[slot]) {
            // Same suit, maybe a new body (a respawn, a resent ghost): checked on every poll.
            if (!ghost) continue;
            uint32_t worn[4]{};
            if (Complete(ReadBody(ghost)) && coop::botnet::WornSuit(ghost, worn) && std::memcmp(worn, incoming.guid, 16) == 0) {
                appliedGhost[slot] = ghost;
                wrongSince[slot] = 0;
                continue;
            }
            if (dressedAt[slot] && static_cast<LONG>(GetTickCount() - dressedAt[slot]) < static_cast<LONG>(kRedressGapMs)) continue;
            if (!WrongSuitSettled(ghost, incoming.guid, wrongSince[slot]) || !PeerSuit(manager, incoming.guid, peer)) continue;
            appliedGhost[slot] = ghost;
            wrongSince[slot] = 0;
            dressedAt[slot] = coop::Stamp(GetTickCount());
            InterlockedIncrement(&ghostRedress);
            if (!haveDonor) InterlockedIncrement(&noDonorDress);
            Record("peer_suit_redress", manager, ghost, &peer, &donor);
            ++nesting;
            changeHook.thiscall<void>(reinterpret_cast<void*>(manager), ghost, &peer);
            --nesting;
            continue;
        }
        uint32_t wearing[4]{};
        if (!ghost) {
            // no ghost yet; keep the generation pending until one exists
        } else if (Complete(ReadBody(ghost)) && coop::botnet::WornSuit(ghost, wearing) && std::memcmp(wearing, incoming.guid, 16) == 0) {
            // Already in that suit: no model swap under a live AnimView (12.10.).
            appliedGeneration[slot] = incoming.generation;
            appliedGhost[slot] = ghost;
            wrongSince[slot] = 0;
        } else if (PeerSuit(manager, incoming.guid, peer)) {
            appliedGeneration[slot] = incoming.generation;
            appliedGhost[slot] = ghost;
            wrongSince[slot] = 0;
            dressedAt[slot] = coop::Stamp(GetTickCount());
            if (!haveDonor) InterlockedIncrement(&noDonorDress);
            InterlockedIncrement(&peerSuitApplied);
            Record("peer_suit_apply", manager, ghost, &peer, &donor);
            ++nesting;
            changeHook.thiscall<void>(reinterpret_cast<void*>(manager), ghost, &peer);
            --nesting;
        } else if (streamPeerSuit) {
            // Streaming is enabled: keep the generation pending and try again
            // on a later poll.
        } else {
            // Not resident and no streaming: consume the generation so this does
            // not retry every poll. A later peer change is picked up normally.
            appliedGeneration[slot] = incoming.generation;
            InterlockedIncrement(&peerSuitMissing);
            Record("peer_suit_not_resident", manager, ghost, nullptr, &donor);
        }
    }
    // The host's bots: their ghosts wear what the bots wear (botnet 'BSUT').
    static uint32_t botGeneration[coop::kMaxBots]{}, botGhost[coop::kMaxBots]{};
    static DWORD botWrongSince[coop::kMaxBots]{}, botDressedAt[coop::kMaxBots]{};
    for (unsigned i = 0; i < coop::kMaxBots; ++i) {
        const auto& wanted = coop::botnet::botSuits[i];
        if (!coop::suitSync || !wanted.have) continue;
        const uint32_t ghost = coop::GhostForMachine(0xF0u + i);
        if (!ghost) continue;
        if (botDressedAt[i] && static_cast<LONG>(GetTickCount() - botDressedAt[i]) < static_cast<LONG>(kRedressGapMs)) continue;
        uint32_t worn[4]{};
        if (Complete(ReadBody(ghost)) && coop::botnet::WornSuit(ghost, worn) && std::memcmp(worn, wanted.guid, 16) == 0) {
            botGhost[i] = ghost;
            botGeneration[i] = wanted.generation;
            botWrongSince[i] = 0;
            continue;
        }
        Guid peer{};
        if (!WrongSuitSettled(ghost, wanted.guid, botWrongSince[i]) || !PeerSuit(manager, wanted.guid, peer)) continue;
        botGhost[i] = ghost;
        botGeneration[i] = wanted.generation;
        botWrongSince[i] = 0;
        botDressedAt[i] = coop::Stamp(GetTickCount());
        InterlockedIncrement(&ghostRedress);
        if (!haveDonor) InterlockedIncrement(&noDonorDress);
        Record("bot_suit_apply", manager, ghost, &peer, &donor);
        ++nesting;
        changeHook.thiscall<void>(reinterpret_cast<void*>(manager), ghost, &peer);
        --nesting;
    }
}
void PoseReport(const char* reason, uint32_t skin, uint32_t player, uint32_t value, uint32_t limit) {
    char event[128]{};
    std::snprintf(event, sizeof(event), "%s skin=%08X value=%u limit=%u", reason, skin, value, limit);
    uint32_t manager = 0;
    Read(base + kManager, &manager, 4);
    Record(event, manager, player, nullptr);
}
bool PreparePose(uint32_t component) {
    uint32_t owner = 0, skin = 0, pose = 0, indices = 0, count = 0, bufferCount = 0;
    if (!Read(component + 0x2C, &owner, 4) || owner < 0x10010) return true;
    const uint32_t player = owner - 0x10;
    if (Detached(player)) { InterlockedIncrement(&detachedSkips); return false; }
    if (!Player(player) || !coop::IsGhostPlayer(player)) return true;
    const Body body = ReadBody(player);
    if (!Read(component + 0x1C, &skin, 4) || !skin) return true;
    RememberSkin(skin, player);
    if (!body.mmi || !body.uniform) {
        if (InterlockedIncrement(&poseMissing) <= 8) PoseReport("pose_without_model", skin, player, body.mmi, body.uniform);
        return false;
    }
    if (!Read(skin + 0x120, &pose, 4) || !body.buffer ||
        !Read(player + 0x1E4, &bufferCount, 4) || !bufferCount || bufferCount > 132) {
        if (InterlockedIncrement(&poseMissing) <= 8) PoseReport("pose_missing_buffer", skin, player, body.buffer, bufferCount);
        return false;
    }
    uint32_t boneIndices[132]{};
    if (!Read(skin + 0xBC, &count, 4) || count > 132 ||
        (count && (!Read(skin + 0x11C, &indices, 4) || !indices ||
                   !Read(indices, boneIndices, count * 4)))) {
        if (InterlockedIncrement(&poseInvalid) <= 8) PoseReport("pose_unreadable_indices", skin, player, count, 132);
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (boneIndices[i] >= bufferCount) {
            const LONG n = InterlockedIncrement(&poseInvalid);
            if (n <= 8) PoseReport("pose_invalid_index", skin, player, boneIndices[i], bufferCount);
            // once per ghost body: which suit it wears, and whether that suit's stream was cut by a deleted body.
            // 02.11.: two ghosts took turns and the single remembered body logged 37000 lines.
            static uint32_t namedGhosts[8]{};
            static unsigned namedNext = 0;
            static volatile LONG namedLines = 0;
            bool named = false;
            for (uint32_t g : namedGhosts) named = named || g == player;
            uint32_t worn[4]{};
            if (!named && namedLines < 32 && coop::botnet::WornSuit(player, worn)) {
                namedGhosts[namedNext++ % std::size(namedGhosts)] = player;
                InterlockedIncrement(&namedLines);
                const bool cut = std::memcmp(worn, interruptedSuit, 16) == 0;
                coop::gate::Event("coop_uniform ghost %08X animates with broken bone indices (index %u of %u, %ld times so far): it wears %08X-%08X%s; its pose is not bound "
                                  "(A-pose) so the skinning cannot read past the bone buffer",
                                  player, boneIndices[i], bufferCount, n, worn[0], worn[1],
                                  cut ? ", the suit whose stream was cut by a deleted body (test birth_suit WARN)" : "");
            }
            return false;
        }
    }
    if (pose == body.buffer) return true;
    if (stopped) return false;
    // Slot 14 reads this pointer before its native assignment at 01091AEE.
    // Use this player's buffer, exactly as that assignment does later.
    if (!WriteBytes(skin + 0x120, &body.buffer, 4)) {
        if (InterlockedIncrement(&poseInvalid) <= 8) PoseReport("pose_bind_write_failed", skin, player, body.buffer, bufferCount);
        return false;
    }
    if (pose) {
        if (InterlockedIncrement(&poseRebound) <= 16)
            PoseReport("pose_buffer_rebound", skin, player, pose, body.buffer);
    } else if (InterlockedIncrement(&poseBound) <= 16) {
        PoseReport("pose_buffer_bound", skin, player, body.buffer, bufferCount);
    }
    return true;
}
// Skins of ghost Players and the Player they belong to. The engine binds a Player skin's pose
// pointer (+0x120) to the Player's own buffer (+0x1E0, native at 0x01091AEE). A detach or a
// suit change frees that buffer, but the deferred skin queue can still run the scale pass for
// the skin afterwards; it then wrote bone matrices into freed memory, and the next heap
// consolidation crashed in the free list (0x013FC421/0x013FC442, nine crashes 19.10. to 05.11.;
// dumps 23352 and 41996: the trashed block was a freed pose buffer, a dead skin's +0x120 still
// pointed at it and the queue still named that skin).
struct SkinOwner { uint32_t skin, player; };
SkinOwner skinOwners[64]{};
SRWLOCK skinOwnersLock = SRWLOCK_INIT;
volatile LONG skinScaleRuns = 0, skinScaleSkips = 0, skinOwnersFull = 0;
void RememberSkin(uint32_t skin, uint32_t player) {
    AcquireSRWLockExclusive(&skinOwnersLock);
    SkinOwner* slot = nullptr;
    for (auto& e : skinOwners) if (e.skin == skin) { slot = &e; break; }
    if (!slot) for (auto& e : skinOwners) if (!e.skin) { slot = &e; break; }
    if (!slot)
        for (auto& e : skinOwners)
            if (!Player(e.player) || !coop::IsGhostPlayer(e.player)) { slot = &e; break; }
    if (slot) *slot = {skin, player};
    else InterlockedIncrement(&skinOwnersFull);
    ReleaseSRWLockExclusive(&skinOwnersLock);
}
// False when the skin belongs to a ghost whose buffer is gone or no longer the one the skin names.
bool SkinPoseLive(uint32_t skin) {
    uint32_t player = 0;
    AcquireSRWLockShared(&skinOwnersLock);
    for (const auto& e : skinOwners) if (e.skin == skin) { player = e.player; break; }
    ReleaseSRWLockShared(&skinOwnersLock);
    if (!player) return true;
    uint32_t pose = 0, buffer = 0;
    if (!Read(skin + 0x120, &pose, 4) || !pose) return false;
    if (!Player(player) || Detached(player) || !Read(player + 0x1E0, &buffer, 4)) return false;
    return pose == buffer;
}
void __cdecl OnSkinScale(uint32_t skin) {
    if (!SkinPoseLive(skin)) {
        const LONG n = InterlockedIncrement(&skinScaleSkips);
        if (n <= 8) coop::gate::Event("coop_uniform skin %08X: scale pass skipped, its pose buffer belongs to a ghost that is gone, detached or redressed (%ld so far)", skin, n);
        return;
    }
    InterlockedIncrement(&skinScaleRuns);
    skinScaleHook.ccall<void>(skin);
}
bool GhostDetached(uint32_t component) {
    uint32_t owner = 0;
    if (!Read(component + 0x2C, &owner, 4) || owner < 0x10010) return false;
    if (!Detached(owner - 0x10)) return false;
    InterlockedIncrement(&detachedSkips);
    return true;
}
// PlayerUniformManager::PreloadUniformStreams (0x00ED5E90, thiscall(guids, count), ret 8), the MP
// level load's own preload (LevelManager::LoadMPUniformStreams 0x0042DD50). It skips zero, bad and
// unregistered GUIDs, starts LoadUniformStream for the rest and broadcasts
// iMsgMPUniformStreamsPreloaded when done; only the LevelManager listens for that, and only during
// an MP load. A preloaded suit nobody wears stays until the first activation of another
// non-resident suit unloads every idle one, so it is asked for again when needed.
constexpr uint32_t kPreloadRva = 0xAD5E90;
bool preloadOk = false;
volatile LONG preloads = 0, preloadsResident = 0, preloadsUnregistered = 0, preloadFaults = 0;
__declspec(noinline) bool CallPreload(uint32_t manager, const uint32_t* guid) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, const uint32_t*, uint32_t)>(base + kPreloadRva)(reinterpret_cast<void*>(manager), guid, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// 1 started, 0 resident already, -1 not possible (no manager, not registered, signature).
int Preload(const uint32_t* guid) {
    uint32_t manager = 0;
    Array uniforms{}, players{};
    if (!preloadOk || !guid || !Read(base + kManager, &manager, 4) || !manager || !Manager(manager, uniforms, players)) return -1;
    if (SuitIndex(guid) < 0) { InterlockedIncrement(&preloadsUnregistered); return -1; }
    Guid want{};
    for (unsigned i = 0; i < 4; ++i) want[i] = guid[i];
    UniformEntry resident{};
    if (ResidentSuit(manager, want, resident)) { InterlockedIncrement(&preloadsResident); return 0; }
    if (!CallPreload(manager, guid)) { InterlockedIncrement(&preloadFaults); return -1; }
    InterlockedIncrement(&preloads);
    return 1;
}
// For the loading barrier: 1 the join suit is on the local Isaac and resident, 0 not yet,
// -1 nothing to wait for (no join suit, or the dress passed without it).
int JoinSuitState(char* why, size_t whySize) {
    if (!coop::loadsync::joinSuit.armed) return -1;
    if (!coop::loadsync::joinSuit.consumed) {
        if (coop::LocalSlotZeroEntity()) { std::snprintf(why, whySize, "Isaac was dressed without it (hooks %s)", installed ? "armed" : "not armed"); return -1; }
        std::snprintf(why, whySize, "no Isaac yet");
        return 0;
    }
    uint32_t manager = 0;
    if (!Read(base + kManager, &manager, 4) || !manager) { std::snprintf(why, whySize, "no uniform manager"); return -1; }
    Guid want{};
    for (unsigned i = 0; i < 4; ++i) want[i] = joinSuitWorn[i];
    UniformEntry resident{};
    if (!ResidentSuit(manager, want, resident)) { std::snprintf(why, whySize, "suit %08X-%08X still streaming", joinSuitWorn[0], joinSuitWorn[1]); return 0; }
    std::snprintf(why, whySize, "Isaac %08X wears %08X-%08X, resident", joinSuitPlayer, joinSuitWorn[0], joinSuitWorn[1]);
    return 1;
}
void WriteDiagnostics(FILE* log) {
    coop::loadsync::WriteJoinSuitDiagnostics(log);
    if (!installed) return;
    static decltype(messages) batch;
    unsigned count = 0;
    {
        std::lock_guard<std::mutex> guard(logMutex);
        count = messageCount;
        std::memcpy(batch, messages, count * sizeof(messages[0]));
        messageCount = 0;
    }
    for (unsigned i = 0; i < count; ++i) std::fprintf(log, "%s\n", batch[i]);
    static DWORD last = 0;
    if (GetTickCount() - last < 1000) return;
    last = GetTickCount();
    std::fprintf(log, "coop_uniform peer_suit selected=%ld (born without a local Isaac %ld) applied=%ld missing=%ld allow_nonresident=%d\n",
        InterlockedCompareExchange(&peerSuitSelected, 0, 0), InterlockedCompareExchange(&noDonorBirths, 0, 0),
        InterlockedCompareExchange(&peerSuitApplied, 0, 0),
        InterlockedCompareExchange(&peerSuitMissing, 0, 0), streamPeerSuit);
    std::fprintf(log, "coop_uniform redress=%ld without_donor=%ld | bot suits sent=%ld received=%ld changes=%ld\n",
        InterlockedCompareExchange(&ghostRedress, 0, 0), InterlockedCompareExchange(&noDonorDress, 0, 0),
        InterlockedCompareExchange(&coop::botnet::botSuitSent, 0, 0), InterlockedCompareExchange(&coop::botnet::botSuitIn, 0, 0),
        InterlockedCompareExchange(&coop::botnet::botSuitChanges, 0, 0));
    std::fprintf(log, "coop_uniform counts change=%ld attach=%ld load=%ld repair=%ld passed=%ld dropped=%ld stopped=%ld polls=%ld no_donor=%ld not_ready=%ld not_linked=%ld\n",
        InterlockedCompareExchange(&changes, 0, 0), InterlockedCompareExchange(&attachments, 0, 0),
        InterlockedCompareExchange(&loads, 0, 0), InterlockedCompareExchange(&repairs, 0, 0),
        InterlockedCompareExchange(&rejected, 0, 0), InterlockedCompareExchange(&overflows, 0, 0),
        InterlockedCompareExchange(&stopped, 0, 0), InterlockedCompareExchange(&polls, 0, 0),
        InterlockedCompareExchange(&noDonor, 0, 0), InterlockedCompareExchange(&notReady, 0, 0),
        InterlockedCompareExchange(&notLinked, 0, 0));
    std::fprintf(log, "coop_uniform births resident=%ld streamed=%ld (in %ld, cut %ld) unregistered=%ld missed=%ld refused_running=%ld | preloads started=%ld resident=%ld "
                      "unregistered=%ld faults=%ld ok=%d\n",
        InterlockedCompareExchange(&birthsResident, 0, 0), InterlockedCompareExchange(&birthsStreamed, 0, 0),
        InterlockedCompareExchange(&birthStreamsDone, 0, 0), InterlockedCompareExchange(&birthStreamsInterrupted, 0, 0),
        InterlockedCompareExchange(&birthsUnregistered, 0, 0), InterlockedCompareExchange(&birthsMissed, 0, 0), InterlockedCompareExchange(&birthsRefusedRunning, 0, 0),
        InterlockedCompareExchange(&preloads, 0, 0), InterlockedCompareExchange(&preloadsResident, 0, 0),
        InterlockedCompareExchange(&preloadsUnregistered, 0, 0), InterlockedCompareExchange(&preloadFaults, 0, 0), preloadOk ? 1 : 0);
    std::fprintf(log, "coop_uniform hooks=%s armable=%d arm_attempts=%ld\n", installed ? "armed" : "not armed (solo)", armable ? 1 : 0,
        InterlockedCompareExchange(&armAttempts, 0, 0));
    std::fprintf(log, "coop_uniform skin scale guard runs=%ld skipped=%ld owners_full=%ld (a skip is a write into a freed ghost pose buffer avoided)\n",
                 InterlockedCompareExchange(&skinScaleRuns, 0, 0), InterlockedCompareExchange(&skinScaleSkips, 0, 0), InterlockedCompareExchange(&skinOwnersFull, 0, 0));
    std::fprintf(log, "coop_uniform detached ghosts detaches=%ld animation_skips=%ld overflow=%ld reclaimed=%ld\n", InterlockedCompareExchange(&detaches, 0, 0),
        InterlockedCompareExchange(&detachedSkips, 0, 0), InterlockedCompareExchange(&detachedOverflow, 0, 0),
        InterlockedCompareExchange(&detachedReclaimed, 0, 0));
    std::fprintf(log, "coop_uniform pose bound=%ld rebound=%ld missing_buffer=%ld invalid_indices=%ld\n",
        InterlockedCompareExchange(&poseBound, 0, 0), InterlockedCompareExchange(&poseRebound, 0, 0), InterlockedCompareExchange(&poseMissing, 0, 0),
        InterlockedCompareExchange(&poseInvalid, 0, 0));
}
bool Install(FILE* log, const std::string& ini) {
    coop::loadsync::suitState = JoinSuitState;
    const bool requested = GetPrivateProfileIntA("Coop", "MPUniformStreams", 0, ini.c_str()) != 0;
    std::fprintf(log, "coop_uniform config MPUniformStreams=%d\n", requested);
    {
        // mov eax,[esp+8] / sub esp,0Ch / push edi / mov edi,ecx / test eax,eax / jbe / push ebp / push esi
        static const unsigned char kPreload[] = {0x8B, 0x44, 0x24, 0x08, 0x83, 0xEC, 0x0C, 0x57, 0x8B, 0xF9, 0x85, 0xC0, 0x76, 0x7A, 0x55, 0x56};
        unsigned char seen[sizeof(kPreload)]{};
        preloadOk = Read(base + kPreloadRva, seen, sizeof(seen)) && std::memcmp(seen, kPreload, sizeof(seen)) == 0;
        std::fprintf(log, "coop_uniform PreloadUniformStreams at %08X %s\n", static_cast<unsigned>(base + kPreloadRva), preloadOk ? "verified" : "signature mismatch: no preloads");
    }
    if (!requested) return false;
    struct Site { uint32_t rva; const char* bytes; void* callback; safetyhook::InlineHook* hook; };
    const Site sites[] = {
        {kChange, "\x51\x53\x8B\x5C\x24\x0C\x56\x8B\xF1\x85\xDB\x0F\x84\x71\x01\x00", reinterpret_cast<void*>(OnChange), &changeHook},
        {kAttach, "\x83\xEC\x34\x53\x56\x8B\x74\x24\x40\x33\xDB\x39\x9E\x88\x16\x00", reinterpret_cast<void*>(OnAttach), &attachHook},
        {kLoad, "\x53\x55\x8B\x6C\x24\x0C\x56\x8B\x35\x98\xC0\x01\x02\x57\x51\x8B", reinterpret_cast<void*>(OnLoad), &loadHook},
        // push ebp / mov ebp,esp / and esp,-16 / sub esp,44h / push ebx,esi,edi / mov edi,[ebp+8] / xor ebx,ebx
        {kSkinScale, "\x55\x8B\xEC\x83\xE4\xF0\x83\xEC\x44\x53\x56\x57\x8B\x7D\x08\x33", reinterpret_cast<void*>(OnSkinScale), &skinScaleHook},
    };
    for (const auto& site : sites) {
        unsigned char actual[16]{};
        if (!Read(base + site.rva, actual, 16) || std::memcmp(actual, site.bytes, 16)) {
            std::fprintf(log, "coop_uniform signature_mismatch va=%08X; disabled\n", static_cast<unsigned>(base + site.rva));
            return false;
        }
        std::fprintf(log, "coop_uniform signature_ok va=%08X\n", static_cast<unsigned>(base + site.rva));
    }
    // SuitStream now only decides whether a peer suit that is not resident here
    // may still be handed to the native ChangeToUniform, which loads it itself.
    // No extra retail function is called directly any more.
    // Default on: a partner's suit is often one this machine has unloaded, so
    // refusing to load it means the suit only arrives by luck (2026-09-17: the
    // first change reached everybody, the second nobody). Off is only for
    // bisecting a streaming crash.
    streamPeerSuit = GetPrivateProfileIntA("Coop", "SuitStream", 1, ini.c_str()) != 0;
    std::fprintf(log, "coop_uniform config SuitStream=%d (allow non-resident peer suit)\n", streamPeerSuit);
    if (!frame_cap::installed) {
        std::fprintf(log, "coop_uniform frame_hook_unavailable; disabled\n");
        return false;
    }
    const unsigned char nativeBind[] = {0x8B,0x88,0xE0,0x01,0x00,0x00,0x6A,0x00,0x6A,0x00,0x89,0x8F,0x20,0x01,0x00,0x00};
    unsigned char bindActual[sizeof(nativeBind)]{};
    if (!Read(base + 0xC91AE4, bindActual, sizeof(bindActual)) || std::memcmp(bindActual, nativeBind, sizeof(nativeBind))) {
        std::fprintf(log, "coop_uniform pose_bind_signature_mismatch; disabled\n");
        return false;
    }
    armable = true;
    std::fprintf(log, "coop_uniform ready; hooks are armed when the first partner connects (solo keeps the retail suit change for the debug menu)\n");
    g_uniformPoll = Poll;
    if (coop::ghostAnimHook2) g_uniformPreparePose = PreparePose;
    g_uniformGhostDetached = GhostDetached;
    std::fprintf(log, "coop_uniform pose_guard=%d\n", g_uniformPreparePose != nullptr);
    std::fprintf(log, "coop_uniform ready singleton=%08X arrays=10/1C player_uniform=1688 player_initial=5A0\n", static_cast<unsigned>(base + kManager));
    return true;
}
// First connected partner: put the three hooks in. Idempotent.
bool Arm() {
    if (installed) return true;
    if (!armable) return false;
    InterlockedIncrement(&armAttempts);
    struct Site { uint32_t rva; void* callback; safetyhook::InlineHook* hook; };
    const Site sites[] = {
        {kChange, reinterpret_cast<void*>(OnChange), &changeHook},
        {kAttach, reinterpret_cast<void*>(OnAttach), &attachHook},
        {kLoad, reinterpret_cast<void*>(OnLoad), &loadHook},
        {kSkinScale, reinterpret_cast<void*>(OnSkinScale), &skinScaleHook},
    };
    for (const auto& site : sites) {
        *site.hook = safetyhook::create_inline(reinterpret_cast<void*>(base + site.rva), site.callback);
        if (!*site.hook) {
            for (const auto& undo : sites) undo.hook->reset();
            armable = false;   // do not retry every tick
            return false;
        }
    }
    armedAt = GetTickCount();
    installed = true;
    return true;
}
// For the companion bot (runtime_coop_bot.inc): its body, the local player's
// resident suit, and the native ChangeToUniform with that suit (the same call
// that gives a partner's ghost a body of its own, "independent_body_ready").
// Called through the retail entry; when the hooks are armed, nesting keeps
// OnChange from redirecting it.
bool BotBody(uint32_t player, uint32_t* out) {
    const Body b = ReadBody(player);
    out[0] = b.mmi; out[1] = b.bones; out[2] = b.uniform;
    return Complete(b);
}
bool BotSuit(uint32_t* out) {
    uint32_t manager = 0;
    Guid mine{};
    if (!Read(base + kManager, &manager, 4) || !manager || !LocalSuit(manager, mine)) return false;
    std::memcpy(out, mine.data(), 16);
    return true;
}
// Another suit for a bot: the n-th registered uniform that is not the local
// player's (loaded or not: ChangeToUniform streams it in, as for a store change).
// Returns its index in the manager's list, or -1.
int BotOtherSuit(unsigned n, uint32_t* out) {
    uint32_t manager = 0;
    Guid mine{};
    Array uniforms{}, players{};
    if (!Read(base + kManager, &manager, 4) || !manager || !LocalSuit(manager, mine) || !Manager(manager, uniforms, players)) return -1;
    int candidates[128];
    unsigned count = 0;
    for (uint32_t i = 0; i < uniforms.count && count < 128; ++i) {
        UniformEntry suit{};
        if (!Read(uniforms.data + i * 24, &suit, 24) || suit.dlc || !suit.stream) continue;
        if (std::memcmp(suit.guid.data(), mine.data(), 16) == 0) continue;
        if (coop::mpsuit::IsMpSuit(suit.guid.data())) continue;   // needs a skin the bots do not pick
        candidates[count++] = static_cast<int>(i);
    }
    if (!count) return -1;
    const int pick = candidates[n % count];
    UniformEntry suit{};
    if (!Read(uniforms.data + pick * 24, &suit, 24)) return -1;
    std::memcpy(out, suit.guid.data(), 16);
    return pick;
}
// A suit's index in the manager's list, -1 when it is not registered (dressing
// in an unregistered one leaves the body bare).
int SuitIndex(const uint32_t* guid) {
    uint32_t manager = 0;
    Array uniforms{}, players{};
    if (!Read(base + kManager, &manager, 4) || !manager || !Manager(manager, uniforms, players)) return -1;
    for (uint32_t i = 0; i < uniforms.count; ++i) {
        UniformEntry suit{};
        if (Read(uniforms.data + i * 24, &suit, 24) && std::memcmp(suit.guid.data(), guid, 16) == 0) return static_cast<int>(i);
    }
    return -1;
}
__declspec(noinline) bool BotDress(uint32_t player, const uint32_t* guid) {
    uint32_t manager = 0;
    if (!Read(base + kManager, &manager, 4) || !manager || !Player(player)) return false;
    bool ok = false;
    ++nesting;
    __try {
        reinterpret_cast<void(__thiscall*)(void*, uint32_t, const uint32_t*)>(base + kChange)(reinterpret_cast<void*>(manager), player, guid);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    --nesting;
    return ok;
}
} // namespace coop_uniform
