// Dead Space 2 United by Richard Hafer.
// Ghosts pull triggers; this is where they let go.
//
// TriggerBox::HandleEvents (PC 0x00EBAE30) walks the entities inside its
// volume (+0x1C0) every check and, for each one without a record, sends
// itself iMsgTrigger (0xB9D18800) with that entity's handle as the source:
//     SendMessageTo(&iMsgTrigger, this, force=0)      0x00756D90 -> 0x00756A00
// SendMessageTo is a DIRECT delivery to one target's HandleEvents, not the
// broadcast dispatch at 0x00756C60, which is why the gate never saw a door
// trigger, and why a partner's ghost standing in another machine's world
// opens its doors, loads its rooms and unloads the one its player is in
// (host, 15:50: part of the room went away under him when the client walked on).
//
// So the direct send is wrapped, and a trigger message whose source is a
// ghost Player is dropped. Each machine's triggers answer to its own Isaac
// only, as the campaign always assumed. The gate then means what it says.
namespace ghosttrigger {
bool enabled = false, installed = false;
// [Bot] IgnoreTriggers: the companion bots (local Players, not ghosts) pull no
// trigger either, strictly: no TriggerBox message with a bot as its source and
// no touch of a TriggerPhysical by a bot. So no door, no stream set, no cull
// set, no checkpoint, no ambush and no scene starts because a bot walked in.
bool botsIgnore = false;
volatile LONG botTriggers = 0, botTouches = 0;
// [Bot] BlockLevelChange: whatever IgnoreTriggers says, a bot never starts a
// level change. 06.10.: the call key (F7) put the bots into a trigger behind
// the leader whose LevelChange (0x0040F870 via LevelChange::Activate) reloaded
// the level twice; the log showed "called over" and "the level is changing"
// in the same second, with the leader outside the trigger.
bool blockBotLevelChange = false;
volatile LONG botLevelChangesDropped = 0;
constexpr uint32_t kLevelChangeVtableRva = 0x17588C4;
constexpr uint32_t kSendToRva = 0x356A00;
constexpr uint32_t kTrigger = 0xB9D18800u, kReverseTrigger = 0xB469F22Eu;
safetyhook::InlineHook sendHook;   // the mirror replays direct sends through this trampoline
volatile LONG seen = 0, dropped = 0, notPlayer = 0;

uint32_t SourceEntity(uint32_t handle) {
    uint32_t table = 0, object = 0, id = 0;
    if (!handle || !Read(base + gate::kHandleTableRva, &table, 4) || !table) return 0;
    const uint32_t entry = table + (handle >> 23) * 8;
    if (!Read(entry, &object, 4) || !Read(entry + 4, &id, 4) || id != handle) return 0;
    return object;
}

// Elevator buttons (24.09.): the button inside the cabin sends its ride
// (contr_elevator_floor_N) straight to the MoverController, a direct send
// the gate never sees, so the lift left with whoever pressed. Held here until
// every player is in the cabin (PartnersInLift), then sent and offered to the
// mirror like any other direct send.
bool liftWait = false;   // [Coop] LiftWait
DWORD liftWaitMs = 60000;  // [Coop] LiftWaitMs: then the ride goes anyway, like every other hold
struct LiftHold { bool used; unsigned char raw[12]; uint32_t target, vtable, hash; int force; DWORD since; bool noticed; };
LiftHold liftHold{};
volatile LONG liftHeld = 0, liftReleased = 0, liftDropped = 0, liftTimedOut = 0;
bool IsMoverController(uint32_t target, uint32_t& vtable) {
    return Read(target, &vtable, 4) && vtable >= base + gate::kRdataLo && vtable < base + gate::kRdataHi && gate::IsLiftController(target, vtable);
}
// The cabin, not a distance. 25.09.: the client rode, the host stood 2.4 m
// from the cabin's centre behind the lower door, inside the old 5 m reach, so
// nothing was held, and both lifts left without him. A player is in a cabin
// when he stands inside the Mover's box: Mover::GetAABBMinsMaxsRelative (PC
// vtable slot 16, 0x00DC21D0, thiscall(mins[4], maxs[4]), ret 8; Xbox slot 17)
// around its position, a little wider at the sides, from just below the floor.
constexpr uint32_t kMoverVtableRva = 0x174CDC0, kMoverAabbSlot = 16, kSimManagerRva = 0x1C1C0A0;
// The box is in the Mover's own frame (0x00F65550 multiplies by the inverse of
// its transform), so a player is turned into that frame first: rows +0x90,
// +0xA0, +0xB0. 12.10.: the recreation lift stands turned 180 degrees, its box
// was laid out mirrored, nobody was ever inside, and the host rode alone.
constexpr uint32_t kMoverAxes = 0x90;
struct Cabin { uint32_t mover; float at[3], mins[4], maxs[4], axes[3][4]; };
volatile LONG cabinFaults = 0, cabinLogs = 0;
bool CabinBox(uint32_t mover, Cabin& c) {
    uint32_t vtable = 0, fn = 0;
    if (!Read(mover, &vtable, 4) || vtable != base + kMoverVtableRva || !Read(vtable + kMoverAabbSlot * 4, &fn, 4) ||
        !Read(mover + placement::kPlayerPosition, c.at, 12) || !Read(mover + kMoverAxes, c.axes, sizeof(c.axes))) return false;
    alignas(16) float mins[4]{}, maxs[4]{};
    __try {
        reinterpret_cast<void(__thiscall*)(uint32_t, float*, float*)>(fn)(mover, mins, maxs);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&cabinFaults);
        return false;
    }
    c.mover = mover;
    std::memcpy(c.mins, mins, sizeof(mins));
    std::memcpy(c.maxs, maxs, sizeof(maxs));
    return maxs[0] > mins[0] && maxs[1] > mins[1] && maxs[2] > mins[2];
}
bool InCabin(const Cabin& c, const float* p) {
    const float d[3] = {p[0] - c.at[0], p[1] - c.at[1], p[2] - c.at[2]};
    float l[3];
    for (unsigned i = 0; i < 3; ++i) l[i] = d[0] * c.axes[i][0] + d[1] * c.axes[i][1] + d[2] * c.axes[i][2];
    const float side = 0.3f;
    return l[0] >= c.mins[0] - side && l[0] <= c.maxs[0] + side &&
           l[2] >= c.mins[2] - side && l[2] <= c.maxs[2] + side &&
           l[1] >= c.mins[1] - 0.6f && l[1] <= c.maxs[1] + 0.3f;
}
// Only switches attached to the moving cabin are gated here. Landing calls
// must remain usable while the cabin is elsewhere.
bool RefuseCabinButton(uint32_t player, uint32_t usable) {
    if (!liftWait || !inLevel || !network || !peerTable.Count() || player != LocalSlotZeroEntity() || usable < 0x230) return false;
    uint32_t vt = 0, ownerVt = 0;
    const uint32_t owner = usable - 0x230;
    if (!Read(usable, &vt, 4) || vt != base + 0x175C390 ||
        !Read(owner, &ownerVt, 4) || ownerVt != base + 0x175C508) return false;
    uint32_t guid[4]{};
    if (!Read(owner + 0x24, guid, sizeof(guid))) return false;
    for (const auto& entry : coop_rides::kCabinButtons) {
        if (std::memcmp(entry.button, guid, sizeof(guid))) continue;
        const DWORD now = GetTickCount();
        Cabin cabin{};
        const uint32_t mover = mirror::ObjectByGuid(entry.mover);
        if (!mover || !CabinBox(mover, cabin)) {
            static volatile LONG unresolved = 0;
            if (InterlockedIncrement(&unresolved) <= 8)
                gate::Event("coop_lift button %08X-%08X has no live cabin; deferring to ride gate", guid[0], guid[1]);
            return false;
        }
        float pos[3]{};
        bool ready = Read(player + placement::kPlayerPosition, pos, sizeof(pos)) && InCabin(cabin, pos);
        for (unsigned i = 0; ready && i < coop_peers::kMaxPeers; ++i) {
            if (!peerTable.peers[i].used || !peerTable.peers[i].token) continue;
            const auto& remote = peerRuntime[i];
            ready = remote.lastState && now - remote.lastState <= 5000 && InCabin(cabin, remote.pos);
        }
        if (!ready) {
            static DWORD lastNotice = 0;
            if (!lastNotice || now - lastNotice >= 2000) {
                lastNotice = now;
                notice::Show("WAITING FOR PARTNER - LIFT");
                gate::Event("coop_lift cabin button %08X-%08X refused before use (mover %08X)", guid[0], guid[1], mover);
            }
        }
        return !ready;
    }
    return false;
}

