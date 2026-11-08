// Dead Space 2 United by Hafer.
// Loot drops, stage one: see them.
//
// An enemy's drop is ItemDropListComponent::DropItem (PC 0x010B44E0,
// `thiscall(x)`, ret 4; the PDB's order after HandleEvents 0x010B43E0 and
// PostInitFromParent 0x010B44A0). It runs on the machine that owns the enemy
// (the host, with HostNPCs) and nowhere else, so ammo and credits from kills
// lie on the host's floor only (COOP_WORLD_GAPS_2026-09-17.md, gap 3).
//
// Before the pickup can be mirrored it has to be named: the function creates
// the pickup entity (0x00DECCB0 on the owner), places it with an offset and
// an impulse, and at 0x010B47C3 does `mov ecx, edi / call 0x00479BC0` with
// edi = that entity and its final matrix on the stack. A mid hook there
// reads the entity: vtable, GUID, position. The next stage sends that to the
// clients and spawns the same pickup there, or registers it with the
// session (PickupItem is a multiplayer class: packUpdate, NetCreate,
// CreateItemOnRemotePickup).
namespace drops {
bool enabled = false, installed = false;
constexpr uint32_t kDropItemRva = 0xCB44E0;
constexpr uint32_t kPlacedRva = 0xCB47C3;
safetyhook::InlineHook dropHook;
safetyhook::MidHook placedHook;
volatile LONG calls = 0, placed = 0, unreadable = 0;
uint32_t lastEntity = 0, lastVtable = 0, lastGuid[4]{};

// A bot's hit on a corpse never uses up its loot ([Bot] KeepCorpseLoot).
// A dead enemy drops its item
// once, on the first damaging hit after it died: DeadSpaceNPCSM's CHECK_DROP_ITEM
// calls DropItem(true), returning to 0x00FF095E, whoever the hit came from. The
// bots kept firing at a target a moment after it fell, so its loot popped out for
// them (and they took it) before the human could stomp it (27.09.). For such a
// hit by a bot the drop is refused: the "dropped" bit (component+0x20 bit 31)
// stays clear and the human's stomp drops it as usual. The corpse's last hit is
// the health component's DamageData, attacker SafePtr at +0xA0 (entity+0x10).
constexpr uint32_t kCorpseHitReturnRva = 0xBF095E, kHealthTypeRva = 0x1CA4252, kLastAttacker = 0xA0, kDropOwner = 0x14;
bool keepBotLoot = false;
volatile LONG corpseHits = 0, keptForHuman = 0, corpseDropsHuman = 0, corpseDropsOther = 0, botStompDrops = 0;
// A corpse a bot stomps on purpose ([Bot] StompCorpses) may drop its loot for
// that bot's hit: the runtime names it here for a moment after each stomp.
struct BotLootPass { uint32_t npc = 0; DWORD until = 0; };
BotLootPass botLootPass[8];
void AllowBotLoot(uint32_t npc, DWORD until) {
    BotLootPass* slot = &botLootPass[0];
    for (BotLootPass& p : botLootPass) {
        if (p.npc == npc) { slot = &p; break; }
        if (static_cast<LONG>(p.until - slot->until) < 0) slot = &p;
    }
    *slot = BotLootPass{npc, until};
}
bool BotLootAllowed(uint32_t npc) {
    const DWORD now = GetTickCount();
    for (const BotLootPass& p : botLootPass)
        if (p.npc == npc && static_cast<LONG>(p.until - now) > 0) return true;
    return false;
}
// A component by its type byte: index = byte [[entity+0x44] + type + 0xC], component = [[entity+0x48] + index*4].
uint32_t ComponentOf(uint32_t entity, uint32_t typeRva) {
    unsigned char type = 0, index = 0;
    uint32_t types = 0, list = 0, component = 0, owner = 0;
    if (!entity || !Read(base + typeRva, &type, 1) || !type || !Read(entity + 0x44, &types, 4) || !types || !Read(entity + 0x48, &list, 4) || !list) return 0;
    if (!Read(types + type + 0xC, &index, 1) || !Read(list + index * 4u, &component, 4) || !component) return 0;
    return Read(component + 0x10, &owner, 4) && owner == entity ? component : 0;
}
int __fastcall OnDropItem(void* self, void*, int arg) {
    InterlockedIncrement(&calls);
    {   // a Dementia phantom leaves nothing behind
        uint32_t owner = 0;
        if (phantom::Any() && Read(reinterpret_cast<uintptr_t>(self) + kDropOwner, &owner, 4) && phantom::Is(owner)) {
            InterlockedIncrement(&phantom::dropsRefused);
            return 0;
        }
    }
    if (keepBotLoot && reinterpret_cast<uintptr_t>(_ReturnAddress()) == base + kCorpseHitReturnRva) {
        InterlockedIncrement(&corpseHits);
        uint32_t npc = 0, attacker = 0;
        const uint32_t health = Read(reinterpret_cast<uintptr_t>(self) + kDropOwner, &npc, 4) ? ComponentOf(npc, kHealthTypeRva) : 0;
        if (health && Read(health + kLastAttacker, &attacker, 4) && attacker > 0x10010) {
            const uint32_t who = attacker - 0x10;
            if (IsBotEntity(who) && BotLootAllowed(npc)) {
                if (InterlockedIncrement(&botStompDrops) <= 40) gate::Event("coop_drops corpse %08X: stomped by the bot %08X on purpose, its loot drops", npc, who);
                return dropHook.thiscall<int>(self, arg);
            }
            if (IsBotEntity(who)) {
                if (InterlockedIncrement(&keptForHuman) <= 20) gate::Event("coop_drops corpse %08X: hit by the bot %08X, its loot kept for the player's stomp", npc, who);
                return 0;
            }
            InterlockedIncrement(who == LocalCampaignPlayer() ? &corpseDropsHuman : &corpseDropsOther);
        } else {
            InterlockedIncrement(&corpseDropsOther);
        }
    }
    return dropHook.thiscall<int>(self, arg);
}

void OnPlaced(SafetyHookContext& ctx) {
    const uint32_t entity = static_cast<uint32_t>(ctx.edi);
    uint32_t vtable = 0, guid[4]{};
    float pos[3]{};
    if (!entity || !Read(entity, &vtable, 4)) { InterlockedIncrement(&unreadable); return; }
    Read(entity + 0x24, guid, 16);
    Read(entity + placement::kPlayerPosition, pos, 12);
    lastEntity = entity; lastVtable = vtable; std::memcpy(lastGuid, guid, 16);
    InterlockedIncrement(&placed);
    if (enabled && gate::trace)
        gate::Event("coop_drops pickup placed entity=%08X vtable=%08X guid=%08X-%08X-%08X-%08X pos=(%.1f %.1f %.1f) host=%d",
                    entity, vtable, guid[0], guid[1], guid[2], guid[3], pos[0], pos[1], pos[2], IsHost() ? 1 : 0);
}

bool Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Drops", "Observe", 1, ini.c_str()) != 0 ||
              (GetPrivateProfileIntA("Bot", "Enabled", 0, ini.c_str()) != 0 && GetPrivateProfileIntA("Bot", "KeepCorpseLoot", 0, ini.c_str()) != 0);
    keepBotLoot = GetPrivateProfileIntA("Bot", "Enabled", 0, ini.c_str()) != 0 && GetPrivateProfileIntA("Bot", "KeepCorpseLoot", 0, ini.c_str()) != 0;
    std::fprintf(log, "coop_drops keep_corpse_loot=%d (a bot's hit on a corpse does not pop its loot; the player's stomp does)\n", keepBotLoot ? 1 : 0);
    if (!enabled) return false;
    // push ebp / mov ebp,esp / and esp,-16 / sub esp,0xD4 / push ebx / push esi / mov esi,ecx / mov eax,[esi+0x20]
    static const unsigned char kDrop[] = {0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x81,0xEC,0xD4,0x00,0x00,0x00,0x53,0x56,0x8B,0xF1,0x8B,0x46,0x20};
    static const unsigned char kPlaced[] = {0x8B,0xCF,0xE8};   // mov ecx,edi / call
    if (!hook_site::Install(log, "coop_drops DropItem", base + kDropItemRva, kDrop, sizeof(kDrop),
                            reinterpret_cast<void*>(OnDropItem), dropHook))
        return false;
    unsigned char actual[3]{};
    if (!Read(base + kPlacedRva, actual, 3) || std::memcmp(actual, kPlaced, 3) != 0) {
        std::fprintf(log, "coop_drops placement signature mismatch at %08X\n", static_cast<unsigned>(base + kPlacedRva));
        return false;
    }
    auto result = safetyhook::MidHook::create(reinterpret_cast<void*>(base + kPlacedRva), OnPlaced);
    if (!result) { std::fprintf(log, "coop_drops placement hook failed\n"); return false; }
    placedHook = std::move(*result);
    installed = true;
    std::fprintf(log, "coop_drops observing DropItem %08X (pickup named at %08X)\n",
                 static_cast<unsigned>(base + kDropItemRva), static_cast<unsigned>(base + kPlacedRva));
    return true;
}

void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    const LONG activity = InterlockedCompareExchange(&calls, 0, 0) + InterlockedCompareExchange(&placed, 0, 0) + corpseHits;
    if (activity == last) return;
    last = activity;
    if (keepBotLoot)
        std::fprintf(log, "coop_drops corpse_hits=%ld kept_for_player=%ld dropped_by_player=%ld dropped_by_other=%ld dropped_by_bot_stomp=%ld\n", corpseHits,
                     keptForHuman, corpseDropsHuman, corpseDropsOther, botStompDrops);
    std::fprintf(log, "coop_drops drop_calls=%ld placed=%ld unreadable=%ld last=%08X vtable=%08X guid=%08X-%08X-%08X-%08X\n",
                 InterlockedCompareExchange(&calls, 0, 0), InterlockedCompareExchange(&placed, 0, 0),
                 InterlockedCompareExchange(&unreadable, 0, 0), lastEntity, lastVtable, lastGuid[0], lastGuid[1], lastGuid[2], lastGuid[3]);
}
}  // namespace drops
