// Dead Space 2 United by Richard Hafer.
// The human's mouse wheel and the PC-only key events, kept away from the bots.
//
// The PC ControllerManager (vtable 0x01B7890C) answers "is event E active /
// pressed on controller C" in two slots: +0x10 IsEventActive (0x010D4020) and
// +0x14 WasEventPressed (0x010D4070), both thiscall(manager, controller, event),
// ret 8. Each asks the base pad mapping for that controller first (0x0077DA80 /
// 0x0077DAA0) and then falls back to a list of pending PC events at +0x18D8
// (last frame's at +0x18DC) without looking at the controller. The mouse wheel
// lands in that list (0x00439AC0 and its two siblings push 0xCB5E0448 for a
// positive delta, 0x011591CF for a negative one through 0x010E6970), so every
// Player that asks with its own controller id (Player+0x41C) saw the human's
// wheel: the weapon cycle in the equipped-item state (0x0089CA64 asks +0x10 for
// both hashes, +1/-1 through the four slots) ran on every bot (07.10.). For a
// bot's pad a yes that only came from the pending list becomes a no.
namespace bot_wheel {
constexpr uint32_t kEventActiveRva = 0xCD4020, kEventPressedRva = 0xCD4070, kBaseActiveRva = 0x37DA80, kBasePressedRva = 0x37DAA0;
constexpr uint32_t kPending = 0x18D8, kPendingPrevious = 0x18DC;
constexpr uint32_t kWheelUp = 0xCB5E0448u, kWheelDown = 0x011591CFu;
safetyhook::InlineHook activeHook, pressedHook;
bool enabled = false;
volatile LONG wheelBlocked = 0, otherBlocked = 0, blockLines = 0, botAsks = 0;
DWORD lastBlockAt[kMaxBots]{};
uint32_t lastBlockEvent[kMaxBots]{};

int BotPad(uint32_t controller) {
    for (const Unit& unit : units)
        if (unit.entity && (controller & 0xFF) == unit.controller) return static_cast<int>(unit.index);
    return -1;
}
// The retail list: {begin, end, capacity} behind the pointer at manager+offset.
bool Listed(uintptr_t manager, uint32_t offset, uint32_t event) {
    uint32_t list = 0, range[2]{};
    if (!Read(manager + offset, &list, 4) || !list || !Read(list, range, sizeof(range)) || range[1] < range[0] || range[1] - range[0] > 64 * 4) return false;
    for (uint32_t at = range[0]; at < range[1]; at += 4) {
        uint32_t value = 0;
        if (Read(at, &value, 4) && value == event) return true;
    }
    return false;
}
void NoteBlocked(int bot, uint32_t controller, uint32_t event, bool pressed, uintptr_t caller) {
    const bool wheel = event == kWheelUp || event == kWheelDown;
    // One count per notch and bot: a notch sits in the list for a frame and may be asked twice.
    const DWORD now = GetTickCount();
    if (lastBlockEvent[bot] != event || now - lastBlockAt[bot] > 40) InterlockedIncrement(wheel ? &wheelBlocked : &otherBlocked);
    lastBlockEvent[bot] = event;
    lastBlockAt[bot] = now;
    if (InterlockedIncrement(&blockLines) <= 24)
        gate::Event("bot wheel guard: %s %08X (%s) on pad %u of bot %d refused (asked by %08X, the human's PC event)", pressed ? "press" : "state", event,
                    event == kWheelUp ? "wheel up" : event == kWheelDown ? "wheel down" : "PC key", controller & 0xFF, bot + 1,
                    static_cast<unsigned>(caller));
}
bool __fastcall OnEventActive(void* manager, void*, uint32_t controller, uint32_t event) {
    const bool result = activeHook.thiscall<bool>(manager, controller, event);
    if (!result) return false;
    const int bot = BotPad(controller);
    if (bot < 0) return true;
    InterlockedIncrement(&botAsks);
    if (reinterpret_cast<bool(__thiscall*)(void*, uint32_t, uint32_t)>(base + kBaseActiveRva)(manager, controller, event)) return true;
    if (!Listed(reinterpret_cast<uintptr_t>(manager), kPending, event)) return true;
    NoteBlocked(bot, controller, event, false, reinterpret_cast<uintptr_t>(_ReturnAddress()));
    return false;
}
bool __fastcall OnEventPressed(void* manager, void*, uint32_t controller, uint32_t event) {
    const bool result = pressedHook.thiscall<bool>(manager, controller, event);
    if (!result) return false;
    const int bot = BotPad(controller);
    if (bot < 0) return true;
    InterlockedIncrement(&botAsks);
    if (reinterpret_cast<bool(__thiscall*)(void*, uint32_t, uint32_t)>(base + kBasePressedRva)(manager, controller, event)) return true;
    const uintptr_t m = reinterpret_cast<uintptr_t>(manager);
    if (!Listed(m, kPending, event) || Listed(m, kPendingPrevious, event)) return true;
    NoteBlocked(bot, controller, event, true, reinterpret_cast<uintptr_t>(_ReturnAddress()));
    return false;
}
bool Install(FILE* log, bool wanted) {
    if (!wanted) { std::fprintf(log, "bot wheel_guard=0 (WheelGuard=0: the human's mouse wheel cycles every bot's weapon too)\n"); return false; }
    // mov eax,[esp+4] / push esi / push edi / mov edi,[esp+10h] / push edi / push eax / mov esi,ecx / call base check
    unsigned char kActive[] = {0x8B, 0x44, 0x24, 0x04, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x10, 0x57, 0x50, 0x8B, 0xF1, 0xE8, 0, 0, 0, 0};
    unsigned char kPressed[sizeof(kActive)];
    std::memcpy(kPressed, kActive, sizeof(kActive));
    const int32_t toActive = static_cast<int32_t>(kBaseActiveRva - (kEventActiveRva + 0x13)), toPressed = static_cast<int32_t>(kBasePressedRva - (kEventPressedRva + 0x13));
    std::memcpy(kActive + 15, &toActive, 4);
    std::memcpy(kPressed + 15, &toPressed, 4);
    // The two slots of the live vtable must still be these functions.
    uint32_t slots[2]{};
    const bool vtable = Read(base + 0x177891C, slots, sizeof(slots)) && slots[0] == base + kEventActiveRva && slots[1] == base + kEventPressedRva;
    const bool active = hook_site::Install(log, "bot wheel guard ControllerManager::IsEventActive", base + kEventActiveRva, kActive, sizeof(kActive),
                                           reinterpret_cast<void*>(OnEventActive), activeHook);
    const bool pressed = hook_site::Install(log, "bot wheel guard ControllerManager::WasEventPressed", base + kEventPressedRva, kPressed, sizeof(kPressed),
                                            reinterpret_cast<void*>(OnEventPressed), pressedHook);
    enabled = active && pressed;
    std::fprintf(log, "bot wheel_guard=%d active=%d pressed=%d vtable_slots=%08X,%08X%s (the PC event list at ControllerManager+0x18D8, wheel up %08X / "
                      "down %08X and the PC-only keys, answers only the human's pad; bot pads 12-14 get their own mapping)\n",
                 enabled ? 1 : 0, active ? 1 : 0, pressed ? 1 : 0, slots[0], slots[1], vtable ? "" : " (not the expected slots)", kWheelUp, kWheelDown);
    return enabled;
}
void WriteDiagnostics(FILE* log) {
    static LONG last = -1;
    const LONG n = wheelBlocked + otherBlocked;
    if (n == last) return;
    last = n;
    std::fprintf(log, "bot wheel guard: wheel_switches_blocked=%ld pc_events_blocked=%ld bot_yes_answers_checked=%ld\n", wheelBlocked, otherBlocked, botAsks);
}
}  // namespace bot_wheel