// The Movers in buckets [from, to) of the SimManager's GUID table, for a walk
// spread over several frames (the bots'; the whole table is every entity of the level).
unsigned MoversIn(uint32_t from, uint32_t to, uint32_t* out, unsigned max) {
    uint32_t sim = 0;
    unsigned n = 0;
    if (!Read(base + kSimManagerRva, &sim, 4) || !sim) return 0;
    for (uint32_t b = from; b < to && b < 4096 && n < max; ++b) {
        uint32_t node = 0;
        if (!Read(sim + 0x70 + b * 4, &node, 4)) break;
        for (unsigned guard = 0; node && guard < 64 && n < max; ++guard) {
            uint32_t object = 0, vtable = 0;
            if (Read(node + 0x40, &object, 4) && object && Read(object, &vtable, 4) && vtable == base + kMoverVtableRva) out[n++] = object;
            if (!Read(node + 0x34, &node, 4)) break;
        }
    }
    return n;
}
// Every Mover in the SimManager's GUID table (buckets +0x70, next +0x34, object +0x40).
unsigned Cabins(Cabin* out, unsigned max) {
    uint32_t sim = 0;
    unsigned n = 0;
    if (!Read(base + kSimManagerRva, &sim, 4) || !sim) return 0;
    for (uint32_t b = 0; b < 4096 && n < max; ++b) {
        uint32_t node = 0;
        if (!Read(sim + 0x70 + b * 4, &node, 4)) break;
        for (unsigned guard = 0; node && guard < 64 && n < max; ++guard) {
            uint32_t object = 0, vtable = 0;
            if (Read(node + 0x40, &object, 4) && object && Read(object, &vtable, 4) && vtable == base + kMoverVtableRva && CabinBox(object, out[n])) ++n;
            if (!Read(node + 0x34, &node, 4)) break;
        }
    }
    return n;
}
// False while a cabin holds a player (the local one or a partner's) and some other
// player is outside it: nobody rides a lift away from his partner.
bool PartnersInLift(DWORD now, char* who, size_t whoSize) {
    const uint32_t body = LocalSlotZeroEntity();
    float pos[3]{};
    if (!body || !Read(body + placement::kPlayerPosition, pos, 12)) return true;
    struct Where { float pos[3]; unsigned slot; bool local; };
    Where players[coop_peers::kMaxPeers + 1 + kMaxBots]{};
    unsigned count = 0, missing = 0;
    players[count++] = Where{{pos[0], pos[1], pos[2]}, 0, true};
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s) {
        const auto& peer = peerTable.peers[s];
        if (!peer.used || !peer.token) continue;
        const auto& runtime = peerRuntime[s];
        if (!runtime.lastState || now - runtime.lastState > 5000) { ++missing; continue; }
        players[count++] = Where{{runtime.pos[0], runtime.pos[1], runtime.pos[2]}, s, false};
    }
    for (unsigned b = 0; b < kMaxBots; ++b) {
        const uint32_t bot = botEntities[b];
        float botPos[3]{};
        if (bot && !botHolding[b] && count < std::size(players) && Read(bot + placement::kPlayerPosition, botPos, 12))
            players[count++] = Where{{botPos[0], botPos[1], botPos[2]}, 0xFFu, false};
    }
    if (count + missing < 2) { std::snprintf(who, whoSize, "nobody else to wait for"); return true; }
    Cabin cabins[16]{};
    const unsigned n = Cabins(cabins, 16);
    std::snprintf(who, whoSize, "nobody in any of %u cabins", n);
    for (unsigned c = 0; c < n; ++c) {
        unsigned inside = 0;
        for (unsigned i = 0; i < count; ++i) inside += InCabin(cabins[c], players[i].pos) ? 1 : 0;
        if (inside && missing) { std::snprintf(who, whoSize, "waiting for %u partner position(s)", missing); return false; }
        if (inside == count) std::snprintf(who, whoSize, "all %u players in cabin %08X", count, cabins[c].mover);
        if (!inside || inside == count) continue;
        if (InterlockedIncrement(&cabinLogs) <= 20)
            gate::Event("coop_lift cabin %08X at (%.1f,%.1f,%.1f) box (%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f): %u of %u players inside", cabins[c].mover,
                        cabins[c].at[0], cabins[c].at[1], cabins[c].at[2], cabins[c].mins[0], cabins[c].mins[1], cabins[c].mins[2], cabins[c].maxs[0],
                        cabins[c].maxs[1], cabins[c].maxs[2], inside, count);
        for (unsigned i = 0; i < count; ++i) {
            if (InCabin(cabins[c], players[i].pos)) continue;
            const float dx = players[i].pos[0] - cabins[c].at[0], dz = players[i].pos[2] - cabins[c].at[2];
            if (players[i].local) std::snprintf(who, whoSize, "our Isaac is outside the cabin (%.1f m)", std::sqrt(dx * dx + dz * dz));
            else if (players[i].slot == 0xFFu) std::snprintf(who, whoSize, "the bot is outside the cabin (%.1f m)", std::sqrt(dx * dx + dz * dz));
            else std::snprintf(who, whoSize, "partner %u is outside the cabin (%.1f m)", players[i].slot, std::sqrt(dx * dx + dz * dz));
            return false;
        }
    }
    return true;
}
volatile LONG levelChangePending = 0;   // sampled here for CheckLevelChangeAfterBotTouch on the diagnostics thread
volatile LONG liftFaults = 0;
// The held ride goes out seconds later: its controller may have been deleted in between
// (the vtable check passes on memory a deferred delete has not reused yet).
int SendHeldRide(void* raw, uint32_t target, int force) {
    __try {
        return sendHook.ccall<int>(raw, reinterpret_cast<void*>(static_cast<uintptr_t>(target)), force);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedIncrement(&liftFaults) <= 5) gate::Event("coop_lift the held ride to %08X faulted: dropped", target);
        return 0;
    }
}
void LiftTick(DWORD now) {
    if (blockBotLevelChange) InterlockedExchange(&levelChangePending, LevelChangePending() ? 1 : 0);
    if (!liftHold.used) return;
    if (!inLevel) { liftHold = LiftHold{}; InterlockedIncrement(&liftDropped); return; }
    char who[64] = "";
    const DWORD held = TicksSince(now, liftHold.since);
    const bool late = liftWaitMs && held >= liftWaitMs;
    if (!late && !PartnersInLift(now, who, sizeof(who))) {
        if (!liftHold.noticed && held >= 1000) { liftHold.noticed = true; notice::Show("WAITING FOR PARTNER - LIFT"); }
        return;
    }
    if (late) InterlockedIncrement(&liftTimedOut);
    const LiftHold h = liftHold;
    liftHold = LiftHold{};
    uint32_t vtable = 0;
    if (!Read(h.target, &vtable, 4) || vtable != h.vtable) { InterlockedIncrement(&liftDropped); return; }
    InterlockedIncrement(&liftReleased);
    gate::Event("coop_lift ride %08X released after %u ms: %s", h.hash, static_cast<unsigned>(held),
                late ? "TIMEOUT, going without the partner" : "everybody is in the cabin");
    if (h.noticed || late) notice::Show(late ? "CONTINUING WITHOUT PARTNER" : "PARTNER HERE - CONTINUING");
    unsigned char raw[12];
    std::memcpy(raw, h.raw, sizeof(raw));
    mirror::OnLocalDirect(h.hash, raw, h.target);
    SendHeldRide(raw, h.target, h.force);
}
int __cdecl OnSendTo(void* message, void* target, int force) {
    uint32_t hash = 0, source = 0;
    if (message && target && Read(reinterpret_cast<uintptr_t>(message), &hash, 4) && hash) {
        uint32_t vtable = 0;
        Read(reinterpret_cast<uintptr_t>(target), &vtable, 4);
        gate::NoteRecentMessage(hash, vtable, true);
        gate::AudioTrace(hash, gate::replaying > 0 || gate::mirroring > 0 ? "direct-replayed" : "direct", vtable);
    }
    if (blockBotLevelChange && message && target && Read(reinterpret_cast<uintptr_t>(message), &hash, 4) && (hash == kTrigger || hash == kReverseTrigger)) {
        uint32_t botSource = 0, vtable = 0;
        Read(reinterpret_cast<uintptr_t>(message) + 8, &botSource, 4);
        Read(reinterpret_cast<uintptr_t>(target), &vtable, 4);
        const uint32_t entity = SourceEntity(botSource);
        if (vtable == base + kLevelChangeVtableRva && entity && IsBotEntity(entity)) {
            if (InterlockedIncrement(&botLevelChangesDropped) <= 20)
                gate::Event("coop_ghosttrigger a bot's trigger %s to the level change %p dropped (bot %08X)", hash == kTrigger ? "entry" : "exit", target, entity);
            return 0;
        }
    }
    if (botsIgnore && message && Read(reinterpret_cast<uintptr_t>(message), &hash, 4) && (hash == kTrigger || hash == kReverseTrigger)) {
        uint32_t botSource = 0;
        Read(reinterpret_cast<uintptr_t>(message) + 8, &botSource, 4);
        const uint32_t entity = SourceEntity(botSource);
        if (entity && IsBotEntity(entity)) {
            if (InterlockedIncrement(&botTriggers) <= 20) {
                uint32_t vtable = 0;
                Read(reinterpret_cast<uintptr_t>(target), &vtable, 4);
                gate::Event("coop_ghosttrigger a bot's %s of trigger %p (vtable RVA %08X) dropped", hash == kTrigger ? "entry" : "exit", target,
                            vtable ? vtable - static_cast<uint32_t>(base) : 0u);
            }
            return 0;
        }
    }
    if (enabled && message && Read(reinterpret_cast<uintptr_t>(message), &hash, 4)) {
        if (hash == kTrigger || hash == kReverseTrigger) {
            InterlockedIncrement(&seen);
            Read(reinterpret_cast<uintptr_t>(message) + 8, &source, 4);
            const uint32_t entity = SourceEntity(source);
            if (entity && (IsGhostPlayer(entity) || IsGhostNecro(entity))) { InterlockedIncrement(&dropped); return 0; }   // a partner's necro pulls no local trigger either
            if (!entity) InterlockedIncrement(&notPlayer);
        } else if (gate::mirroring == 0 && gate::replaying == 0 && target) {
            gate::NoteUse(message);
            uint32_t targetVtable = 0;
            const uint32_t targetObject = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(target));
            const DWORD now = GetTickCount();
            char who[64] = "";
            if (liftWait && peerTable.Count() && !coop_internal::IsInternal(hash) && IsMoverController(targetObject, targetVtable) &&
                !PartnersInLift(now, who, sizeof(who))) {
                // The newest press wins; an older held ride of another lift is dropped.
                if (liftHold.used) InterlockedIncrement(&liftDropped);
                liftHold = LiftHold{};
                liftHold.used = true;
                Read(reinterpret_cast<uintptr_t>(message), liftHold.raw, sizeof(liftHold.raw));
                liftHold.target = targetObject; liftHold.vtable = targetVtable; liftHold.hash = hash;
                liftHold.force = force; liftHold.since = now;
                InterlockedIncrement(&liftHeld);
                gate::Event("coop_lift ride %08X held: %s", hash, who);
                return 1;
            }
            // Everything else delivered straight to a world object is offered
            // to the mirror (host side decides by the target's class).
            mirror::OnLocalDirect(hash, message, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(target)));
        }
    }
    return sendHook.ccall<int>(message, target, force);
}

