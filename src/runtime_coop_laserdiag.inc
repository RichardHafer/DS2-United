// Dead Space 2 United by Richard Hafer.
// The aiming laser of the own Isaac's gun ([Coop] LaserDiag, on by default, log only).
// Players saw it stay on while reloading and stay red (the empty-clip laser) after a reload
// (02.11.). Gun::UpdateAimingVFX (0x00DC8340, thiscall(gun), returns an Entity*) runs every
// frame for the equipped gun; its laser handles (beam +0x3C0, point +0x3C4, faded beam +0x3C8)
// and the loaded ammo (+0x39C) are logged whenever one of them changes on our Isaac's gun, and
// every reload start, so one test shows which step leaves the laser in the wrong state.
// It also reads the global aim data [0x020213AC]+0x20 that the bots' kinesis hooks swap.
namespace laserdiag {
constexpr uint32_t kUpdateAimingVfxRva = 0x9C8340, kGunOwner = 0x240, kAmmoLoaded = 0x39C, kBeam = 0x3C0, kPoint = 0x3C4, kFaded = 0x3C8;
safetyhook::InlineHook updateHook;
bool enabled = true, installed = false;
struct State { uint32_t gun = 0, beam = 0, point = 0, faded = 0, ret = 0; int32_t ammo = -1; };
State last;
volatile LONG frames = 0, changes = 0, lines = 0, reloads = 0, emptyFrames = 0, botAimSwapFrames = 0;
DWORD reloadAt = 0;
// Left-on lasers ([Coop] LaserCleanup, 13.11.: a plasma cutter's three beams stood frozen in
// the room, through reloads, on every gun). The laser only follows the gun while the gun runs
// UpdateAimingVFX; the switch off comes from the player's state messages
// (PlayerEquippedItemSM::HandleStateMessage 0x008A2F00 calls the gun's vtable +0x2E4, which
// kills the handles +0x3C0..+0x3CC and clears bit 8 of +0x390). A gun whose updates stopped
// with a handle still set holds a frozen laser; the frame tick calls that same +0x2E4 for it.
constexpr uint32_t kKillAimingVfxSlot = 0x2E4, kGunFlags = 0x390;
constexpr DWORD kStaleMs = 500;
struct Watched { uint32_t gun = 0, vtable = 0, owner = 0; DWORD at = 0; bool local = false; };
Watched watched[8]{};
bool cleanup = true;
volatile LONG cleanups = 0, cleanupFaults = 0, cleanupRefused = 0, cleanupLines = 0, watchFull = 0, leftOnSeen = 0;
void Watch(uint32_t g, uint32_t ownerRef, bool local) {
    const DWORD now = GetTickCount();
    Watched* free = nullptr;
    for (Watched& w : watched) {
        if (w.gun == g) { w.at = now ? now : 1; w.owner = ownerRef; w.local = local; return; }
        if (!w.gun && !free) free = &w;
    }
    if (!free) { InterlockedIncrement(&watchFull); return; }
    uint32_t vtable = 0;
    if (!Read(g, &vtable, 4)) return;
    *free = Watched{g, vtable, ownerRef, now ? now : 1, local};
}
__declspec(noinline) bool KillAimingVfx(uint32_t gun, uint32_t fn) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*)>(fn)(reinterpret_cast<void*>(static_cast<uintptr_t>(gun)));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// 14.11.: every left-on laser came from a switch-off out of PlayerEquippedItemSM in mid aim,
// a rebuild by the player's aim tick (0x00891AE9, runs while Sentient flag 7 at +0x298 is
// set) in the same frame, and the aim release a frame later without a second switch-off. The
// item machine's messages 0x1F..0x23, flag 7 changes and the rebuilding call are logged with
// the return addresses on the stack, ours marked, to see who sends them and in which order.
uint32_t frameNo = 0;
uint32_t OursEnd() {
    static uint32_t end = 0;
    if (!end) {
        const uintptr_t ours = reinterpret_cast<uintptr_t>(&__ImageBase);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(ours);
        end = static_cast<uint32_t>(ours + reinterpret_cast<const IMAGE_NT_HEADERS*>(ours + dos->e_lfanew)->OptionalHeader.SizeOfImage);
    }
    return end;
}
void StackCallers(const void* from, char* out, size_t size, bool raw = false) {
    const uint32_t ours = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&__ImageBase)), oursEnd = OursEnd();
    const uintptr_t sp = reinterpret_cast<uintptr_t>(from);
    size_t used = 0;
    unsigned found = 0;
    out[0] = 0;
    for (unsigned i = 0; i < 128 && found < 10 && used + 20 < size; ++i) {
        uint32_t v = 0;
        if (!Read(sp + i * 4, &v, 4)) break;
        const bool exe = v >= base + 0x1000 && v < base + 0x1600000, mine = v >= ours + 0x1000 && v < oursEnd;
        if (!exe && !mine) continue;
        if (raw) {
            used += static_cast<size_t>(std::snprintf(out + used, size - used, "%s%s%08X", used ? " " : "", mine ? "ours+" : "", mine ? v - ours : v));
            ++found;
            continue;
        }
        // only values right behind a call: E8 rel32, FF /2 as reg, [reg+disp8], [reg+disp32] or [abs32]
        uint8_t b[7]{};
        if (!Read(v - 7, b, 7)) continue;
        const bool call = b[2] == 0xE8 || (b[5] == 0xFF && (b[6] & 0xF8) == 0xD0) || (b[4] == 0xFF && (b[5] & 0xF8) == 0x50) ||
                          (b[1] == 0xFF && ((b[2] & 0xF8) == 0x90 || b[2] == 0x15));
        if (!call) continue;
        used += static_cast<size_t>(std::snprintf(out + used, size - used, "%s%s%08X", used ? " " : "", mine ? "ours+" : "", mine ? v - ours : v));
        ++found;
    }
    if (!used) std::snprintf(out, size, "none");
}
constexpr uint32_t kItemSmMessageRva = 0x4A2F00, kSetFlagRva = 0xAA52F0, kClearFlagRva = 0xA942D0;
constexpr uint32_t kSentientFlags = 0x298, kActiveItem = 0xCEC, kItemSmOwner = 0x50, kItemSmOther = 0x60;
safetyhook::InlineHook itemMsgHook, setFlagHook, clearFlagHook;
volatile LONG msgLines = 0, flagLines = 0, msgCounts[5]{}, flagSets = 0, flagClears = 0;
bool AimFlag(uint32_t player) {
    uint32_t f = 0;
    return Read(player + kSentientFlags, &f, 4) && (f & 0x80) != 0;
}
uint32_t BeamOf(uint32_t ref) {
    uint32_t beam = 0;
    return ref > 0x10 && Read(ref - 0x10 + kBeam, &beam, 4) ? beam : 0;
}
// PlayerEquippedItemSM::HandleStateMessage, thiscall(sm, a1, a2, a3, msg, a5), ret 0x14
uint32_t __fastcall OnItemSmMessage(void* sm, void*, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t msg, uint32_t a5) {
    const uint32_t s = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sm));
    uint32_t owner = 0, active = 0, other = 0;
    const uint32_t player = enabled && msg >= 0x1F && msg <= 0x23 ? LocalCampaignPlayer() : 0;
    const bool ours = player && Read(s + kItemSmOwner, &owner, 4) && owner == player;
    if (!ours) return itemMsgHook.thiscall<uint32_t>(sm, a1, a2, a3, msg, a5);
    Read(player + kActiveItem, &active, 4);
    Read(s + kItemSmOther, &other, 4);
    const bool flagBefore = AimFlag(player);
    const uint32_t beamBefore = BeamOf(active), otherBefore = BeamOf(other);
    char stack[200];
    StackCallers(_AddressOfReturnAddress(), stack, sizeof(stack));
    const uint32_t result = itemMsgHook.thiscall<uint32_t>(sm, a1, a2, a3, msg, a5);
    InterlockedIncrement(&msgCounts[msg - 0x1F]);
    if (InterlockedIncrement(&msgLines) <= 600)
        gate::Event("laser msg t=%lu frame=%lu msg %02X aim_flag=%d->%d gun %08X beam %08X->%08X other %08X beam %08X->%08X args %08X %08X %08X %08X | stack %s",
                    static_cast<unsigned long>(GetTickCount()), static_cast<unsigned long>(frameNo), msg, flagBefore ? 1 : 0, AimFlag(player) ? 1 : 0,
                    active > 0x10 ? active - 0x10 : 0, beamBefore, BeamOf(active), other > 0x10 ? other - 0x10 : 0, otherBefore, BeamOf(other), a1, a2, a3, a5, stack);
    return result;
}
void NoteFlag(uint32_t sentient, bool set, const void* from) {
    const uint32_t player = LocalCampaignPlayer();
    if (!player || sentient != player || AimFlag(player) == set) return;
    InterlockedIncrement(set ? &flagSets : &flagClears);
    if (InterlockedIncrement(&flagLines) > 600) return;
    char stack[200];
    StackCallers(from, stack, sizeof(stack));
    uint32_t active = 0;
    Read(player + kActiveItem, &active, 4);
    gate::Event("laser aim_flag %s t=%lu frame=%lu beam %08X | stack %s", set ? "set" : "cleared", static_cast<unsigned long>(GetTickCount()),
                static_cast<unsigned long>(frameNo), BeamOf(active), stack);
}
uint32_t __fastcall OnSetFlag(void* sentient, void*, uint32_t flag) {
    if (flag == 7 && enabled) NoteFlag(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sentient)), true, _AddressOfReturnAddress());
    return setFlagHook.thiscall<uint32_t>(sentient, flag);
}
uint32_t __fastcall OnClearFlag(void* sentient, void*, uint32_t flag) {
    if (flag == 7 && enabled) NoteFlag(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sentient)), false, _AddressOfReturnAddress());
    return clearFlagHook.thiscall<uint32_t>(sentient, flag);
}
// The rebuild needs bit 3 of +0x390 again, which the kill clears and only Gun's base
// StartAimingVFX (0x00DC81D0, vtable +0x2DC; every override jumps there) sets.
constexpr uint32_t kStartAimingVfxRva = 0x9C81D0;
safetyhook::InlineHook startHook;
volatile LONG startLines = 0, startsSeen = 0;
void __fastcall OnStartAimingVfx(void* gun, void*) {
    const uint32_t g = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(gun));
    uint32_t ownerRef = 0, beam = 0, flags = 0;
    const uint32_t player = enabled ? LocalCampaignPlayer() : 0;
    if (player && Read(g + kGunOwner, &ownerRef, 4) && ownerRef - 0x10 == player) {
        Read(g + kBeam, &beam, 4);
        Read(g + kGunFlags, &flags, 4);
        InterlockedIncrement(&startsSeen);
        if (InterlockedIncrement(&startLines) <= 300) {
            char stack[160];
            StackCallers(_AddressOfReturnAddress(), stack, sizeof(stack), true);
            gate::Event("laser on t=%lu frame=%lu: gun %08X beam %08X flags %08X aim_flag=%d ret %08X | raw %s", static_cast<unsigned long>(GetTickCount()),
                        static_cast<unsigned long>(frameNo), g, beam, flags, AimFlag(player) ? 1 : 0,
                        static_cast<unsigned>(reinterpret_cast<uintptr_t>(_ReturnAddress())), stack);
        }
    }
    startHook.thiscall<void>(gun);
}
// Who switches our laser off: Gun's base kill 0x00DB0A00 (thiscall(gun), ret; the guns' own
// +0x2E4 overrides kill their extra handles and jump here), logged with the caller for our
// Isaac's gun. A left-on laser is then the release whose usual caller never came.
constexpr uint32_t kKillAimingVfxRva = 0x9B0A00;
safetyhook::InlineHook killHook;
struct Caller { uint32_t rva = 0; LONG count = 0; };
Caller killCallers[8]{};
volatile LONG killsSeen = 0, killLines = 0;
void __fastcall OnKillAimingVfx(void* gun, void*) {
    const uint32_t g = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(gun));
    const uint32_t from = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    uint32_t ownerRef = 0, beam = 0;
    const uint32_t player = LocalCampaignPlayer();
    if (player && Read(g + kGunOwner, &ownerRef, 4) && ownerRef - 0x10 == player && Read(g + kBeam, &beam, 4) && beam) {
        InterlockedIncrement(&killsSeen);
        const uint32_t rva = from - static_cast<uint32_t>(base);
        for (Caller& c : killCallers) {
            if (c.rva == rva || !c.rva) { c.rva = rva; InterlockedIncrement(&c.count); break; }
        }
        if (InterlockedIncrement(&killLines) <= 300) {
            char stack[200];
            StackCallers(_AddressOfReturnAddress(), stack, sizeof(stack));
            gate::Event("laser off t=%lu frame=%lu: gun %08X beam %08X switched off from %08X aim_flag=%d | stack %s", static_cast<unsigned long>(GetTickCount()),
                        static_cast<unsigned long>(frameNo), g, beam, from, AimFlag(player) ? 1 : 0, stack);
        }
    }
    killHook.thiscall<void>(gun);
}
// The owner's state machines with their current state names, for the left-on line: which
// state the switch-off message should have come from. Same walk as the spawn watchdog's.
void OwnerStates(uint32_t actor, char* out, size_t size) {
    namespace m = ::ds2::watchdog::machine;
    out[0] = 0;
    uint32_t root = 0;
    if (!actor || !Read(actor + m::kActorTree + m::kTreeRoot, &root, 4) || !root) { std::snprintf(out, size, "unreadable"); return; }
    uint32_t stack[32]{}, seen[32]{};
    unsigned depth = 0, count = 0;
    size_t used = 0;
    stack[depth++] = root;
    while (depth) {
        const uint32_t sm = stack[--depth];
        bool repeat = false;
        for (unsigned i = 0; i < count; ++i) repeat = repeat || seen[i] == sm;
        if (repeat || count >= 32) continue;
        seen[count++] = sm;
        uint32_t array = 0, index = 0, name = 0;
        char text[32]{};
        if (Read(sm + m::kStateArr, &array, 4) && array && Read(sm + m::kCurState, &index, 4) && index < 4096 &&
            Read(array + index * m::kStateSize + m::kStateName, &name, 4) && name && Read(name, text, sizeof(text) - 1)) {
            // a name only when every byte up to its end is printable; other machines keep no name there
            size_t length = 0;
            bool plain = true;
            for (; length < sizeof(text) && text[length]; ++length) plain = plain && text[length] >= 0x20 && text[length] <= 0x7E;
            if (plain && length >= 3 && used + length + 2 < size) used += static_cast<size_t>(std::snprintf(out + used, size - used, "%s%s", used ? "," : "", text));
        }
        uint32_t child = 0, next = 0;
        if (Read(sm + m::kChildHead, &child, 4) && child && depth < 32) stack[depth++] = child;
        if (Read(sm + m::kNext, &next, 4) && next && depth < 32) stack[depth++] = next;
    }
    if (!used) std::snprintf(out, size, "none");
}
// Frame tick, game thread (the guns update on it too). With LaserCleanup=0 it only reports.
void Tick(DWORD now) {
    if (!installed) return;
    ++frameNo;
    for (Watched& w : watched) {
        if (!w.gun || static_cast<LONG>(now - w.at) < static_cast<LONG>(kStaleMs)) continue;
        const Watched seen = w;
        uint32_t vtable = 0, handles[4]{}, fn = 0, flags = 0;
        int32_t ammo = -1;
        if (!Read(seen.gun, &vtable, 4) || vtable != seen.vtable || !Read(seen.gun + kBeam, handles, sizeof(handles))) { w = Watched{}; continue; }
        if (!handles[0] && !handles[1] && !handles[2] && !handles[3]) {
            if (now - seen.at > 10000) w = Watched{};   // switched off as it should be
            continue;
        }
        w = Watched{};
        Read(seen.gun + kAmmoLoaded, &ammo, 4);
        Read(seen.gun + kGunFlags, &flags, 4);
        const bool slotOk = Read(vtable + kKillAimingVfxSlot, &fn, 4) && fn >= base + 0x1000 && fn < base + gate::kRdataLo;
        const bool ok = cleanup && slotOk && KillAimingVfx(seen.gun, fn);
        InterlockedIncrement(!cleanup ? &leftOnSeen : !slotOk ? &cleanupRefused : ok ? &cleanups : &cleanupFaults);
        if (InterlockedIncrement(&cleanupLines) <= 100) {
            char states[200];
            uint32_t ownerNow = 0;
            Read(seen.gun + kGunOwner, &ownerNow, 4);
            OwnerStates(ownerNow > 0x10 ? ownerNow - 0x10 : 0, states, sizeof(states));
            gate::Event("laser left on: gun %08X (%s, owner %08X) no aiming update for %lu ms, beam %08X point %08X faded %08X empty %08X, ammo %d, flags %08X%s; "
                        "owner states [%s] -> %s",
                        seen.gun, seen.local ? "our Isaac" : "another player or bot", ownerNow > 0x10 ? ownerNow - 0x10 : 0, static_cast<unsigned long>(now - seen.at),
                        handles[0], handles[1], handles[2], handles[3], ammo, flags,
                        reloadAt && now - reloadAt < 4000 ? ", within 4 s of a reload start" : "", states,
                        !cleanup ? "left on ([Coop] LaserCleanup=0)" : !slotOk ? "NOT switched off (kill slot outside the code)"
                                 : ok ? "switched off (gun vtable +0x2E4)" : "kill FAULTED");
        }
    }
}
uint32_t __fastcall OnUpdateAimingVfx(void* gun, void*) {
    const uint32_t result = updateHook.thiscall<uint32_t>(gun);
    if (!enabled) return result;
    const uint32_t g = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(gun));
    uint32_t ref = 0;
    const uint32_t player = LocalCampaignPlayer();
    uint32_t ownerRef = 0;
    Read(g + kGunOwner, &ownerRef, 4);
    Watch(g, ownerRef, player && ownerRef - 0x10 == player);
    if (!player || !Read(g + kGunOwner, &ref, 4) || ref - 0x10 != player) return result;
    InterlockedIncrement(&frames);
    State now;
    now.gun = g;
    now.ret = result;
    Read(g + kAmmoLoaded, &now.ammo, 4);
    Read(g + kBeam, &now.beam, 4);
    Read(g + kPoint, &now.point, 4);
    Read(g + kFaded, &now.faded, 4);
    if (now.ammo == 0) InterlockedIncrement(&emptyFrames);
    const bool changed = now.gun != last.gun || now.ammo != last.ammo || (now.beam != 0) != (last.beam != 0) || now.beam != last.beam ||
                         (now.point != 0) != (last.point != 0) || now.faded != last.faded || (now.ret != 0) != (last.ret != 0);
    if (changed) {
        InterlockedIncrement(&changes);
        if (InterlockedIncrement(&lines) <= 400) {
            const DWORD t = GetTickCount();
            char stack[200];
            StackCallers(_AddressOfReturnAddress(), stack, sizeof(stack));
            gate::Event("laser t=%lu frame=%lu gun %08X ammo_loaded %d -> %d, beam %08X point %08X faded %08X update_ret %08X aim_flag=%d%s | stack %s", t,
                        static_cast<unsigned long>(frameNo), g, last.gun == g ? last.ammo : -1, now.ammo, now.beam, now.point, now.faded, now.ret,
                        AimFlag(player) ? 1 : 0, reloadAt && t - reloadAt < 4000 ? " (within 4 s of a reload start)" : "", stack);
        }
        last = now;
    }
    return result;
}
// From the voices module's reload hook (runtime_coop_voices.inc): a reload of our Isaac starts.
void NoteReload(uint32_t player) {
    if (!enabled || !installed || player != LocalCampaignPlayer()) return;
    InterlockedIncrement(&reloads);
    reloadAt = GetTickCount();
    int32_t ammo = -1;
    uint32_t beam = 0;
    if (last.gun) { Read(last.gun + kAmmoLoaded, &ammo, 4); Read(last.gun + kBeam, &beam, 4); }
    if (InterlockedIncrement(&lines) <= 400) gate::Event("laser t=%lu reload starts: gun %08X ammo_loaded %d beam %08X", reloadAt, last.gun, ammo, beam);
}
void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Coop", "LaserDiag", 1, ini.c_str()) != 0;
    cleanup = GetPrivateProfileIntA("Coop", "LaserCleanup", 1, ini.c_str()) != 0;
    if (!enabled) cleanup = false;
    if (!enabled) { std::fprintf(log, "laser diagnostics and cleanup off ([Coop] LaserDiag=0)\n"); return; }
    // push ebp / mov ebp,esp / and esp,-16 / mov eax,[0x020213AC] / mov eax,[eax+20h]
    static const unsigned char kSite[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0xA1, 0xAC, 0x13, 0x02, 0x02, 0x8B, 0x40, 0x20};
    installed = hook_site::Install(log, "laser diagnostics Gun::UpdateAimingVFX", base + kUpdateAimingVfxRva, kSite, sizeof(kSite),
                                   reinterpret_cast<void*>(OnUpdateAimingVfx), updateHook);
    // push esi / mov esi,ecx / cmp dword ptr [esi+3C0h],0
    static const unsigned char kKill[] = {0x56, 0x8B, 0xF1, 0x83, 0xBE, 0xC0, 0x03, 0x00, 0x00, 0x00};
    const bool killOk = installed && hook_site::Install(log, "laser diagnostics Gun kill aiming VFX", base + kKillAimingVfxRva, kKill, sizeof(kKill),
                                                        reinterpret_cast<void*>(OnKillAimingVfx), killHook);
    std::fprintf(log, "laser switch-off callers %s (Gun 0x00DB0A00, our Isaac's gun only)\n", killOk ? "logged" : "NOT logged");
    // push ebx / push esi / push edi / mov edi,ecx / mov ecx,[edi+50h] / mov eax,[ecx+0CECh]
    static const unsigned char kItemMsg[] = {0x53, 0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x4F, 0x50, 0x8B, 0x81, 0xEC, 0x0C, 0x00, 0x00};
    // push esi / push edi / mov edi,[esp+0Ch] / mov esi,ecx
    static const unsigned char kSetFlag[] = {0x56, 0x57, 0x8B, 0x7C, 0x24, 0x0C, 0x8B, 0xF1};
    // mov edx,ecx / push esi / mov esi,[esp+8] / mov ecx,esi
    static const unsigned char kClearFlag[] = {0x8B, 0xD1, 0x56, 0x8B, 0x74, 0x24, 0x08, 0x8B, 0xCE};
    const bool msgOk = installed && hook_site::Install(log, "laser diagnostics PlayerEquippedItemSM messages", base + kItemSmMessageRva, kItemMsg, sizeof(kItemMsg),
                                                       reinterpret_cast<void*>(OnItemSmMessage), itemMsgHook);
    const bool setOk = installed && hook_site::Install(log, "laser diagnostics Sentient::SetBehaviorFlag", base + kSetFlagRva, kSetFlag, sizeof(kSetFlag),
                                                       reinterpret_cast<void*>(OnSetFlag), setFlagHook);
    const bool clearOk = installed && hook_site::Install(log, "laser diagnostics Sentient::ClearBehaviorFlag", base + kClearFlagRva, kClearFlag, sizeof(kClearFlag),
                                                         reinterpret_cast<void*>(OnClearFlag), clearFlagHook);
    // sub esp,14h / push ebx / push esi / mov esi,ecx / push edi / mov edi,[esi+0Ch]
    static const unsigned char kStart[] = {0x83, 0xEC, 0x14, 0x53, 0x56, 0x8B, 0xF1, 0x57, 0x8B, 0x7E, 0x0C};
    const bool startOk = installed && hook_site::Install(log, "laser diagnostics Gun::StartAimingVFX", base + kStartAimingVfxRva, kStart, sizeof(kStart),
                                                         reinterpret_cast<void*>(OnStartAimingVfx), startHook);
    std::fprintf(log, "laser switch-on callers %s (Gun 0x00DC81D0, our Isaac's gun only)\n", startOk ? "logged" : "NOT logged");
    std::fprintf(log, "laser message trace: item machine messages 0x1F..0x23 %s, aim flag 7 set %s, cleared %s (our Isaac only, stacks with ours+ for this plugin)\n",
                 msgOk ? "logged" : "NOT logged", setOk ? "logged" : "NOT logged", clearOk ? "logged" : "NOT logged");
    std::fprintf(log, "laser diagnostics %s ([Coop] LaserDiag): our gun's laser handles and loaded ammo on every change, every reload start; "
                      "cleanup %s ([Coop] LaserCleanup): a gun with no aiming update for %lu ms and a laser still set is switched off\n",
                 installed ? "on" : "NOT installed", cleanup && installed ? "on" : "off", static_cast<unsigned long>(kStaleMs));
}
void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG lastChanges = -1;
    const LONG c = InterlockedCompareExchange(&changes, 0, 0) + InterlockedCompareExchange(&reloads, 0, 0) * 1000 +
                   (InterlockedCompareExchange(&cleanups, 0, 0) + InterlockedCompareExchange(&cleanupFaults, 0, 0) + InterlockedCompareExchange(&leftOnSeen, 0, 0)) * 100000 +
                   InterlockedCompareExchange(&killsSeen, 0, 0) * 7;
    if (c == lastChanges) return;
    lastChanges = c;
    std::fprintf(log, "laser frames=%ld changes=%ld reloads=%ld empty_frames=%ld lines=%ld | left on: seen only=%ld switched off=%ld faults=%ld refused=%ld watch_full=%ld\n",
                 InterlockedCompareExchange(&frames, 0, 0), InterlockedCompareExchange(&changes, 0, 0), InterlockedCompareExchange(&reloads, 0, 0),
                 InterlockedCompareExchange(&emptyFrames, 0, 0), InterlockedCompareExchange(&lines, 0, 0), InterlockedCompareExchange(&leftOnSeen, 0, 0),
                 InterlockedCompareExchange(&cleanups, 0, 0),
                 InterlockedCompareExchange(&cleanupFaults, 0, 0), InterlockedCompareExchange(&cleanupRefused, 0, 0), InterlockedCompareExchange(&watchFull, 0, 0));
    if (InterlockedCompareExchange(&killsSeen, 0, 0)) {
        std::fprintf(log, "laser switched off by the game %ld times, callers:", InterlockedCompareExchange(&killsSeen, 0, 0));
        for (const Caller& k : killCallers)
            if (k.rva) std::fprintf(log, " %08X x%ld", k.rva + 0x400000u, k.count);
        std::fprintf(log, "\n");
    }
    std::fprintf(log, "laser item messages 1F=%ld 20=%ld 21=%ld 22=%ld 23=%ld | aim flag sets=%ld clears=%ld | switch-ons=%ld | frame=%lu\n", InterlockedCompareExchange(&msgCounts[0], 0, 0),
                 InterlockedCompareExchange(&msgCounts[1], 0, 0), InterlockedCompareExchange(&msgCounts[2], 0, 0), InterlockedCompareExchange(&msgCounts[3], 0, 0),
                 InterlockedCompareExchange(&msgCounts[4], 0, 0), InterlockedCompareExchange(&flagSets, 0, 0), InterlockedCompareExchange(&flagClears, 0, 0),
                 InterlockedCompareExchange(&startsSeen, 0, 0), static_cast<unsigned long>(frameNo));
}
}  // namespace laserdiag
