// Dead Space 2 United by Richard Hafer.
// The pause menu does not stop the world in co-op ([Coop] PauseKeepsRunning).
//
// The engine already knows this rule: PauseManager::PauseLogic (Xbox
// 0x8421B8F8, PC 0x00DAD1C0) asks GameFlowManager for the game mode
// ([[0x0201C0CC]+0x7C], 1 = multiplayer) and only outside a multiplayer game
// calls RWS::MainLoop::Logic::PushPause(1) (PC 0x0076FE30, cdecl(mask), ret)
// and counts it at +0x10; UnpauseLogic pairs it with PopPause (0x0076FE60).
// In a match the menu opens and the world goes on. The campaign runs in
// single-player mode, so one player's menu froze his whole world while the
// partner's kept running.
//
// PauseLogic is inlined into several PauseManager functions on PC (the pause
// menu, the system UI, the save kiosk), all in 0x00DA0000-0x00DC6000. Pushes
// from there are skipped while a partner is connected, and exactly as many
// pops from there are skipped after them, so the logic pause count stays
// balanced even if a partner leaves or joins with the menu open. Every other
// pause (loading, the debug menu) is left alone.
namespace pausefree {
bool enabled = false, installed = false;
constexpr uint32_t kPushPauseRva = 0x36FE30, kPopPauseRva = 0x36FE60;
constexpr uint32_t kManagerLo = 0x9A0000, kManagerHi = 0x9C6000;
safetyhook::InlineHook pushHook, popHook;
LONG skippedDepth = 0;
volatile LONG pushesSkipped = 0, popsSkipped = 0, pushesPassed = 0;

// The PC pause menu is not the PauseManager's: it is an Apt screen that
// pauses by itself once its fade-in timer runs out (0x00A63200: [screen+0x224]
// <= 0 and not yet paused -> PushPause(1), [screen+0x228] = 1) and pops when it
// closes (0x00A63160, PopPause at 0x00A631EB while +0x228). 25.09., 16:3x: the
// menu stopped both worlds, pushes_skipped=0: the PauseManager never paused.
constexpr uint32_t kScreenPushReturnRva = 0x663223, kScreenPopReturnRva = 0x6631F0;
// 25.09. evening: still paused, and the log named the pauser: 0x02805011 on
// the host, 0x02711011 on the client, inside the packer's sections (aYv,
// RVA 0x202D000-0x242D000, writable and executable, filled at run time; QuFIo
// behind it). The pause menu is protected code that runs from a stub whose
// address changes with every start, so no fixed return address can name it.
// A pause from there is the menu's.
constexpr uint32_t kPackerLo = 0x202D000, kPackerHi = 0x3350000;
bool FromPacker(uint32_t caller) { return caller >= base + kPackerLo && caller < base + kPackerHi; }
bool FromPauseManager(uint32_t caller) {
    return (caller >= base + kManagerLo && caller < base + kManagerHi) || caller == base + kScreenPushReturnRva || caller == base + kScreenPopReturnRva ||
           FromPacker(caller);
}

// The save kiosk is not a menu over a running world: SaveKiosk::BeginSave (0x00BAE040) links the
// kiosk to iMsgPausedTick ([0x0204D4C4], sent only while the logic is paused) and then calls
// PauseLogic; its update and input run from that tick alone. With the pause skipped the kiosk
// never ticked and the player stood frozen at it (02.11., and 14.10. until damage ended it).
// So a PauseLogic called from the kiosk pauses this world as in the campaign.
constexpr uint32_t kPauseLogicPushReturnRva = 0x9AD409, kKioskBeginSaveReturnRva = 0x7AE0B6, kKioskStopVfxReturnRva = 0x78B767;
volatile LONG kioskPausesPassed = 0, popsPassed = 0, popsUnmatched = 0;
// Pushes and pops from the PauseManager pair up last in, first out: a pop skips only when the push
// it closes was skipped. 02.11.: the count of skipped pushes stood at 2 and 3 for a whole session
// (pushes whose pops never came), and that count would have swallowed the pop of a real pause.
constexpr unsigned kStack = 32;
bool stack[kStack]{};
unsigned stackDepth = 0;
volatile LONG stackFull = 0;
void PushEntry(bool skipped) {
    if (stackDepth < kStack) stack[stackDepth++] = skipped;
    else InterlockedIncrement(&stackFull);
}
void __cdecl OnPushPause(uint32_t mask) {
    const auto caller = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    bool kiosk = false;
    if (caller == base + kPauseLogicPushReturnRva) {
        // PauseLogic: push esi / push 1 / call, so its own caller sits three slots up.
        const auto* frame = reinterpret_cast<const uint32_t*>(_AddressOfReturnAddress());
        const uint32_t outer = frame[3];
        kiosk = outer == base + kKioskBeginSaveReturnRva || outer == base + kKioskStopVfxReturnRva;
    }
    if (enabled && peerTable.Count() && FromPauseManager(caller) && !kiosk) {
        ++skippedDepth;
        PushEntry(true);
        InterlockedIncrement(&pushesSkipped);
        gate::Event("coop_pause the menu opened: the world keeps running for the partners (caller %08X)", caller);
        return;
    }
    if (FromPauseManager(caller)) PushEntry(false);
    if (kiosk && peerTable.Count() && InterlockedIncrement(&kioskPausesPassed) <= 20)
        gate::Event("coop_pause the save kiosk pauses this world (its menu runs on the paused tick); the partners play on");
    InterlockedIncrement(&pushesPassed);
    // Every other pauser once, so the next one that stops a world is named.
    static uint32_t seen[8]{};
    bool known = false;
    for (uint32_t c : seen) known |= c == caller;
    for (auto& c : seen) if (!known && !c) { c = caller; gate::Event("coop_pause a pause from %08X (mask %X) left alone", caller, mask); break; }
    pushHook.ccall<void>(mask);
}
void __cdecl OnPopPause(uint32_t mask) {
    const auto caller = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    if (FromPauseManager(caller)) {
        if (stackDepth) {
            if (stack[--stackDepth]) {
                if (skippedDepth > 0) --skippedDepth;
                InterlockedIncrement(&popsSkipped);
                return;
            }
            InterlockedIncrement(&popsPassed);
        } else {
            InterlockedIncrement(&popsUnmatched);
        }
    }
    popHook.ccall<void>(mask);
}

bool Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Coop", "PauseKeepsRunning", 0, ini.c_str()) != 0;
    if (enabled) {
        // mov ecx,[esp+4] / mov eax,0x0204D420 / mov edx,1
        static const unsigned char kPush[] = {0x8B, 0x4C, 0x24, 0x04, 0xB8, 0x20, 0xD4, 0x04, 0x02, 0xBA, 0x01, 0x00};
        // mov ecx,[esp+4] / mov eax,0x0204D424 / lea esp,[esp]
        static const unsigned char kPop[] = {0x8B, 0x4C, 0x24, 0x04, 0xB8, 0x24, 0xD4, 0x04, 0x02, 0x8D, 0xA4, 0x24};
        const bool a = hook_site::Install(log, "coop_pause Logic::PushPause", base + kPushPauseRva, kPush, sizeof(kPush),
                                          reinterpret_cast<void*>(OnPushPause), pushHook);
        const bool b = a && hook_site::Install(log, "coop_pause Logic::PopPause", base + kPopPauseRva, kPop, sizeof(kPop),
                                               reinterpret_cast<void*>(OnPopPause), popHook);
        if (a && !b) pushHook = {};   // never one without the other
        installed = a && b;
    }
    std::fprintf(log, "coop_pause keeps_running=%d installed=%d (the pause menu does not stop the world while a partner is connected)\n",
                 enabled ? 1 : 0, installed ? 1 : 0);
    return installed;
}

void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    const LONG n = InterlockedCompareExchange(&pushesSkipped, 0, 0) + InterlockedCompareExchange(&popsSkipped, 0, 0) + InterlockedCompareExchange(&kioskPausesPassed, 0, 0) * 1000;
    if (n == last) return;
    last = n;
    std::fprintf(log, "coop_pause pushes_skipped=%ld pops_skipped=%ld depth=%ld pushes_passed=%ld pops_passed=%ld pops_unmatched=%ld stack=%u stack_full=%ld "
                      "kiosk_pauses=%ld\n",
                 InterlockedCompareExchange(&pushesSkipped, 0, 0), InterlockedCompareExchange(&popsSkipped, 0, 0), skippedDepth,
                 InterlockedCompareExchange(&pushesPassed, 0, 0), InterlockedCompareExchange(&popsPassed, 0, 0), InterlockedCompareExchange(&popsUnmatched, 0, 0),
                 stackDepth, InterlockedCompareExchange(&stackFull, 0, 0), InterlockedCompareExchange(&kioskPausesPassed, 0, 0));
}
}  // namespace pausefree