// Who touched a touch trigger last. 25.09., ch11 marker test bed, twice: the
// player who stayed behind saw part of the room he stood in go black. The
// room's parts are streams (mtb_floor_zone01-03, ...) that cull-set messages
// (disable_cull_set_1_mtb_floor_zone01, heard by a StreamScripter) hide and
// show; the triggers that send them are touch triggers, and touch triggers
// queue their message through the scheduler (TriggerPhysical touch 0x015F15E0,
// thiscall(entity), ret 4; SendMessageTo never sees it), so the partner's
// ghost walking on fired the rear player's triggers. The touch is noted here
// per trigger; the gate drops a message heard only by StreamScripters whose
// trigger only a ghost touched (StreamSets the engine guards itself).
constexpr uint32_t kTouchRva = 0x11F15E0;
safetyhook::InlineHook touchHook;
bool cullGuard = false;
struct Touch { uint32_t trigger; DWORD ghostAt, localAt, botAt; uint32_t sceneGhost; DWORD mixedGhostsAt; };
struct Entry { uint32_t trigger, entity; DWORD at; };
Entry lastGhostEntry{}, lastLocalEntry{};
constexpr DWORD kSceneTouchMs = 1500;
Touch touches[64]{};
unsigned touchNext = 0;
volatile LONG ghostTouches = 0;
Touch* TouchOf(uint32_t trigger, bool create) {
    for (auto& t : touches) if (t.trigger == trigger) return &t;
    if (!create) return nullptr;
    Touch& t = touches[touchNext++ % std::size(touches)];
    t = Touch{};
    t.trigger = trigger;
    return &t;
}
void __fastcall OnTouch(void* trigger, void*, uint32_t entity) {
    if (entity && IsBotEntity(entity)) {
        if (botsIgnore) { InterlockedIncrement(&botTouches); return; }   // the trigger never learns a bot was there
        if (blockBotLevelChange) TouchOf(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(trigger)), true)->botAt = GetTickCount();
    } else if (blockBotLevelChange && entity && entity == LocalSlotZeroEntity()) {
        TouchOf(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(trigger)), true)->localAt = GetTickCount();
    }
    if (entity && peerTable.Count()) {
        const bool ghost = IsGhostPlayer(entity) || IsGhostNecro(entity);
        if (ghost || entity == LocalSlotZeroEntity()) {
            Touch* t = TouchOf(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(trigger)), true);
            const DWORD now = Stamp(GetTickCount());
            if (ghost) {
                if (t->sceneGhost != entity && t->ghostAt && TicksSince(now, t->ghostAt) <= kSceneTouchMs) t->mixedGhostsAt = now;
                t->sceneGhost = entity;
                t->ghostAt = now;
                InterlockedIncrement(&ghostTouches);
            } else t->localAt = now;
        }
    }
    touchHook.thiscall<void>(trigger, entity);
}
bool GhostOnlyTouch(uint32_t trigger, DWORD now) {
    if (!cullGuard) return false;
    const Touch* t = TouchOf(trigger, false);
    return t && t->ghostAt && now - t->ghostAt < 2000 && !(t->localAt && now - t->localAt < 2000);
}
bool BotOnlyTouch(uint32_t trigger, DWORD now) {
    const Touch* t = TouchOf(trigger, false);
    return t && t->botAt && now - t->botAt < 2000 && !(t->localAt && now - t->localAt < 2000);
}

