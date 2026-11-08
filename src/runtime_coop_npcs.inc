// Dead Space 2 United by Hafer.
namespace npcs {
bool enabled = false, client = false;
// Scripted actors are local. The stasis room (ch01, 17:50): the necromorph
// that grabs Isaac is spawned by the same chain that starts the sequence, on
// the host only; the client's sequence then had nobody to grab him and hung
// until the gate's timeout. Ghosts do not act in sequences: they are moved
// by the host. So a spawn that arrives within this window after a
// ScriptedSequence message fired here is spawned locally on a client as
// well. Where the host spawned the same actor in the same moment the client
// briefly has two (its own and the ghost); where the host did it long ago
// it has the one it needs.
bool scriptedLocal = true;
DWORD scriptedWindowMs = 3000;
volatile LONG scriptedAllowed = 0;
// NeutralNPCs are story actors driven by the room's own triggers and
// sequences. 23.09.: the client's Edgar (35CA9AC7-B21859F1) was the host's
// ghost, so when the host walked back into the hall his trigger put Edgar to
// idle for the client standing in front of him, and the knife the host's
// script attaches never existed there. With this on, every machine spawns
// and keeps its own neutrals and the host does not ghost them.
bool neutralsLocal = false;
constexpr uint32_t kNeutralDescriptorRva = 0x1CB4830, kNeutralVtableRva = 0x1784EE8;   // classID 82C7DF9A, checked live 23.09.
volatile LONG neutralSpawnsLocal = 0, neutralsNotGhosted = 0;
// More classes every machine spawns and runs itself ([Coop] LocalNPCClasses).
// NicoleBoss (24.09.): her state machine only runs where she is not a ghost,
// so the client's copy never faded out, never brought in the marker heart and
// took none of the client's shots. Each machine now fights its own Nicole and
// hits::OnNicoleModifiers shares the damage.
struct LocalClass { const char* name; uint32_t descriptorRva, vtableRva; bool on; };
LocalClass localClasses[] = {
    {"NicoleBoss", 0x1CAAFA0, 0x1774B78, false},   // classID E2FDCD0D; the ctor 0x0107D7E0 writes the vtable, checked live 24.09.
};
volatile LONG localClassSpawns = 0, localClassNotGhosted = 0;
const LocalClass* LocalByDescriptor(uint32_t descriptor) {
    for (const auto& c : localClasses) if (c.on && descriptor == base + c.descriptorRva) return &c;
    return nullptr;
}
const LocalClass* LocalByVtable(uint32_t vtable) {
    for (const auto& c : localClasses) if (c.on && vtable == base + c.vtableRva) return &c;
    return nullptr;
}
bool AnyLocalClass() {
    for (const auto& c : localClasses) if (c.on) return true;
    return false;
}
// A client that spawns a sequence's actor itself (scriptedLocal) also gets
// the host's ghost of it. 24.09.: Tiedemann (NeutralNPC 52CB3804-29D0B83A)
// stood there twice after the ch11 insider scene. While a local twin with the
// same GUID exists the ghost is hidden through the Entity hide slot (+0x68,
// the one the cutscene module uses; NeutralNPC's is the same 0x00754750) and
// shown again (+0x6C) when the twin is gone. Only NeutralNPC ghosts: a hidden
// enemy ghost would still attack.
bool hideGhostTwins = false;   // [Coop] HideNeutralGhostTwins
struct HiddenTwin { uint32_t ghost, vtable; uint32_t guid[4]; };
HiddenTwin hiddenTwins[16]{};
DWORD lastTwinCheck = 0;
volatile LONG twinsHidden = 0, twinsShown = 0, twinsGone = 0, twinsFull = 0, twinFaults = 0, twinsRehidden = 0, twinWalksCut = 0;
safetyhook::InlineHook spawnHook;
volatile LONG spawnCalls = 0, suppressed = 0, unknown = 0, registered = 0, existing = 0, invalid = 0, coordinatorsSeen = 0;
// Where the scan loses its candidates. npc_registered stayed 0 through every
// session while the client suppressed real spawns (invisible attackers), and
// the offline walk of the live list (dump 34144, 22.09.) showed four nodes
// that pass every static check, so the loss happens at run time and only a
// per-stage count can say where. reject_rep also records the first class reps
// that were turned away, which is how the coordinator's own rep was found.
volatile LONG scanRuns = 0, nodesWalked = 0, rejectVtable = 0, rejectGetter = 0, rejectRep = 0, rejectBody = 0, rejectProxy = 0, rejectSlots = 0, rejectKey = 0;
uint32_t seenReps[8]{};
unsigned seenRepCount = 0;
void NoteRep(uint32_t rep) {
    for (unsigned i = 0; i < seenRepCount; ++i) if (seenReps[i] == rep) return;
    if (seenRepCount < std::size(seenReps)) seenReps[seenRepCount++] = rep;
}
// PairedAttackCoordinator's NetReflectable subobject sits at coordinator+0x58
// and its constructor (0x01092440) writes this vtable there. Its slot +0x78
// returns 0x020AB668, not the factory descriptor 0x020ACFD0 the old check
// compared against, so no coordinator was ever recognised. The vtable the
// constructor writes is the exact test; the slots +8/+0x30 differ from the
// NPC ones as well (0x01076C20 / a folded no-op), so they are only required
// to lie inside the image for a coordinator.
constexpr uint32_t kCoordinatorReflectableVtableRva = 0x1773150;
DWORD lastScan = 0;
std::mutex logMutex;
char messages[64][224]{};
unsigned messageCount = 0;
volatile LONG logDropped = 0;

void Record(const char* action, uint32_t object, uint32_t type, uint32_t key, uint32_t proxy, uint32_t guidAddress) {
    uint32_t guid[4]{};
    const bool readable = guidAddress && Read(guidAddress, guid, sizeof(guid));
    std::lock_guard<std::mutex> guard(logMutex);
    if (messageCount == std::size(messages)) { InterlockedIncrement(&logDropped); return; }
    std::snprintf(messages[messageCount++], sizeof(messages[0]),
        "coop_npc action=%s object=%08X class=%08X key=%08X proxy=%08X guid_readable=%d guid=%08X-%08X-%08X-%08X",
        action, object, type, key, proxy, readable, guid[0], guid[1], guid[2], guid[3]);
}
void Flush(FILE* log) {
    if (!enabled) return;
    char pending[64][224]{};
    unsigned count = 0;
    {
        std::lock_guard<std::mutex> guard(logMutex);
        count = messageCount;
        std::memcpy(pending, messages, count * sizeof(messages[0]));
        messageCount = 0;
    }
    for (unsigned i = 0; i < count; ++i) std::fprintf(log, "%s\n", pending[i]);
}

// Grab trace. The coordinator is not created per grab: it is built once per
// player from the PlayerSettings and then carries the pairing in its fields,
// so a grab is a field update on an object that already exists on both
// machines. Its constructor (0x01092440) fills +0x78 and +0x80 with the
// sentinel 0xFE16702F and zeroes +0x68..+0xC4. Both sides log every change of
// that block, with whether the object is local (a proxy registered here) or an
// incoming ghost, so one grab says where the chain stops: unchanged fields on
// the sender mean the grab does not reach the coordinator at all, changed on
// the sender and unchanged on the receiver mean the update is not transmitted.
constexpr uint32_t kCoordinatorSentinel = 0xFE16702Fu;
struct CoordinatorWatch { uint32_t object, snapshot[8]; bool seen; };
CoordinatorWatch coordinatorWatch[4]{};
volatile LONG coordinatorChanges = 0;
void WatchCoordinator(uint32_t object, uint32_t proxy) {
    uint32_t now[8]{};
    if (!Read(object + 0x68, now, sizeof(now))) return;   // +0x68 .. +0x84
    for (auto& w : coordinatorWatch) {
        if (w.object && w.object != object) continue;
        if (!w.object) { w.object = object; std::memcpy(w.snapshot, now, sizeof(now)); w.seen = true;
            Record("coordinator_seen", object, proxy, now[4], now[6], 0); return; }
        if (std::memcmp(w.snapshot, now, sizeof(now)) == 0) return;
        std::memcpy(w.snapshot, now, sizeof(now));
        InterlockedIncrement(&coordinatorChanges);
        // A partner's copy has a proxy too; its ghost bit (proxy+0x30 & 2) tells them apart.
        uint32_t proxyFlags = 0;
        const bool ghost = proxy && Read(proxy + 0x30, &proxyFlags, 4) && (proxyFlags & 2);
        Record(ghost ? "coordinator_changed_ghost" : "coordinator_changed_ours", object, now[0], now[4], now[6], 0);
        return;
    }
}
// PairedAttackCoordinator::OnReflectableAvailable (0x010E2A60, thiscall(const NetID*, uint32_t),
// ret 8) clears the "waiting for the defender" bit (0x20 at +0x10) only when the arriving
// NetID equals the ATTACKER's (+0x20/+0x24, 0x010E2A89; the defender's is +0x28/+0x2C), so a
// partner's grab copy that unpacked before its defender's ghost never resolved and played
// nothing (16.10., grab 1). The bit is cleared here for the defender's NetID; the original
// then resolves both fighters as it does for the attacker.
bool pairDefenderFix = true;   // [Coop] PairDefenderWaitFix
safetyhook::InlineHook pairAvailableHook;
volatile LONG pairAvailableCalls = 0, pairDefenderWaitsFixed = 0;
void __fastcall OnPairAvailable(void* self, void*, const uint32_t* id, uint32_t arg) {
    InterlockedIncrement(&pairAvailableCalls);
    const uint32_t coordinator = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    unsigned char flags = 0;
    uint32_t defender[2]{}, incoming[2]{};
    if (pairDefenderFix && id && Read(coordinator + 0x10, &flags, 1) && (flags & 0x20) && Read(coordinator + 0x28, defender, 8) &&
        Read(reinterpret_cast<uintptr_t>(id), incoming, 8) && incoming[0] == defender[0] && incoming[1] == defender[1]) {
        const unsigned char cleared = static_cast<unsigned char>(flags & ~0x20);
        if (WriteBytes(coordinator + 0x10, &cleared, 1)) {
            InterlockedIncrement(&pairDefenderWaitsFixed);
            Record("coordinator_defender_arrived", coordinator, flags, defender[0], defender[1], 0);
        }
    }
    pairAvailableHook.thiscall<void>(self, id, arg);
}
bool __fastcall OnShouldSpawn(void* filter, void*, bool prior, uint32_t info) {
    InterlockedIncrement(&spawnCalls);
    const bool allowed = spawnHook.thiscall<bool>(filter, prior, info);
    if (!allowed || !client) return allowed;
    // the Dementia mutator's phantom: our own spawn, it stays on this machine
    if (InterlockedCompareExchange(&phantom::spawning, 0, 0)) { InterlockedIncrement(&phantom::allowedOnClient); return allowed; }
    uint32_t mode = 0, packet = 0, descriptor = 0;
    if (!Read(reinterpret_cast<uintptr_t>(filter) + 8, &mode, 4) || mode != 0 ||
        !info || !Read(info + 0x10, &packet, 4) || !packet || !Read(packet, &descriptor, 4)) {
        InterlockedIncrement(&unknown); return allowed;
    }
    const auto type = npc_policy::Classify(descriptor);
    if (type == npc_policy::Classification::Unknown) InterlockedIncrement(&unknown);
    if (type != npc_policy::Classification::NPC) return allowed;
    uint32_t classID = 0, guidAddress = 0;
    Read(descriptor + 8, &classID, 4);
    Read(info + 0xC, &guidAddress, 4);
    if (neutralsLocal && descriptor == base + kNeutralDescriptorRva) {
        InterlockedIncrement(&neutralSpawnsLocal);
        Record("allowed_neutral", info, classID, 0, 0, guidAddress);
        return allowed;
    }
    if (LocalByDescriptor(descriptor)) {
        InterlockedIncrement(&localClassSpawns);
        Record("allowed_local_class", info, classID, 0, 0, guidAddress);
        return allowed;
    }
    if (scriptedLocal) {
        const DWORD sequenceAt = static_cast<DWORD>(InterlockedCompareExchange(&gate::lastSequenceTick, 0, 0));
        if (sequenceAt && GetTickCount() - sequenceAt <= scriptedWindowMs) {
            InterlockedIncrement(&scriptedAllowed);
            Record("allowed_scripted", info, classID, 0, 0, guidAddress);
            return allowed;
        }
    }
    InterlockedIncrement(&suppressed);
    Record("suppressed", info, classID, 0, 0, guidAddress);
    return false;
}

// coordinatorOnly: the client's pass. A client never registers NPCs (the host
// owns them), but a grab coordinator spawned on the client (the client's Isaac
// grabbed by the host's necro) is the client's own object, like its Isaac.
// The network class of a NetReflectable, 0 when its getter is not a plain one.
// The class rep getter sits in vtable slot +0x78; the slot may hold an
// incremental-link thunk (jmp rel32) in front of the real getter.
uint32_t ClassRepOf(uint32_t reflectable) {
    uint32_t vt = 0, getter = 0;
    if (!Read(reflectable, &vt, 4) || !vt || !Read(vt + 0x78, &getter, 4) || getter < base || getter >= base + hits::kCodeEnd) return 0;
    for (int hop = 0; hop < 2; ++hop) {
        unsigned char op = 0; int32_t rel = 0;
        if (!Read(getter, &op, 1) || op != 0xE9 || !Read(getter + 1, &rel, 4)) break;
        getter = static_cast<uint32_t>(getter + 5 + rel);
        if (getter < base || getter >= base + hits::kCodeEnd) return 0;
    }
    unsigned char head[6]{};
    if (!Read(getter, head, sizeof(head)) || head[5] != 0xC3 || (head[0] != 0xA1 && head[0] != 0xB8)) return 0;   // only "mov eax,[mem]; ret" / "mov eax,imm; ret" getters are called
    using GetClassRep = uint32_t (__thiscall*)(void*);
    return reinterpret_cast<GetClassRep>(getter)(reinterpret_cast<void*>(reflectable));
}
bool IsNpcReflectable(uint32_t reflectable) { return ClassRepOf(reflectable) == base + 0x1CA8328; }   // DeadSpaceNPC dynClassRep 0x020A8328

void Register(uint32_t reflectable, bool coordinatorOnly) {
    uint32_t vt = 0, proxy = 0, setKey = 0, init = 0;
    if (!Read(reflectable, &vt, 4) || !vt) { InterlockedIncrement(&rejectVtable); return; }
    const bool coordinatorByVtable = vt == base + kCoordinatorReflectableVtableRva;
    bool coordinator = coordinatorByVtable, npc = false;
    if (!coordinatorByVtable) {
        const uint32_t classRep = ClassRepOf(reflectable);
        if (!classRep) { InterlockedIncrement(&rejectGetter); return; }
        coordinator = classRep == base + 0x1CACFD0;   // PairedAttackCoordinator factory descriptor 0x020ACFD0
        npc = classRep == base + 0x1CA8328;           // DeadSpaceNPC dynClassRep 0x020A8328
        if (!coordinator && !npc) { InterlockedIncrement(&rejectRep); NoteRep(classRep); return; }
    }
    if (coordinator) {
        InterlockedIncrement(&coordinatorsSeen);
        uint32_t existingProxy = 0;
        Read(reflectable + 0xC, &existingProxy, 4);
        WatchCoordinator(reflectable - 0x58, existingProxy);   // the coordinator's NetReflectable sits at +0x58, not +0x5C
    }
    else if (coordinatorOnly) { InterlockedIncrement(&rejectRep); return; }
    if (IsPlayerOrNecroEntity(reflectable - 0x5C)) { InterlockedIncrement(&rejectBody); return; }   // bodies register themselves (RegisterLocalPlayer / necro), never as NPCs
    if (neutralsLocal && !coordinator) {
        uint32_t entityVtable = 0;
        if (Read(reflectable - 0x5C, &entityVtable, 4) && entityVtable == base + kNeutralVtableRva) {
            if (InterlockedIncrement(&neutralsNotGhosted) <= 32) Record("kept_neutral", reflectable, entityVtable, 0, 0, reflectable - 0x38);
            return;
        }
    }
    if (!coordinator && AnyLocalClass()) {
        uint32_t entityVtable = 0;
        if (Read(reflectable - 0x5C, &entityVtable, 4) && LocalByVtable(entityVtable)) {
            if (InterlockedIncrement(&localClassNotGhosted) <= 32) Record("kept_local_class", reflectable, entityVtable, 0, 0, reflectable - 0x38);
            return;
        }
    }
    if (!coordinator && phantom::Is(reflectable - 0x5C)) { InterlockedIncrement(&phantom::keptOnHost); return; }   // a hallucination, never ghosted
    if (!Read(reflectable + 0xC, &proxy, 4)) { InterlockedIncrement(&invalid); return; }
    if (proxy) { InterlockedIncrement(&existing); InterlockedIncrement(&rejectProxy); return; }
    if (!Read(vt + 8, &setKey, 4) || !Read(vt + 0x30, &init, 4) ||
        setKey < base || setKey >= base + hits::kCodeEnd || init < base || init >= base + hits::kCodeEnd) { InterlockedIncrement(&invalid); InterlockedIncrement(&rejectSlots); return; }
    if (!coordinator && (setKey != base + 0x3D1D10 || init != base + 0x3D1DC0)) { InterlockedIncrement(&invalid); InterlockedIncrement(&rejectSlots); return; }
    uint32_t key = 0;
    if (!AllocatePlayerSubKey(session, machineIndex, key)) { InterlockedIncrement(&invalid); InterlockedIncrement(&rejectKey); return; }
    Record(coordinator ? "register_begin_coordinator" : "register_begin", reflectable, vt, key, 0, reflectable - 0x38);
    using Set = void (__thiscall*)(void*, uint32_t);
    using Init = void (__thiscall*)(void*);
    reinterpret_cast<Set>(setKey)(reinterpret_cast<void*>(reflectable), key);
    reinterpret_cast<Init>(init)(reinterpret_cast<void*>(reflectable));
    reinterpret_cast<Set>(base + kAddToSession)(reinterpret_cast<void*>(reflectable), 0);
    if (Read(reflectable + 0xC, &proxy, 4) && proxy) {
        InterlockedIncrement(&registered);
        Record("registered", reflectable, vt, key, proxy, reflectable - 0x38);
    }
    else { InterlockedIncrement(&invalid); faulted = true; replicationReady = false; tickMode = 0; }
}
bool CallEntitySlot(uint32_t entity, uint32_t slot) {
    uint32_t vtable = 0, fn = 0;
    if (!Read(entity, &vtable, 4) || !Read(vtable + slot, &fn, 4) || fn < base + 0x1000 || fn >= base + gate::kRdataLo) return false;
    __try {
        reinterpret_cast<void(__thiscall*)(void*)>(fn)(reinterpret_cast<void*>(entity));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&twinFaults);
        return false;
    }
}
void HideGhostTwins() {
    const DWORD now = GetTickCount();
    if (!hideGhostTwins || now - lastTwinCheck < 100) return;
    lastTwinCheck = now;
    struct Seen { uint32_t entity, guid[4]; bool ghost, neutral; };
    static Seen seen[2048];
    unsigned count = 0, steps = 0;
    uint32_t node = 0;
    Read(base + 0x1C5B744, &node, 4);
    for (; node && steps < 4096 && count < std::size(seen); ++steps) {
        const uint32_t entity = node - 0x5C;
        uint32_t vtable = 0, reflectableVtable = 0;
        if (Read(node, &reflectableVtable, 4) && reflectableVtable != base + kCoordinatorReflectableVtableRva &&
            Read(entity, &vtable, 4) && vtable >= base + gate::kRdataLo && vtable < base + gate::kRdataHi &&
            !IsPlayerOrNecroEntity(entity)) {
            Seen& e = seen[count];
            if (Read(entity + 0x24, e.guid, sizeof(e.guid)) && (e.guid[0] | e.guid[1] | e.guid[2] | e.guid[3])) {
                e.entity = entity;
                e.ghost = guidfix::IsGhostEntity(entity);
                e.neutral = vtable == base + kNeutralVtableRva;
                ++count;
            }
        }
        uint32_t next = 0;
        if (!Read(node + 4, &next, 4) || next == node) { node = 0; break; }
        node = next;
    }
    // A walk cut short by a cap has not seen every entity: a hidden ghost missing
    // from it is not gone, and is kept (shown again only once a full walk says so).
    const bool complete = node == 0;
    if (!complete) InterlockedIncrement(&twinWalksCut);
    auto present = [&](uint32_t entity) { for (unsigned i = 0; i < count; ++i) if (seen[i].entity == entity) return true; return false; };
    auto twinOf = [&](const uint32_t* guid, uint32_t except) {
        for (unsigned i = 0; i < count; ++i)
            if (!seen[i].ghost && seen[i].entity != except && std::memcmp(seen[i].guid, guid, 16) == 0) return seen[i].entity;
        return 0u;
    };
    // Hidden ghosts first: gone with the level, or their twin is gone.
    for (auto& h : hiddenTwins) {
        if (!h.ghost) continue;
        uint32_t vtable = 0;
        // A partial walk decides nothing about a ghost or twin it did not reach.
        if (!complete && (!present(h.ghost) || !twinOf(h.guid, h.ghost))) continue;
        if (!present(h.ghost) || !Read(h.ghost, &vtable, 4) || vtable != h.vtable) { InterlockedIncrement(&twinsGone); h = HiddenTwin{}; continue; }
        if (twinOf(h.guid, h.ghost)) {
            // The host's updates show his NPC again (24.09.: the intro's Franco stood twice on
            // the client although his ghost had been hidden): hidden again whenever it shows.
            unsigned char hiddenCount = 0;
            if (Read(h.ghost + 0x16D, &hiddenCount, 1) && !hiddenCount && CallEntitySlot(h.ghost, 0x68) &&
                InterlockedIncrement(&twinsRehidden) <= 16)
                Record("ghost_twin_rehidden", h.ghost, h.vtable, 0, 0, h.ghost + 0x24);
            continue;
        }
        if (CallEntitySlot(h.ghost, 0x6C)) {
            InterlockedIncrement(&twinsShown);
            Record("ghost_twin_shown", h.ghost, h.vtable, 0, 0, h.ghost + 0x24);
        }
        h = HiddenTwin{};
    }
    for (unsigned i = 0; i < count; ++i) {
        const Seen& e = seen[i];
        if (!e.ghost || !e.neutral) continue;
        bool already = false;
        for (const auto& h : hiddenTwins) if (h.ghost == e.entity) { already = true; break; }
        if (already || !twinOf(e.guid, e.entity)) continue;
        HiddenTwin* slot = nullptr;
        for (auto& h : hiddenTwins) if (!h.ghost) { slot = &h; break; }
        if (!slot) { InterlockedIncrement(&twinsFull); continue; }
        uint32_t vtable = 0;
        if (!Read(e.entity, &vtable, 4) || !CallEntitySlot(e.entity, 0x68)) continue;
        slot->ghost = e.entity;
        slot->vtable = vtable;
        std::memcpy(slot->guid, e.guid, sizeof(slot->guid));
        InterlockedIncrement(&twinsHidden);
        Record("ghost_twin_hidden", e.entity, vtable, 0, 0, e.entity + 0x24);
    }
}

