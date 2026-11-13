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
volatile LONG cleanups = 0, cleanupFaults = 0, cleanupRefused = 0, cleanupLines = 0, watchFull = 0;
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
// Frame tick, game thread (the guns update on it too).
void Tick(DWORD now) {
    if (!installed || !cleanup) return;
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
        const bool ok = slotOk && KillAimingVfx(seen.gun, fn);
        InterlockedIncrement(!slotOk ? &cleanupRefused : ok ? &cleanups : &cleanupFaults);
        if (InterlockedIncrement(&cleanupLines) <= 100)
            gate::Event("laser left on: gun %08X (%s, owner %08X) no aiming update for %lu ms, beam %08X point %08X faded %08X empty %08X, ammo %d, flags %08X%s -> %s",
                        seen.gun, seen.local ? "our Isaac" : "another player or bot", seen.owner ? seen.owner - 0x10 : 0, static_cast<unsigned long>(now - seen.at),
                        handles[0], handles[1], handles[2], handles[3], ammo, flags,
                        reloadAt && now - reloadAt < 4000 ? ", within 4 s of a reload start" : "",
                        !slotOk ? "NOT switched off (kill slot outside the code)" : ok ? "switched off (gun vtable +0x2E4)" : "kill FAULTED");
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
            gate::Event("laser t=%lu gun %08X ammo_loaded %d -> %d, beam %08X point %08X faded %08X update_ret %08X%s", t, g, last.gun == g ? last.ammo : -1, now.ammo, now.beam,
                        now.point, now.faded, now.ret, reloadAt && t - reloadAt < 4000 ? " (within 4 s of a reload start)" : "");
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
    std::fprintf(log, "laser diagnostics %s ([Coop] LaserDiag): our gun's laser handles and loaded ammo on every change, every reload start; "
                      "cleanup %s ([Coop] LaserCleanup): a gun with no aiming update for %lu ms and a laser still set is switched off\n",
                 installed ? "on" : "NOT installed", cleanup && installed ? "on" : "off", static_cast<unsigned long>(kStaleMs));
}
void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG lastChanges = -1;
    const LONG c = InterlockedCompareExchange(&changes, 0, 0) + InterlockedCompareExchange(&reloads, 0, 0) * 1000 +
                   (InterlockedCompareExchange(&cleanups, 0, 0) + InterlockedCompareExchange(&cleanupFaults, 0, 0)) * 100000;
    if (c == lastChanges) return;
    lastChanges = c;
    std::fprintf(log, "laser frames=%ld changes=%ld reloads=%ld empty_frames=%ld lines=%ld | left on and switched off=%ld faults=%ld refused=%ld watch_full=%ld\n",
                 InterlockedCompareExchange(&frames, 0, 0), InterlockedCompareExchange(&changes, 0, 0), InterlockedCompareExchange(&reloads, 0, 0),
                 InterlockedCompareExchange(&emptyFrames, 0, 0), InterlockedCompareExchange(&lines, 0, 0), InterlockedCompareExchange(&cleanups, 0, 0),
                 InterlockedCompareExchange(&cleanupFaults, 0, 0), InterlockedCompareExchange(&cleanupRefused, 0, 0), InterlockedCompareExchange(&watchFull, 0, 0));
}
}  // namespace laserdiag