// A restart puts everybody back: a ride held from before it has no cabin to go to.
void OnRestart() {
    std::memset(touches, 0, sizeof(touches));
    lastGhostEntry = {};
    lastLocalEntry = {};
    if (liftHold.used) { liftHold = LiftHold{}; InterlockedIncrement(&liftDropped); }
}

// Doors are no TriggerBox: StandardDoor::TryToActivateDoor (PC 0x00DDA380,
// thiscall(actor), ret 4, bool in al, reached through the vtable only) runs
// for every Player in the door's field. It notes the actor's side (+0x259)
// and goes to state 1, and the tick then loads that side's open set. A door
// that is already open does the same when the actor stands on the other side.
// So a bot behind the player reopened doors toward its own side and the room
// in front of the player was unloaded (08.10.). With [Bot] IgnoreTriggers a
// bot never activates a door; a bot left behind a shut door is brought over
// by the catch-up like any bot that falls behind.
constexpr uint32_t kTryActivateRva = 0x9DA380;
safetyhook::InlineHook doorActivateHook;
volatile LONG doorByBot = 0, doorByLocal = 0, doorByOther = 0;
bool __fastcall OnTryActivateDoor(void* door, void*, uint32_t actor) {
    if (actor && IsBotEntity(actor)) {
        if (InterlockedIncrement(&doorByBot) <= 10) gate::Event("coop_ghosttrigger door %p: a bot's activation refused (bot %08X)", door, actor);
        return false;
    }
    InterlockedIncrement(actor && actor == LocalSlotZeroEntity() ? &doorByLocal : &doorByOther);
    return doorActivateHook.thiscall<bool>(door, actor);
}

