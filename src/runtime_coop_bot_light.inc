// Dead Space 2 United by Hafer.
// Each Isaac's aim light points where that Isaac aims.
//
// The light a Player switches on while aiming is its Flashlight (Player+0x418;
// Enable 0x0088D5F0 plays PlayerSettings' flashlight VFX, handle at +4, Update
// 0x00875040 moves it every frame). Its matrix comes from Flashlight::GetAimMatrix
// (0x00874E80, thiscall(flashlight, matrix out), ret 4): the light sits at the
// equipped gun (Player+0xCEC, Gun matrix 0x00DA70C0) and, while the aim camera
// is up (Player+0x3DC bit 27, set at 0x00882BA5 right after the aim camera is
// pushed for Player+0x420) on a Player that is not a net ghost, points at a spot
// 7 m along the camera that CameraManager::GetViewportCamera(0) (0x00792970,
// thiscall(cm, player id), ret 4) returns. The 0 is a literal, the human's
// viewport, so every aiming bot's light swung towards the human's crosshair.
// For a bot the call gets the bot's own player id; its viewport is in the
// CameraManager's list (cm+0x168) or the call keeps the retail 0.
namespace bot_light {
constexpr uint32_t kGetAimMatrixRva = 0x474E80, kCameraCallRva = 0x474F04, kGetViewportCameraRva = 0x392970, kAimDistanceRva = 0x1B5E310;
constexpr uint32_t kFlashlight = 0x418, kFlashlightVfx = 4, kGunOwner = 0x240, kCameraMatrix = 0x50;
safetyhook::MidHook cameraHook;
bool enabled = false;
volatile LONG humanCalls = 0, botFixed = 0, botNoViewport = 0, otherCalls = 0, lines = 0;
LONG botLines[kMaxBots]{};

// The camera matrix GetViewportCamera hands out for a viewport index: forward row
// at +0x20, position at +0x30.
bool CameraPoint(uint32_t cm, int index, Vec3& at) {
    uint32_t blenders = 0;
    float m[16]{};
    if (index < 0 || !Read(cm + kCmBlenders, &blenders, 4) || !blenders || !Read(blenders + index * kBlenderSize + kCameraMatrix, m, sizeof(m))) return false;
    if (!std::isfinite(m[8]) || !std::isfinite(m[12])) return false;
    at = {m[12] + m[8] * 7.0f, m[13] + m[9] * 7.0f, m[14] + m[10] * 7.0f};
    return true;
}
int ViewportOf(uint32_t cm, uint32_t id) {
    uint32_t count = 0;
    if (!Read(cm + kCmNumViewports, &count, 4) || count > 4) return -1;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t player = 0;
        if (Read(cm + kCmPlayerIds + i * 4, &player, 4) && player == id) return static_cast<int>(i);
    }
    return -1;
}
// At the call: esi is the gun, ecx the CameraManager, [esp] the pushed 0.
void OnCameraCall(SafetyHookContext& ctx) {
    uint32_t ref = 0;
    if (!Read(ctx.esi + kGunOwner, &ref, 4) || ref < 0x10010) { InterlockedIncrement(&otherCalls); return; }
    const uint32_t owner = ref - 0x10;
    const Unit* unit = UnitOf(owner);
    if (!unit) {
        InterlockedIncrement(owner == LocalCampaignPlayer() ? &humanCalls : &otherCalls);
        return;
    }
    const uint32_t cm = static_cast<uint32_t>(ctx.ecx);
    uint32_t id = 0;
    const int viewport = Read(owner + kPlayerId, &id, 4) ? ViewportOf(cm, id) : -1;
    if (viewport < 0 || id == 0) {
        InterlockedIncrement(&botNoViewport);
        if (InterlockedIncrement(&lines) <= 30)
            gate::Event("bot aim light: bot %u (Isaac %08X, player id %u) has no viewport of its own; its light keeps the human's camera", unit->index + 1, owner, id);
        return;
    }
    *reinterpret_cast<uint32_t*>(ctx.esp) = id;
    InterlockedIncrement(&botFixed);
    if (InterlockedIncrement(&botLines[unit->index]) <= 4 && InterlockedIncrement(&lines) <= 30) {
        Vec3 retail{}, own{};
        const int human = ViewportOf(cm, 0);
        const bool both = CameraPoint(cm, human, retail) && CameraPoint(cm, viewport, own);
        gate::Event("bot aim light: bot %u (Isaac %08X, player id %u) aims its light with viewport %d instead of viewport %d; aim points %.1f m apart%s",
                    unit->index + 1, owner, id, viewport, human, both ? coop_bot::Distance(retail, own) : -1.0f, both ? "" : " (camera unreadable)");
    }
}
bool Install(FILE* log, bool wanted) {
    if (!wanted) { std::fprintf(log, "bot own_aim_light=0 (OwnAimLight=0: an aiming bot's light points at the human's crosshair)\n"); return false; }
    // push ebp / mov ebp,esp / and esp,-16 / mov eax,[ecx] / mov ecx,[eax+0CECh] / sub esp,5Ch / push esi
    static const unsigned char kEntry[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x8B, 0x01, 0x8B, 0x88, 0xEC, 0x0C, 0x00, 0x00, 0x83, 0xEC, 0x5C, 0x56};
    // mov ecx,[CameraManager] / push 0
    const uint32_t cmGlobal = static_cast<uint32_t>(base) + kCameraManagerRva, distance = static_cast<uint32_t>(base) + kAimDistanceRva;
    const unsigned char kPush[] = {0x8B, 0x0D, static_cast<unsigned char>(cmGlobal), static_cast<unsigned char>(cmGlobal >> 8), static_cast<unsigned char>(cmGlobal >> 16),
                                   static_cast<unsigned char>(cmGlobal >> 24), 0x6A, 0x00};
    // call GetViewportCamera / movss xmm0,[7.0]
    const int32_t rel = static_cast<int32_t>(kGetViewportCameraRva - (kCameraCallRva + 5));
    unsigned char kCall[] = {0xE8, 0, 0, 0, 0, 0xF3, 0x0F, 0x10, 0x05, static_cast<unsigned char>(distance), static_cast<unsigned char>(distance >> 8),
                             static_cast<unsigned char>(distance >> 16), static_cast<unsigned char>(distance >> 24)};
    std::memcpy(kCall + 1, &rel, 4);
    unsigned char entry[sizeof(kEntry)]{}, push[sizeof(kPush)]{};
    float seven = 0.0f;
    const bool shape = Read(base + kGetAimMatrixRva, entry, sizeof(entry)) && std::memcmp(entry, kEntry, sizeof(kEntry)) == 0 &&
                       Read(base + kCameraCallRva - sizeof(kPush), push, sizeof(push)) && std::memcmp(push, kPush, sizeof(kPush)) == 0 &&
                       Read(distance, &seven, 4) && seven == 7.0f;
    if (!shape) {
        std::fprintf(log, "bot own_aim_light=0 (Flashlight::GetAimMatrix at %08X does not match: entry %02X %02X %02X, push %02X %02X, distance %.2f)\n",
                     static_cast<unsigned>(base + kGetAimMatrixRva), entry[0], entry[1], entry[2], push[0], push[6], seven);
        return false;
    }
    enabled = hook_site::InstallMid(log, "bot aim light Flashlight::GetAimMatrix camera call", base + kCameraCallRva, kCall, sizeof(kCall), OnCameraCall, cameraHook);
    std::fprintf(log, "bot own_aim_light=%d (Flashlight::GetAimMatrix %08X asks GetViewportCamera with the bot's player id instead of 0)\n", enabled ? 1 : 0,
                 static_cast<unsigned>(base + kGetAimMatrixRva));
    return enabled;
}
// Whose aim light is on: Player+0x418 Flashlight, VFX handle at +4.
uint32_t LightOf(uint32_t player) {
    uint32_t light = 0, handle = 0;
    return player && Read(player + kFlashlight, &light, 4) && light && Read(light + kFlashlightVfx, &handle, 4) ? handle : 0;
}
void WriteDiagnostics(FILE* log) {
    static LONG last = -1;
    const LONG n = humanCalls + botFixed + botNoViewport + otherCalls;
    static unsigned lastLit = ~0u;
    static LONG litLines = 0;
    const uint32_t human = LocalCampaignPlayer();
    uint32_t handles[kMaxBots + 1]{LightOf(human)};
    unsigned lit = handles[0] ? 1u : 0u;
    for (unsigned i = 0; i < kMaxBots; ++i) {
        handles[i + 1] = LightOf(units[i].entity);
        if (handles[i + 1]) lit |= 2u << i;
    }
    if (lit != lastLit && litLines < 60) {
        ++litLines;
        lastLit = lit;
        std::fprintf(log, "bot aim light lit: human %08X vfx %08X; bots %08X vfx %08X, %08X vfx %08X, %08X vfx %08X\n", human, handles[0], units[0].entity, handles[1],
                     units[1].entity, handles[2], units[2].entity, handles[3]);
    }
    if (n == last) return;
    last = n;
    std::fprintf(log, "bot aim light: human_aims=%ld bot_aims_own_camera=%ld bot_no_viewport=%ld other=%ld\n", humanCalls, botFixed, botNoViewport, otherCalls);
}
}  // namespace bot_light