void Scan() {
    if (!enabled || !replicationReady || faulted) return;   // the client scans too, for its own grab coordinators only
    const DWORD now = GetTickCount();
    if (now - lastScan < 100) return;
    lastScan = now;
    InterlockedIncrement(&scanRuns);
    __try {
        uint32_t node = 0;
        Read(base + 0x1C5B744, &node, 4);
        unsigned additions = 0;
        for (unsigned count = 0; node && count < 4096 && additions < 8 && !faulted; ++count) {
            InterlockedIncrement(&nodesWalked);
            uint32_t next = 0;
            if (!Read(node + 4, &next, 4)) { InterlockedIncrement(&invalid); break; }
            const LONG before = InterlockedCompareExchange(&registered, 0, 0);
            Register(node, client);
            if (before != InterlockedCompareExchange(&registered, 0, 0)) ++additions;
            if (next == node) break;
            node = next;
        }
        if (client) HideGhostTwins();
    } __except (TickFilter(GetExceptionCode(), GetExceptionInformation())) {
        faulted = true; replicationReady = false; tickMode = 0;
    }
}

// A paired attack whose partner is a local class. 24.09. (dump 28460): the
// host's Nicole grabbed the host (mm_kill_player), the host's
// PairedAttackCoordinator reached the client as a ghost, and a state of its
// machine (entered at 0x010D35E0 from the ghost update) read its participant
// SafePtr at +0x88 (the host's Nicole, which the client never has now) and
// called 0x00EA51E0 on null. That setter and its neighbour 0x00EA5100 (both
// thiscall(one argument), ret 4, on the participant actor) return here when
// there is no actor; the rest of that state already tests its pointers.
safetyhook::InlineHook pairSetHook, pairFlagHook, pairEnterHook;
volatile LONG pairNullSkipped = 0, pairEnterSkipped = 0, pairEnterFaults = 0;
// The state entry itself (0x010D35E0, thiscall, no arguments, plain ret)
// dereferences both participants again after the setters (dump 28924:
// [null+0x5C] at 0x010D3683). Without both it is skipped as a whole.
void __fastcall OnPairEnter(void* state) {
    uint32_t owner = 0, a = 0, b = 0;
    const uint32_t s = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(state));
    if (s && Read(s + 0x40, &owner, 4) && owner && Read(owner + 0x88, &a, 4) && Read(owner + 0x90, &b, 4) && (!a || !b)) {
        InterlockedIncrement(&pairEnterSkipped);
        return;
    }
    // Dump 28128 (24.09. 18:34): both were there on entry, and the first one
    // was gone by 0x010D3683: one of the calls in between let go of it.
    // The rest of the state uses both without a test, so a fault in it ends
    // the state here instead of the game.
    __try {
        pairEnterHook.thiscall<void>(state);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        InterlockedIncrement(&pairEnterFaults);
    }
}
void __fastcall OnPairSet(void* self, void*, uint32_t argument) {
    if (!self) { InterlockedIncrement(&pairNullSkipped); return; }
    pairSetHook.thiscall<void>(self, argument);
}
void __fastcall OnPairFlag(void* self, void*, uint32_t argument) {
    if (!self) { InterlockedIncrement(&pairNullSkipped); return; }
    pairFlagHook.thiscall<void>(self, argument);
}
void InstallPairGuard(FILE* log) {
    // mov eax,[esp+4] / push esi / mov esi,ecx / push edi / lea edi,[esi+0x5C]
    static const unsigned char kSet[] = {0x8B, 0x44, 0x24, 0x04, 0x56, 0x8B, 0xF1, 0x57, 0x8D, 0x7E, 0x5C};
    // push ebx / mov ebx,[esp+8] / push esi / mov esi,ecx / mov al,[esi+0x290]
    static const unsigned char kFlag[] = {0x53, 0x8B, 0x5C, 0x24, 0x08, 0x56, 0x8B, 0xF1, 0x8A, 0x86, 0x90, 0x02, 0x00, 0x00};
    const bool a = hook_site::Install(log, "coop_npcs pair guard set", base + 0xAA5100, kSet, sizeof(kSet),
                                      reinterpret_cast<void*>(OnPairSet), pairSetHook);
    const bool b = hook_site::Install(log, "coop_npcs pair guard flag", base + 0xAA51E0, kFlag, sizeof(kFlag),
                                      reinterpret_cast<void*>(OnPairFlag), pairFlagHook);
    // push ebp / mov ebp,esp / and esp,-16 / sub esp,0x14 / push ebx / push esi / mov esi,ecx
    static const unsigned char kEnter[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x14, 0x53, 0x56, 0x8B, 0xF1};
    const bool c = hook_site::Install(log, "coop_npcs pair guard enter", base + 0xCD35E0, kEnter, sizeof(kEnter),
                                      reinterpret_cast<void*>(OnPairEnter), pairEnterHook);
    std::fprintf(log, "coop_npcs pair_guard enter=%d\n", c ? 1 : 0);
    std::fprintf(log, "coop_npcs pair_guard set=%d flag=%d (a paired attack without its local partner skips the two setters)\n", a ? 1 : 0, b ? 1 : 0);
}
// Sound occlusion of ghost NPCs. 24.09. (live, two Leapers): on the client the
// ghost Leaper's sounds carried SCENEMASK_OCCLUDED (emitter scene mask
// 0x0100FFFF, host 0x0000FFFF) and sounded muffled. Its RouteOcclusionComponent
// (vtable 0x01B41B74, owner at +0x10, m_result +0x1C: 3 on the client, 1 on the
// host; Flags16 +0x2C = 0x30, TEST_LOS | TEST_NAVPOWER) runs the NavPower test
// with the ghost's nav routing state, which only the host's AI keeps (the
// ghost's DSNavRoutingComponent position was empty), so the route never
// exists and the enemy counts as behind a wall. GhostOcclusion=1 drops the
// NavPower test on ghosts and keeps the line-of-sight one; 2 disables the
// occlusion on ghosts altogether (OCCLUSIONFLAG_DISABLED). 1 was not enough
// (24.09. 21:30): with LOS alone the client still saw result 3 on 111 visits
// and heard pack boys in the open muffled, so the LOS test needs host-side
// state too.
int ghostOcclusion = 0;   // [Coop] GhostOcclusion
constexpr uint32_t kRouteOcclusionVtableRva = 0x1741B74, kOcclusionOwner = 0x10, kOcclusionResult = 0x1C, kOcclusionFlags = 0x2C;
constexpr uint16_t kOcclusionDisabled = 0x01, kOcclusionTestNav = 0x20;
DWORD occlusionTickAt = 0;
volatile LONG occlusionGhosts = 0, occlusionFound = 0, occlusionChanged = 0, occlusionMissing = 0, occlusionOccludedSeen = 0;
struct OcclusionSeen { uint32_t entity, component; };
OcclusionSeen occlusionSeen[128]{};
unsigned occlusionSeenCount = 0, occlusionSeenNext = 0;
uint32_t FindOcclusionComponent(uint32_t entity) {
    for (unsigned i = 0; i < occlusionSeenCount; ++i) {
        if (occlusionSeen[i].entity != entity) continue;
        uint32_t vtable = 0, owner = 0;
        const uint32_t c = occlusionSeen[i].component;
        if (c && Read(c, &vtable, 4) && vtable == base + kRouteOcclusionVtableRva && Read(c + kOcclusionOwner, &owner, 4) && owner == entity) return c;
        occlusionSeen[i].entity = 0;
    }
    // Components follow the entity in the same allocation (0x900..0x2400 for a
    // Leaper). Read in pieces so an uncommitted page at the end costs only the rest.
    uint32_t block[0x1000]{};
    unsigned words = 0;
    for (unsigned piece = 0; piece < 4; ++piece) {
        if (!Read(entity + piece * 0x1000, block + piece * 0x400, 0x1000)) break;
        words += 0x400;
    }
    if (!words) return 0;
    uint32_t found = 0;
    for (unsigned i = 0x40; i + 4 < words; ++i)
        if (block[i] == base + kRouteOcclusionVtableRva && block[i + 4] == entity) { found = entity + i * 4; break; }
    const unsigned slot = occlusionSeenNext++ % std::size(occlusionSeen);
    occlusionSeen[slot] = OcclusionSeen{entity, found};
    if (occlusionSeenCount < std::size(occlusionSeen)) ++occlusionSeenCount;
    return found;
}
void GhostOcclusionTick(DWORD now) {
    if (!ghostOcclusion || !inLevel || now - occlusionTickAt < 1000) return;
    occlusionTickAt = now;
    uint32_t node = 0;
    if (!Read(base + guidfix::kReflectableListRva, &node, 4)) return;
    for (unsigned count = 0; node && count < 4096; ++count) {
        const uint32_t entity = node - guidfix::kEntityReflectable;
        uint32_t next = 0;
        if (!Read(node + 4, &next, 4)) break;
        if (guidfix::IsGhostEntity(entity)) {
            InterlockedIncrement(&occlusionGhosts);
            const uint32_t component = FindOcclusionComponent(entity);
            if (!component) {
                InterlockedIncrement(&occlusionMissing);
            } else {
                InterlockedIncrement(&occlusionFound);
                uint16_t flags = 0;
                uint32_t result = 0;
                Read(component + kOcclusionResult, &result, 4);
                if (result == 3) InterlockedIncrement(&occlusionOccludedSeen);
                if (Read(component + kOcclusionFlags, &flags, 2)) {
                    const uint16_t wanted = ghostOcclusion == 2 ? static_cast<uint16_t>(flags | kOcclusionDisabled)
                                                               : static_cast<uint16_t>(flags & ~kOcclusionTestNav);
                    if (wanted != flags) {
                        WriteBytes(component + kOcclusionFlags, &wanted, 2);
                        // A disabled test no longer updates its result: the last
                        // "occluded" (3) would stay. 1 is what an audible enemy shows.
                        if (ghostOcclusion == 2 && result != 1) { const uint32_t clear = 1; WriteBytes(component + kOcclusionResult, &clear, 4); }
                        if (InterlockedIncrement(&occlusionChanged) <= 32) {
                            uint32_t guid[4]{};
                            Read(entity + 0x24, guid, 16);
                            gate::Event("coop_npcs ghost occlusion entity=%08X guid=%08X-%08X component=%08X flags %04X -> %04X result=%u",
                                        entity, guid[0], guid[1], component, flags, wanted, result);
                        }
                    }
                }
            }
        }
        if (next == node) break;
        node = next;
    }
}
void InstallPairDefenderFix(FILE* log) {
    // mov eax,[esp+4] / sub esp,10h / push esi / mov esi,ecx / test byte [esi+10h],10h / je
    static const unsigned char kAvailable[] = {0x8B, 0x44, 0x24, 0x04, 0x83, 0xEC, 0x10, 0x56, 0x8B, 0xF1, 0xF6, 0x46, 0x10, 0x10, 0x74, 0x13};
    const bool d = pairDefenderFix && hook_site::Install(log, "coop_npcs PairedAttackCoordinator::OnReflectableAvailable", base + 0xCE2A60, kAvailable,
                                                         sizeof(kAvailable), reinterpret_cast<void*>(OnPairAvailable), pairAvailableHook);
    std::fprintf(log, "coop_npcs pair_defender_wait_fix=%d (a partner's grab copy that waits for its defender resolves when he arrives)\n", d ? 1 : 0);
}
bool Install(FILE* log) {
    InstallPairDefenderFix(log);   // whoever owns the enemies: the grab copies are every machine's
    if (!enabled) return true;
    if (AnyLocalClass()) InstallPairGuard(log);
    struct Site { uint32_t rva; const char* bytes; unsigned size; };
    const Site sites[] = {
        {0xDB20, "\x8A\x44\x24\x04\x53\x55\x56\x57\x8B\xF9\x84\xC0\x0F\x84", 14},
        {0x3CB6D0, "\x56\x57\x8B\xF9\x8B\x47\x08\x3B\x44\x24\x0C\x75\x07", 13},
        {0xBDB2B0, "\xA1\x2C\x12\xF6\x01\xC3", 6},
        {0x3D1D10, "\x56\x57\x8B\x7C\x24\x0C\x57\x8B\xF1\xE8\x22\xF3\x00\x00\x89\x7E", 16},
        {0x3D1DC0, "\x83\xEC\x0C\x53\x56\x8B\xF1\x33\xDB\x39\x9E\x18\x01\x00\x00\x75", 16},
        {0x3E10E0, "\x8B\x44\x24\x04\x56\x8B\xF1\x85\xC0\x75\x21\x6A\x54\xE8\xBE\x2B", 16},
        {0x1D6C0, "\xB8\xD0\xCF\x0A\x02\xC3", 6},   // PairedAttackCoordinator::getClassRep
    };
    for (const auto& site : sites) {
        unsigned char actual[16]{};
        if (!Read(base + site.rva, actual, site.size) || std::memcmp(actual, site.bytes, site.size)) {
            std::fprintf(log, "coop_npcs signature_mismatch va=%08X; disabled\n", static_cast<unsigned>(base + site.rva));
            return false;
        }
    }
    // For every role: the main menu lobby can make this machine a client after
    // Install, and on the host OnShouldSpawn passes everything through.
    spawnHook = safetyhook::create_inline(reinterpret_cast<void*>(base + 0xDB20), reinterpret_cast<void*>(OnShouldSpawn));
    std::fprintf(log, "coop_npcs role=%s client_spawn_hook=%d signatures_ok=1\n", client ? "client" : "host", static_cast<bool>(spawnHook));
    return !client || static_cast<bool>(spawnHook);
}
// A local NPC class never targets a ghost Player. 25.09., the Nicole fight:
// every machine runs its own NicoleBoss, and each one picked the partner's
// ghost Isaac as her target: grabs and paired attacks on a body that is not
// played on this machine. DeadSpaceNPC::UpdateTarget (PC 0x00FFF5D0, only on
// an authoritative NPC) walks the awareness candidates and accepts the first
// its virtual FilterTarget (vtable +0x300, thiscall(Entity*), ret 4) passes;
// NicoleBoss inherits the base one (0x00FD9B30, mov al,1 / ret 4, shared by
// seventeen slots, so the slot is swapped, not the code). The target itself is
// the SafePtr at npc+0x6F8. Only local classes: the host's ghosted enemies keep
// hunting the client's Isaac the way they must.
constexpr uint32_t kFilterTargetSlot = 0x300, kBaseFilterTargetRva = 0xBD9B30;
// Local neutrals too (NeutralNPCsLocal=1). 25.09., Tiedemann: both machines
// fired 5225A301, then only the client's world went on into the javelin part
// and the host's stood 14 s with its leader unable to aim or fire. Tiedemann is
// a local NeutralNPC, and NeutralNPC::FilterTarget (0x012D9300, own override:
// refuses Players only while +0x871 bit 0x10 says "ignore players") accepted
// the partner's ghost: the host's Tiedemann went for the client's ghost, the
// pair guard stopped that grab, and the host's chain never reached its next
// step. With the ghost refused each Tiedemann takes his own Isaac.
constexpr uint32_t kNeutralFilterTargetRva = 0xED9300;
struct FilterSlot { const char* name; uint32_t vtableRva, expectedRva, original; };
FilterSlot filterSlots[4]{};
unsigned filterSlotCount = 0;
bool LocalNeutral(uint32_t vtable) { return neutralsLocal && vtable == base + kNeutralVtableRva; }
volatile LONG targetFilterCalls = 0, targetGhostRejected = 0, targetAccepted = 0, nicoleBotRefused = 0;
// Nicole never takes a companion bot as her target ([Bot] NicoleIgnoresBots).
// Her only attack is the
// paired kill, and CanNPCPairAttackPlayer (0x00FEEE80) refuses a bot (NO_PAIRED,
// +0x3DC bit 30): with a bot as her target she walked up to it and stood there,
// and never grabbed the human (27.09.). Her FilterTarget slot is swapped for this
// even when she is not a local class; NearestTarget asks the same slot.
bool botFilter = false;
bool __fastcall OnLocalFilterTarget(uint32_t npc, void*, uint32_t candidate) {
    InterlockedIncrement(&targetFilterCalls);
    uint32_t vtable = 0;
    Read(npc, &vtable, 4);
    uint32_t original = base + kBaseFilterTargetRva;
    for (unsigned i = 0; i < filterSlotCount; ++i)
        if (vtable == base + filterSlots[i].vtableRva && filterSlots[i].original) original = filterSlots[i].original;
    if (botFilter && candidate && vtable == base + localClasses[0].vtableRva && IsBotEntity(candidate)) {
        if (InterlockedIncrement(&nicoleBotRefused) <= 5) gate::Event("coop_npcs Nicole %08X would have taken the bot %08X as her target: refused", npc, candidate);
        return false;
    }
    if (candidate && (IsGhostPlayer(candidate) || IsGhostNecro(candidate))) {
        if (InterlockedIncrement(&targetGhostRejected) <= 20)
            gate::Event("coop_npcs local NPC %08X (vtable %08X) would have targeted the ghost %08X: refused", npc, vtable, candidate);
        return false;
    }
    InterlockedIncrement(&targetAccepted);
    return reinterpret_cast<bool(__thiscall*)(uint32_t, uint32_t)>(original)(npc, candidate);
}
void InstallLocalTargetFilter(FILE* log) {
    filterSlotCount = 0;
    for (const auto& c : localClasses)
        if (c.on && filterSlotCount < std::size(filterSlots)) filterSlots[filterSlotCount++] = FilterSlot{c.name, c.vtableRva, kBaseFilterTargetRva, 0};
    if (neutralsLocal && filterSlotCount < std::size(filterSlots))
        filterSlots[filterSlotCount++] = FilterSlot{"NeutralNPC", kNeutralVtableRva, kNeutralFilterTargetRva, 0};
    if (botFilter && !localClasses[0].on && filterSlotCount < std::size(filterSlots))
        filterSlots[filterSlotCount++] = FilterSlot{localClasses[0].name, localClasses[0].vtableRva, kBaseFilterTargetRva, 0};
    for (unsigned i = 0; i < filterSlotCount; ++i) {
        FilterSlot& c = filterSlots[i];
        const uint32_t slot = base + c.vtableRva + kFilterTargetSlot;
        uint32_t current = 0;
        if (!Read(slot, &current, 4) || !current) {
            std::fprintf(log, "coop_npcs %s target filter: slot %08X unreadable, not installed\n", c.name, slot);
            continue;
        }
        if (current != base + c.expectedRva) {
            std::fprintf(log, "coop_npcs %s target filter: slot %08X holds %08X, expected %08X; left alone\n", c.name, slot, current,
                         base + c.expectedRva);
            continue;
        }
        const uint32_t ours = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&OnLocalFilterTarget));
        DWORD previous = 0;
        void* at = reinterpret_cast<void*>(slot);
        if (!VirtualProtect(at, 4, PAGE_READWRITE, &previous)) {
            std::fprintf(log, "coop_npcs %s target filter: VirtualProtect failed at %08X\n", c.name, slot);
            continue;
        }
        c.original = current;
        std::memcpy(at, &ours, 4);
        VirtualProtect(at, 4, previous, &previous);
        std::fprintf(log, "coop_npcs %s target filter installed: vtable slot %08X %08X -> %08X (ghost Players are never its target)\n",
                     c.name, slot, current, ours);
    }
}
// The nearest player, not the first ([Coop] NearestTarget). 25.09., the
// Ubermorph only ever went for the host. DeadSpaceNPC::UpdateTarget (PC
// 0x00FFF5D0, Xbox 0x84496C90) takes AwarenessComponent::GetInterestedTarget
// (0x00F60D90, thiscall(aware, visible) ret 8; the NPC's component at +0x71C),
// the first tracked entity it is aware of (array at component+0x30, 0x70 a
// piece, aware bit at +0x48, SafePtr at +0x30, count at +0x720), and keeps
// it while FilterTarget (vtable +0x300) agrees. In a one-player game the
// first is the only one; with a partner it is whoever was noticed first,
// normally the host. So right after that call (0x00FFF608: eax = the first,
// edi = the NPC, ebx = the component, ebp = the current target) the nearest
// tracked, aware entity its own FilterTarget accepts is handed on instead. The
// current target stays while it is not clearly farther (x1.3 + 2 m), so an
// enemy does not flip between two players standing side by side.
bool nearestTarget = false;
safetyhook::MidHook nearestHook;
volatile LONG targetPicks = 0, targetSwitched = 0, targetKept = 0, targetFaults = 0, botCandidates = 0, watcherTargetsRefused = 0;
bool watcherTargets = false;   // installed for [Necro] Invasion: a necro watching a scene as Isaac is nobody's target
bool WatcherBody(uint32_t entity, DWORD now) {
    return entity && (IsGhostPlayer(entity) || IsGhostNecro(entity)) &&
           (roles::WatchingMachine(MachineOf(entity), now) || roles::SpectatingMachine(MachineOf(entity), now));
}
constexpr uint32_t kAfterInterestedRva = 0xBFF608, kAwareCount = 0x720, kAwareFlags = 0x724, kAwareEntries = 0x30, kAwareStride = 0x70;
bool Accepts(uint32_t npc, uint32_t target) {
    uint32_t vtable = 0, fn = 0;
    if (!Read(npc, &vtable, 4) || !Read(vtable + kFilterTargetSlot, &fn, 4) || !fn) return false;
    __try {
        return reinterpret_cast<bool(__thiscall*)(void*, uint32_t)>(fn)(reinterpret_cast<void*>(npc), target);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&targetFaults);
        return false;
    }
}
void OnFirstTarget(SafetyHookContext& ctx) {
    const auto first = static_cast<uint32_t>(ctx.eax), npc = static_cast<uint32_t>(ctx.edi), comp = static_cast<uint32_t>(ctx.ebx),
               current = static_cast<uint32_t>(ctx.ebp);
    if (!first || !npc || !comp) return;
    const DWORD now = GetTickCount();
    const bool firstWatcher = watcherTargets && peerTable.Count() && WatcherBody(first, now);
    // With partners, or with the companion bot: both are players an enemy should weigh by distance.
    if (!firstWatcher && (!nearestTarget || (!peerTable.Count() && !AnyBot()))) return;
    uint32_t count = 0;
    unsigned char flags = 0;
    float at[3]{};
    if (!Read(comp + kAwareFlags, &flags, 1) || !(flags & 2) || !Read(comp + kAwareCount, &count, 4) || !count || count > 32 ||
        !Read(npc + 0xC0, at, 12)) return;
    InterlockedIncrement(&targetPicks);
    uint32_t best = 0;
    float bestD = 0.0f, currentD = -1.0f;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t entry = comp + kAwareEntries + i * kAwareStride;
        unsigned char aware = 0;
        uint32_t ptr = 0;
        if (!Read(entry + 0x48, &aware, 1) || !(aware & 1) || !Read(entry + 0x30, &ptr, 4) || !ptr) continue;
        const uint32_t entity = ptr - 0x10;
        if (IsBotEntity(entity)) InterlockedIncrement(&botCandidates);
        if (watcherTargets && WatcherBody(entity, now)) continue;
        float pos[3]{};
        if (!Read(entity + 0xC0, pos, 12) || !Accepts(npc, entity)) continue;
        const float dx = pos[0] - at[0], dy = pos[1] - at[1], dz = pos[2] - at[2];
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (entity == current) currentD = d;
        if (!best || d < bestD) { best = entity; bestD = d; }
    }
    if (!best) {
        if (firstWatcher) { ctx.eax = 0; InterlockedIncrement(&watcherTargetsRefused); }
        return;
    }
    uint32_t pick = best;
    if (nearestTarget && currentD >= 0.0f && currentD <= bestD * 1.3f + 2.0f) { pick = current; InterlockedIncrement(&targetKept); }
    if (pick == first) return;
    if (firstWatcher) InterlockedIncrement(&watcherTargetsRefused);
    ctx.eax = pick;
    if (InterlockedIncrement(&targetSwitched) <= 40) {
        uint32_t vtable = 0;
        Read(npc, &vtable, 4);
        gate::Event("coop_npcs NPC %08X (vtable %08X) takes the nearest player %08X (%.1f m) instead of the first it noticed %08X", npc, vtable,
                    pick, pick == best ? bestD : currentD, first);
    }
}
void InstallNearestTarget(FILE* log, const std::string& ini) {
    nearestTarget = GetPrivateProfileIntA("Coop", "NearestTarget", 0, ini.c_str()) != 0;
    watcherTargets = GetPrivateProfileIntA("Necro", "Invasion", 1, ini.c_str()) != 0 && GetPrivateProfileIntA("Necro", "WatchScenes", 1, ini.c_str()) != 0;
    if (nearestTarget || watcherTargets) {
        // mov esi,eax / test esi,esi / je +25h
        static const unsigned char kSite[] = {0x8B, 0xF0, 0x85, 0xF6, 0x74, 0x25};
        unsigned char actual[sizeof(kSite)]{};
        if (!Read(base + kAfterInterestedRva, actual, sizeof(actual)) || std::memcmp(actual, kSite, sizeof(kSite)) != 0) {
            std::fprintf(log, "coop_npcs nearest_target: signature mismatch at %08X, not installed\n", static_cast<unsigned>(base + kAfterInterestedRva));
            nearestTarget = watcherTargets = false;
        } else {
            nearestHook = safetyhook::create_mid(reinterpret_cast<void*>(base + kAfterInterestedRva), OnFirstTarget);
            if (!nearestHook) nearestTarget = watcherTargets = false;
        }
    }
    std::fprintf(log, "coop_npcs nearest_target=%d watcher_targets=%d (an enemy goes for the nearest player it noticed, not the first; a necro watching a scene as Isaac is nobody's target)\n",
                 nearestTarget ? 1 : 0, watcherTargets ? 1 : 0);
}
void WriteTargetFilterDiagnostics(FILE* log) {
    static LONG lastPicks = -1;
    const LONG pickState = targetSwitched + targetPicks / 100 + botCandidates / 100 + watcherTargetsRefused;
    if ((nearestTarget || watcherTargets) && pickState != lastPicks) {
        lastPicks = pickState;
        std::fprintf(log, "coop_npcs nearest_target picks=%ld switched=%ld kept=%ld faults=%ld watcher_refused=%ld bot_candidates=%ld\n", targetPicks,
                     targetSwitched, targetKept, targetFaults, watcherTargetsRefused, botCandidates);
    }
    static LONG last = -1;
    const LONG calls = InterlockedCompareExchange(&targetFilterCalls, 0, 0);
    if ((!AnyLocalClass() && !neutralsLocal) || calls / 200 == last) return;
    last = calls / 200;
    std::fprintf(log, "coop_npcs local_target_filter calls=%ld ghost_refused=%ld accepted=%ld\n", calls,
                 InterlockedCompareExchange(&targetGhostRejected, 0, 0), InterlockedCompareExchange(&targetAccepted, 0, 0));
}
} // namespace npcs