// The level's trigger volumes (TriggerPhysical: 3585 of 3587 in the campaign accept any
// Player) ask DS_EntityFilterComponent::CheckEntityPassesFilter (PC 0x00EA76F0,
// thiscall(Entity*), ret 4, bool) whether a body counts: entering, "occupied" and the
// detonator all go through it. The SendMessageTo and touch hooks above never see those
// (bot trigger audit 13.10.: bots locked doors and moved checkpoints). With [Bot]
// IgnoreTriggers a bot does not count for a TriggerPhysical; never recorded, it never
// "exits" either. [Coop] GhostTriggerFilter does the same for partner ghosts (off).
constexpr uint32_t kFilterCheckRva = 0xAA76F0, kTriggerPhysicalVtableRva = 0x175D5F0, kFilterOwner = 0x10;
safetyhook::InlineHook filterHook;
bool filterHooked = false, ghostFilter = false;
volatile LONG filterCalls = 0, filterTriggerChecks = 0, filterBotRefused[kMaxBots]{}, filterGhostRefused = 0, filterGhostPassed = 0, filterLines = 0;
struct OtherOwner { uint32_t vtable; LONG count; };
OtherOwner filterOthers[8]{};
SRWLOCK filterLock = SRWLOCK_INIT;
int BotIndexOf(uint32_t entity) {
    for (unsigned b = 0; b < kMaxBots; ++b) if (botEntities[b] && botEntities[b] == entity) return static_cast<int>(b);
    return -1;
}
volatile LONG filterProxyRefused = 0, localNecroPassed = 0, localNecroRefused = 0;
// The last TriggerPhysical a partner's ghost and our own Isaac entered here. 02.11.: the host
// walked ahead, his ghost entered the door trigger on the client, the client's game started the
// QTE on its own Isaac far behind and the client led the scene the host had walked into
// (runtime_coop_mirror.inc, ghostseq).
volatile LONG ghostEntries = 0, localEntries = 0;
bool OwnerIsTrigger(void* self, uint32_t& owner) {
    uint32_t vtable = 0;
    const uint32_t comp = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    return Read(comp + kFilterOwner, &owner, 4) && owner && Read(owner, &vtable, 4) && vtable == base + kTriggerPhysicalVtableRva;
}
// A ghost entered a trigger within windowMs that our Isaac did not enter in that time.
bool GhostEnteredFirst(DWORD now, DWORD windowMs, uint32_t& ghost, uint32_t& trigger) {
    const Entry g = lastGhostEntry, l = lastLocalEntry;
    if (!g.at || TicksSince(now, g.at) > windowMs) return false;
    if (l.at && TicksSince(now, l.at) <= windowMs && l.trigger == g.trigger) return false;
    ghost = g.entity;
    trigger = g.trigger;
    return true;
}
// Prefer the message's source to another volume that happened to fire nearby.
uint32_t SceneActor(const void* message, DWORD now, uint32_t& trigger, bool& ambiguous) {
    uint32_t handle = 0;
    if (!message || !Read(reinterpret_cast<uintptr_t>(message) + 8, &handle, 4)) return 0;
    const uint32_t source = SourceEntity(handle);
    if (!source) return 0;
    const uint32_t local = LocalSlotZeroEntity();
    if (source == local || IsGhostPlayer(source)) return source;
    uint32_t vtable = 0;
    if (!Read(source, &vtable, 4) || vtable != base + kTriggerPhysicalVtableRva) return 0;
    trigger = source;
    const Touch* t = TouchOf(source, false);
    if (!t) return 0;
    const bool localTouch = t->localAt && TicksSince(now, t->localAt) <= kSceneTouchMs;
    const bool ghostTouch = t->ghostAt && TicksSince(now, t->ghostAt) <= kSceneTouchMs;
    ambiguous = ghostTouch && (localTouch || (t->mixedGhostsAt && TicksSince(now, t->mixedGhostsAt) <= kSceneTouchMs));
    if (ambiguous) return 0;
    return localTouch ? local : ghostTouch ? t->sceneGhost : 0;
}
bool __fastcall OnFilterCheck(void* self, void*, uint32_t entity) {
    InterlockedIncrement(&filterCalls);
    // A necro's ghost camera (RespawnProxy, vtable 0x01B596E0) flies free and passes trigger
    // filters by default (AllowRespawnProxy): in story co-op a TriggerPhysical does not count it,
    // or it fired the stream sets, culls and checkpoints of the rooms it flew through.
    uint32_t entityVtable = 0;
    if (entity && Read(entity, &entityVtable, 4) && entityVtable == base + 0x17596E0 && !play::VersusMode()) {
        uint32_t proxyOwner = 0, ownerVtable = 0;
        const uint32_t component = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
        if (Read(component + kFilterOwner, &proxyOwner, 4) && proxyOwner && Read(proxyOwner, &ownerVtable, 4) && ownerVtable == base + kTriggerPhysicalVtableRva) {
            InterlockedIncrement(&filterProxyRefused);
            return false;
        }
    }
    const int bot = entity ? BotIndexOf(entity) : -1;
    const bool ghost = bot < 0 && entity && IsGhostPlayer(entity);
    // Evidence for the invasion: does the local necro body count for a trigger filter here?
    if (bot < 0 && !ghost && entity && IsNecroEntity(entity) && !IsGhostNecro(entity)) {
        const bool passed = filterHook.thiscall<bool>(self, entity);
        InterlockedIncrement(passed ? &localNecroPassed : &localNecroRefused);
        return passed;
    }
    if (bot < 0 && !ghost) {
        const bool passed = filterHook.thiscall<bool>(self, entity);
        uint32_t trigger = 0;
        if (passed && entity && entity == LocalSlotZeroEntity() && OwnerIsTrigger(self, trigger)) {
            lastLocalEntry = Entry{trigger, entity, GetTickCount() | 1u};
            InterlockedIncrement(&localEntries);
        }
        return passed;
    }
    const uint32_t comp = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    uint32_t owner = 0, vtable = 0;
    if (!Read(comp + kFilterOwner, &owner, 4) || !owner || !Read(owner, &vtable, 4)) return filterHook.thiscall<bool>(self, entity);
    if (vtable != base + kTriggerPhysicalVtableRva) {
        if (bot >= 0) {   // GravityField, DrainField and the other filtered fields stay the game's
            AcquireSRWLockExclusive(&filterLock);
            for (auto& o : filterOthers) {
                if (o.vtable && o.vtable != vtable) continue;
                o.vtable = vtable;
                ++o.count;
                break;
            }
            ReleaseSRWLockExclusive(&filterLock);
        }
        return filterHook.thiscall<bool>(self, entity);
    }
    InterlockedIncrement(&filterTriggerChecks);
    if (ghost) {
        if (!ghostFilter) {
            InterlockedIncrement(&filterGhostPassed);
            const bool passed = filterHook.thiscall<bool>(self, entity);
            if (passed) { lastGhostEntry = Entry{owner, entity, GetTickCount() | 1u}; InterlockedIncrement(&ghostEntries); }
            return passed;
        }
        InterlockedIncrement(&filterGhostRefused);
        return false;
    }
    if (!botsIgnore) return filterHook.thiscall<bool>(self, entity);
    InterlockedIncrement(&filterBotRefused[bot]);
    if (InterlockedIncrement(&filterLines) <= 30) {
        uint32_t guid[4]{};
        Read(owner + 0x24, guid, sizeof(guid));
        gate::Event("bot %d: trigger %08X (%08X-%08X-%08X-%08X) does not count it ([Bot] IgnoreTriggers)", bot + 1, owner, guid[0], guid[1], guid[2], guid[3]);
    }
    return false;
}

