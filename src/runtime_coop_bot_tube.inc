// Dead Space 2 United by Hafer.
// Bots never crawl through vent tubes: following the leader through them was taken
// out (13.10.). A bot waits at the hatch and the catch-up puts it by him afterwards.
// The game would still let a bot in on its own: PlayerTubeCrawlingSM::CheckEnterTube
// (0x00EA1920, cdecl(player), returns al) takes the use press of a pickup next to a
// hatch, and TubeMarker::TryEnter would run the hatch's level messages (cull sets,
// checkpoints, the locks behind the leader) a second time. For a bot it says no.
constexpr uint32_t kCheckEnterTubeRva = 0xAA1920, kInputEvent = 0xFD4;
safetyhook::InlineHook tubeEnterHook;
bool tubeGuard = false;
volatile LONG tubeStrayBlocked = 0;
bool __cdecl OnCheckEnterTube(uint32_t player) {
    if (!UnitOf(player)) return tubeEnterHook.ccall<bool>(player);
    uint32_t event = 0;
    if (Read(player + kInputEvent, &event, 4) && event == 3) InterlockedIncrement(&tubeStrayBlocked);   // 3 = use
    return false;
}
void InstallTubeGuard(FILE* log) {
    // mov ecx,[esp+4] / push ebx / xor bl,bl / test byte [ecx+438h],1
    static const unsigned char kEnter[] = {0x8B, 0x4C, 0x24, 0x04, 0x53, 0x32, 0xDB, 0xF6, 0x81, 0x38, 0x04, 0x00, 0x00, 0x01};
    tubeGuard = hook_site::Install(log, "bot PlayerTubeCrawlingSM::CheckEnterTube", base + kCheckEnterTubeRva, kEnter, sizeof(kEnter),
                                   reinterpret_cast<void*>(OnCheckEnterTube), tubeEnterHook);
    std::fprintf(log, "bot tube_guard=%d (bots never crawl into a vent tube; they wait and are put by the leader after it)\n", tubeGuard ? 1 : 0);
}
// The same for every other usable. A bot presses use for a pickup, and
// PlayerUseObjectSM::CheckUseObjectStart (PC 0x00870C40, cdecl(Player*), returns al) then
// starts whatever usable the game offers next to it: lift switches, hacking stations,
// save kiosks (bot trigger audit 13.10.). Pickups go by the closest pickup (+0x40C), not
// this. A bot may start only the bench, the store, a locker, a power node door or a
// stasis station, the ones the bot module walks up to on purpose.
constexpr uint32_t kUseStartRva = 0x470C40, kOfferedUsable = 0x3F4, kUsableInOwner = 0x230;
safetyhook::InlineHook useStartHook;
bool useGuard = false;
volatile LONG useAllowed = 0, useRefused = 0, useRefusedLines = 0;
bool DeliberateUsable(uint32_t usable) {
    uint32_t usableVtable = 0, ownerVtable = 0;
    if (!usable || !Read(usable, &usableVtable, 4)) return false;
    if (usableVtable == base + kNodeSwitchUsableVtableRva || usableVtable == base + kStationUsableVtableRva) return true;
    if (usable < kUsableInOwner || !Read(usable - kUsableInOwner, &ownerVtable, 4)) return false;
    return ownerVtable == base + kBenchVtableRva || ownerVtable == base + kStoreVtableRva || ownerVtable == base + kStandardContainerVtableRva;
}
bool __cdecl OnUseStart(uint32_t player) {
    if (!UnitOf(player)) {
        uint32_t usable = 0;
        if (ghosttrigger::liftWait && Read(player + kOfferedUsable, &usable, 4) &&
            ghosttrigger::RefuseCabinButton(player, usable)) return false;
        if (uselock::RefuseLocal(player)) return false;
        const bool started = useStartHook.ccall<bool>(player);
        uselock::Started(player, started);
        return started;
    }
    uint32_t usable = 0;
    if (!Read(player + kOfferedUsable, &usable, 4) || !usable) return useStartHook.ccall<bool>(player);
    if (DeliberateUsable(usable)) {
        InterlockedIncrement(&useAllowed);
        return useStartHook.ccall<bool>(player);
    }
    InterlockedIncrement(&useRefused);
    if (InterlockedIncrement(&useRefusedLines) <= 20) {
        uint32_t usableVtable = 0, ownerVtable = 0;
        Read(usable, &usableVtable, 4);
        if (usable >= kUsableInOwner) Read(usable - kUsableInOwner, &ownerVtable, 4);
        gate::Event("bot: use of usable %08X refused (usable vtable rva %08X, owner +0x230 vtable rva %08X): not one the bot means to use", usable,
                    static_cast<unsigned>(usableVtable - base), static_cast<unsigned>(ownerVtable - base));
    }
    return false;
}
void InstallUseGuard(FILE* log) {
    // push esi / mov esi,[esp+8] / xor al,al / test byte [esi+438h],1
    static const unsigned char kUseSig[] = {0x56, 0x8B, 0x74, 0x24, 0x08, 0x32, 0xC0, 0xF6, 0x86, 0x38, 0x04, 0x00, 0x00, 0x01};
    useGuard = hook_site::Install(log, "bot PlayerUseObjectSM::CheckUseObjectStart", base + kUseStartRva, kUseSig, sizeof(kUseSig),
                                  reinterpret_cast<void*>(OnUseStart), useStartHook);
    std::fprintf(log, "bot use_guard=%d (a bot's use press starts only the bench, the store, a locker, a node door or a stasis station)\n", useGuard ? 1 : 0);
}
void WriteTubeDiagnostics(FILE* log) {
    std::fprintf(log, "bot tube_guard=%d stray_entries_blocked=%ld use_guard=%d uses_allowed=%ld uses_refused=%ld\n", tubeGuard ? 1 : 0,
                 InterlockedCompareExchange(&tubeStrayBlocked, 0, 0), useGuard ? 1 : 0, InterlockedCompareExchange(&useAllowed, 0, 0),
                 InterlockedCompareExchange(&useRefused, 0, 0));
}
