// Dead Space 2 United by Hafer.
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
uint32_t __fastcall OnUpdateAimingVfx(void* gun, void*) {
    const uint32_t result = updateHook.thiscall<uint32_t>(gun);
    if (!enabled) return result;
    const uint32_t g = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(gun));
    uint32_t ref = 0;
    const uint32_t player = LocalCampaignPlayer();
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
    if (!enabled) { std::fprintf(log, "laser diagnostics off ([Coop] LaserDiag=0)\n"); return; }
    // push ebp / mov ebp,esp / and esp,-16 / mov eax,[0x020213AC] / mov eax,[eax+20h]
    static const unsigned char kSite[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0xA1, 0xAC, 0x13, 0x02, 0x02, 0x8B, 0x40, 0x20};
    installed = hook_site::Install(log, "laser diagnostics Gun::UpdateAimingVFX", base + kUpdateAimingVfxRva, kSite, sizeof(kSite),
                                   reinterpret_cast<void*>(OnUpdateAimingVfx), updateHook);
    std::fprintf(log, "laser diagnostics %s ([Coop] LaserDiag): our gun's laser handles and loaded ammo on every change, every reload start\n",
                 installed ? "on" : "NOT installed");
}
void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG lastChanges = -1;
    const LONG c = InterlockedCompareExchange(&changes, 0, 0) + InterlockedCompareExchange(&reloads, 0, 0) * 1000;
    if (c == lastChanges) return;
    lastChanges = c;
    std::fprintf(log, "laser frames=%ld changes=%ld reloads=%ld empty_frames=%ld lines=%ld\n", InterlockedCompareExchange(&frames, 0, 0),
                 InterlockedCompareExchange(&changes, 0, 0), InterlockedCompareExchange(&reloads, 0, 0), InterlockedCompareExchange(&emptyFrames, 0, 0),
                 InterlockedCompareExchange(&lines, 0, 0));
}
}  // namespace laserdiag