bool Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Coop", "GhostsIgnoreTriggers", 1, ini.c_str()) != 0;
    // mov ecx,[esp+8] / test ecx,ecx / je / mov edx,[esp+4] / mov eax,[edx]
    static const unsigned char kSig[] = {0x8B,0x4C,0x24,0x08,0x85,0xC9,0x74,0x7F,0x8B,0x54,0x24,0x04,0x8B,0x02};
    if (!hook_site::Install(log, "coop_ghosttrigger SendMessageTo", base + kSendToRva, kSig, sizeof(kSig),
                            reinterpret_cast<void*>(OnSendTo), sendHook))
        return false;
    liftWait = GetPrivateProfileIntA("Coop", "LiftWait", 0, ini.c_str()) != 0;
    cullGuard = GetPrivateProfileIntA("Streams", "GhostsIgnoreCullTriggers", 0, ini.c_str()) != 0;
    botsIgnore = botWanted && GetPrivateProfileIntA("Bot", "IgnoreTriggers", 0, ini.c_str()) != 0;
    blockBotLevelChange = botWanted && GetPrivateProfileIntA("Bot", "BlockLevelChange", 1, ini.c_str()) != 0;
    bool touchHooked = false;
    if (cullGuard || botsIgnore || blockBotLevelChange) {
        // push ebp / mov ebp,esp / and esp,-16 / sub esp,24h / push ebx / push esi / push edi / mov edi,[ebp+8] / mov esi,ecx
        static const unsigned char kTouch[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x24, 0x53, 0x56, 0x57, 0x8B, 0x7D, 0x08, 0x8B, 0xF1};
        touchHooked = hook_site::Install(log, "coop_ghosttrigger TriggerPhysical touch", base + kTouchRva, kTouch, sizeof(kTouch),
                                         reinterpret_cast<void*>(OnTouch), touchHook);
        cullGuard = cullGuard && touchHooked;
    }
    bool doorHooked = false;
    if (botsIgnore) {
        // push ebx / push esi / mov esi,ecx / cmp dword [esi+234h],0 / push edi / mov edi,[esp+10h]
        static const unsigned char kDoor[] = {0x53, 0x56, 0x8B, 0xF1, 0x83, 0xBE, 0x34, 0x02, 0x00, 0x00, 0x00, 0x57, 0x8B, 0x7C, 0x24, 0x10};
        doorHooked = hook_site::Install(log, "coop_ghosttrigger StandardDoor::TryToActivateDoor", base + kTryActivateRva, kDoor, sizeof(kDoor),
                                        reinterpret_cast<void*>(OnTryActivateDoor), doorActivateHook);
    }
    std::fprintf(log, "coop_ghosttrigger bots_ignore_doors=%d (a bot opens no door and loads no room behind the player)\n", doorHooked ? 1 : 0);
    ghostFilter = GetPrivateProfileIntA("Coop", "GhostTriggerFilter", 0, ini.c_str()) != 0;
    const bool ghostEntriesWanted = GetPrivateProfileIntA("Mirror", "GhostSequenceHoldMs", 20000, ini.c_str()) != 0;
    if (botsIgnore || ghostFilter || botWanted || ghostEntriesWanted) {
        // push ebx / push ebp / push esi / mov esi,ecx / xor bl,bl / push edi / cmp [esi+1Ch],bl / je / mov eax,[esi+14h]
        static const unsigned char kFilter[] = {0x53, 0x55, 0x56, 0x8B, 0xF1, 0x32, 0xDB, 0x57, 0x38, 0x5E, 0x1C, 0x74, 0x4F, 0x8B, 0x46, 0x14};
        filterHooked = hook_site::Install(log, "coop_ghosttrigger DS_EntityFilterComponent::CheckEntityPassesFilter", base + kFilterCheckRva, kFilter, sizeof(kFilter),
                                          reinterpret_cast<void*>(OnFilterCheck), filterHook);
    }
    std::fprintf(log, "coop_ghosttrigger trigger_filter=%d bots_refused=%d ghosts_refused=%d (TriggerPhysical volumes: a bot%s does not count for entering or occupying)\n",
                 filterHooked ? 1 : 0, filterHooked && botsIgnore ? 1 : 0, filterHooked && ghostFilter ? 1 : 0, ghostFilter ? " or a partner's ghost" : "");
    std::fprintf(log, "coop_ghosttrigger bots_ignore_triggers=%d touch_hook=%d (a bot pulls no trigger: no door, stream set, cull set, checkpoint or ambush)\n",
                 botsIgnore ? 1 : 0, touchHooked ? 1 : 0);
    std::fprintf(log, "coop_ghosttrigger ghosts_ignore_cull_triggers=%d (a partner's ghost does not switch the rooms we stand in)\n", cullGuard ? 1 : 0);
    std::fprintf(log, "coop_ghosttrigger bots_block_level_change=%d (a trigger only a bot touched never starts a level change)\n",
                 blockBotLevelChange && touchHooked ? 1 : 0);
    gate::audioTrace = GetPrivateProfileIntA("Coop", "AudioTrace", 0, ini.c_str()) != 0;
    std::fprintf(log, "coop_audio trace=%d (every sound-named message, local/replayed/direct, for comparing the two machines)\n", gate::audioTrace ? 1 : 0);
    liftWaitMs = static_cast<DWORD>(GetPrivateProfileIntA("Coop", "LiftWaitMs", 60000, ini.c_str()));
    std::fprintf(log, "coop_lift wait=%d timeout=%lu ms (a lift button waits until every partner is in the cabin)\n",
                 liftWait ? 1 : 0, liftWaitMs);
    installed = true;
    std::fprintf(log, "coop_ghosttrigger enabled=%d (ghost Players no longer pull triggers)\n", enabled);
    return true;
}

// A level change that starts within 3 s of a touch only a bot made, and was not
// stopped above: a chain the one-step check does not see (trigger -> MultiManager
// -> LevelChange). Logged so the next case names its trigger.
void CheckLevelChangeAfterBotTouch(FILE* log) {
    static bool wasPending = false;
    const bool pending = InterlockedCompareExchange(&levelChangePending, 0, 0) != 0;   // GameMainLoop is not asked from this thread
    if (pending && !wasPending && blockBotLevelChange) {
        const DWORD now = GetTickCount();
        for (const auto& t : touches)
            if (t.botAt && now - t.botAt < 3000 && !(t.localAt && now - t.localAt < 3000))
                std::fprintf(log, "coop_ghosttrigger level change started %lu ms after only a bot touched trigger %08X (not blocked: a chain?)\n",
                             static_cast<unsigned long>(now - t.botAt), t.trigger);
    }
    wasPending = pending;
}

void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    CheckLevelChangeAfterBotTouch(log);
    static LONG last = -1;
    const LONG now = InterlockedCompareExchange(&seen, 0, 0);
    // The lift counters move without any trigger send, so they count too.
    const LONG activity = now + InterlockedCompareExchange(&liftHeld, 0, 0) + InterlockedCompareExchange(&liftDropped, 0, 0) +
                          InterlockedCompareExchange(&liftReleased, 0, 0) + InterlockedCompareExchange(&botTriggers, 0, 0) +
                          InterlockedCompareExchange(&botTouches, 0, 0) + InterlockedCompareExchange(&botLevelChangesDropped, 0, 0) +
                          InterlockedCompareExchange(&gate::botLevelChangesHeld, 0, 0) + InterlockedCompareExchange(&doorByBot, 0, 0) +
                          InterlockedCompareExchange(&doorByLocal, 0, 0) + InterlockedCompareExchange(&doorByOther, 0, 0);
    if (activity == last) return;
    last = activity;
    std::fprintf(log, "coop_ghosttrigger bot_triggers_dropped=%ld bot_touches_dropped=%ld bot_level_changes_dropped=%ld (direct) %ld (touch)\n",
                 InterlockedCompareExchange(&botTriggers, 0, 0), InterlockedCompareExchange(&botTouches, 0, 0),
                 InterlockedCompareExchange(&botLevelChangesDropped, 0, 0), InterlockedCompareExchange(&gate::botLevelChangesHeld, 0, 0));
    {
        char others[200] = "";
        int used = 0;
        AcquireSRWLockShared(&filterLock);
        for (const auto& o : filterOthers)
            if (o.vtable && used >= 0 && used < static_cast<int>(sizeof(others)))
                used += std::snprintf(others + used, sizeof(others) - used, " %08X:%ld", static_cast<unsigned>(o.vtable - base), o.count);
        ReleaseSRWLockShared(&filterLock);
        std::fprintf(log, "coop_ghosttrigger trigger_filter calls=%ld trigger_checks=%ld bot_refused=%ld,%ld,%ld ghost_refused=%ld ghost_passed=%ld bot_other_owners(rva:count)=%s\n",
                     InterlockedCompareExchange(&filterCalls, 0, 0), InterlockedCompareExchange(&filterTriggerChecks, 0, 0),
                     InterlockedCompareExchange(&filterBotRefused[0], 0, 0), InterlockedCompareExchange(&filterBotRefused[1], 0, 0),
                     InterlockedCompareExchange(&filterBotRefused[2], 0, 0), InterlockedCompareExchange(&filterGhostRefused, 0, 0),
                     InterlockedCompareExchange(&filterGhostPassed, 0, 0), others[0] ? others : " none");
    }
    std::fprintf(log, "coop_ghosttrigger trigger_entries ghost=%ld local=%ld\n", InterlockedCompareExchange(&ghostEntries, 0, 0),
                 InterlockedCompareExchange(&localEntries, 0, 0));
    std::fprintf(log, "coop_ghosttrigger invasion proxy_refused=%ld local_necro_filters passed=%ld refused=%ld\n", InterlockedCompareExchange(&filterProxyRefused, 0, 0),
                 InterlockedCompareExchange(&localNecroPassed, 0, 0), InterlockedCompareExchange(&localNecroRefused, 0, 0));
    std::fprintf(log, "coop_ghosttrigger door_activations by_bot_refused=%ld by_player=%ld by_other=%ld\n", InterlockedCompareExchange(&doorByBot, 0, 0),
                 InterlockedCompareExchange(&doorByLocal, 0, 0), InterlockedCompareExchange(&doorByOther, 0, 0));
    std::fprintf(log, "coop_ghosttrigger trigger_sends=%ld dropped_ghost=%ld source_unresolved=%ld lift_held=%ld lift_released=%ld lift_timed_out=%ld lift_dropped=%ld\n", now,
                 InterlockedCompareExchange(&dropped, 0, 0), InterlockedCompareExchange(&notPlayer, 0, 0),
                 InterlockedCompareExchange(&liftHeld, 0, 0), InterlockedCompareExchange(&liftReleased, 0, 0),
                 InterlockedCompareExchange(&liftTimedOut, 0, 0), InterlockedCompareExchange(&liftDropped, 0, 0));
    std::fprintf(log, "coop_ghosttrigger cull_guard=%d ghost_touches=%ld culls_dropped=%ld\n", cullGuard ? 1 : 0,
                 InterlockedCompareExchange(&ghostTouches, 0, 0), InterlockedCompareExchange(&gate::cullsDropped, 0, 0));
}
}  // namespace ghosttrigger
