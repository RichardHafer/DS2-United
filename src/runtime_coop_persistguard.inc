// Dead Space 2 United by Hafer.
// Persistence manager after the world is gone.
//
// Crash family 0x00DB64F0 (dumps 42544 and 58180 of 22.09., 60844, 62708 and
// 10436; the last one from the debug menu's checkpoint selector): the world shutdown at
// 0x00413800 destroys the persistent-item manager ([0x0201C0C8], its
// destructor clears the pointer at 0x00DE716C) and only THEN flushes the
// SimManager's pending deletes (0x00413904 -> 0x00775EF0). Each object deleted
// there gets iMsgNotifyDeleteEntity, and ISpawned::HandleEvents (0x00DE9E40)
// removes its persistent record through the manager without a null check.
// In the three dumps still on disk the object was a global LogEntry (global_assets, model
// 0BFBC6C6, the log the player's log storage keeps) holding record 1, and
// ISpawned::RemovePersistentRecord (0x00DE7010) called into this == 0.
//
// The manager methods ISpawned uses, all thiscall with this = [0x0201C0C8]:
//   0x00DE55E0 create record (2 args, ret 8, returns the record id)
//   0x00DE5670 remove record (2 args, ret 8, result unused)
//   0x00DE1A40 update record (3 args, ret 0xC, result unused)
// With the manager gone there is no record to create, remove or update: the
// call is skipped and "no record" (-1) returned.
//
// The guard alone was not enough (dump 1404): 47 record calls were skipped,
// then a Leaper prop from the plugin's locked character stream died in the same flush
// on [0x0201C0A4] (221 users in the game, not guardable one by one). The
// flush at 0x00413904 is SimManager::DeleteAllEntities (0x00775EF0): it
// deletes every entity still alive, after the shutdown's managers are gone,
// and it starts with the same reset broadcast MainLoop::Reset sends, so
// calling it earlier, at 0x00413800, faulted too (dump 15288): [0x0201C0A4]
// is already gone there. Between GameMainLoop::CloseImpl's entry and
// 0x00413800 only its reset broadcast (0x0040EF24) and a buffer free
// (0x007AA1C0) run, so the reset is where it goes (not seen directly: it may
// even go before CloseImpl). In the retail game nothing is left for the
// flush; what was left were the plugin's locked streams (MP pool, sixth-type record and
// character stream: 185 entities on stream 0xB). So at CloseImpl's entry
// (0x0040EF00, thiscall, no arguments, plain ret), before the reset, the necro
// module destroys those streams' entities, unlocks and unloads them.
namespace persistguard {
bool installed = false;
constexpr uint32_t kCreateRva = 0x9E55E0, kRemoveRva = 0x9E5670, kUpdateRva = 0x9E1A40;
constexpr uint32_t kCloseImplRva = 0xEF00;
safetyhook::InlineHook createHook, removeHook, updateHook, closeHook;
volatile LONG createSkipped = 0, removeSkipped = 0, updateSkipped = 0, closes = 0;

void __fastcall OnCloseImpl(void* self, void*) {
    InterlockedIncrement(&closes);
    necro::OnWorldClose();
    closeHook.thiscall<void>(self);
}

uint32_t __fastcall OnCreate(void* self, void*, uint32_t a, uint32_t b) {
    if (!self) { InterlockedIncrement(&createSkipped); return 0xFFFFFFFFu; }
    return createHook.thiscall<uint32_t>(self, a, b);
}
uint32_t __fastcall OnRemove(void* self, void*, uint32_t a, uint32_t b) {
    if (!self) { InterlockedIncrement(&removeSkipped); return 0; }
    return removeHook.thiscall<uint32_t>(self, a, b);
}
uint32_t __fastcall OnUpdate(void* self, void*, uint32_t a, uint32_t b, uint32_t c) {
    if (!self) { InterlockedIncrement(&updateSkipped); return 0; }
    return updateHook.thiscall<uint32_t>(self, a, b, c);
}

bool Install(FILE* log, const std::string& ini) {
    if (GetPrivateProfileIntA("Coop", "PersistenceTeardownGuard", 1, ini.c_str()) == 0) {
        std::fprintf(log, "coop_persistguard off\n");
        return false;
    }
    // sub esp,0x24 / push ebx / push esi / mov esi,[esp+0x30] / push edi / push esi / mov ebx,ecx
    static const unsigned char kCreate[] = {0x83, 0xEC, 0x24, 0x53, 0x56, 0x8B, 0x74, 0x24, 0x30, 0x57, 0x56, 0x8B, 0xD9};
    // sub esp,0x20 / mov eax,[esp+0x24] / mov edx,[eax] / push ebx / mov [esp+0xC],edx / mov edx,[eax+4]
    static const unsigned char kRecord[] = {0x83, 0xEC, 0x20, 0x8B, 0x44, 0x24, 0x24, 0x8B, 0x10, 0x53, 0x89, 0x54, 0x24, 0x0C, 0x8B, 0x50, 0x04};
    const bool a = hook_site::Install(log, "coop_persistguard create record", base + kCreateRva, kCreate, sizeof(kCreate),
                                      reinterpret_cast<void*>(OnCreate), createHook);
    const bool b = hook_site::Install(log, "coop_persistguard remove record", base + kRemoveRva, kRecord, sizeof(kRecord),
                                      reinterpret_cast<void*>(OnRemove), removeHook);
    const bool c = hook_site::Install(log, "coop_persistguard update record", base + kUpdateRva, kRecord, sizeof(kRecord),
                                      reinterpret_cast<void*>(OnUpdate), updateHook);
    // fldz / mov eax,[0x01AA5D00] / sub esp,0x14
    static const unsigned char kClose[] = {0xD9, 0xEE, 0xA1, 0x00, 0x5D, 0xAA, 0x01, 0x83, 0xEC, 0x14};
    const bool d = GetPrivateProfileIntA("Coop", "WorldCloseDropsStreams", 1, ini.c_str()) != 0 &&
                   hook_site::Install(log, "coop_persistguard GameMainLoop::CloseImpl", base + kCloseImplRva, kClose, sizeof(kClose),
                                      reinterpret_cast<void*>(OnCloseImpl), closeHook);
    installed = a || b || c || d;
    std::fprintf(log, "coop_persistguard create=%d remove=%d update=%d world_close_drops_streams=%d (our streams leave before the world's managers; records are left alone once their manager is gone)\n",
                 a ? 1 : 0, b ? 1 : 0, c ? 1 : 0, d ? 1 : 0);
    return installed;
}

void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    const LONG total = InterlockedCompareExchange(&createSkipped, 0, 0) + InterlockedCompareExchange(&removeSkipped, 0, 0) +
                       InterlockedCompareExchange(&updateSkipped, 0, 0) + InterlockedCompareExchange(&closes, 0, 0);
    if (total == last) return;
    last = total;
    std::fprintf(log, "coop_persistguard world_closes=%ld skipped create=%ld remove=%ld update=%ld (calls into the destroyed manager)\n",
                 InterlockedCompareExchange(&closes, 0, 0),
                 InterlockedCompareExchange(&createSkipped, 0, 0), InterlockedCompareExchange(&removeSkipped, 0, 0),
                 InterlockedCompareExchange(&updateSkipped, 0, 0));
}
}  // namespace persistguard

// A level load that waits on an audio thread which is not running.
//
// 23.09., client hung after following the host's checkpoint pick (live, PID
// 34904): the main thread sat in the loop at 0x00772D10, which waits until
// the audio command buffer ([0x0202EB70], executed by 0x0053C5D0, counter
// +0x100) has been run once more, sleeping 1 ms per turn through 0x00462340.
// It had been queued from a stream-loaded notice (global_assets) during the
// load, but the audio thread was stopped: [0x0202CA83] = 0, 0x538 bytes
// pending, counter frozen at 33532. Nothing would ever run it. The engine's
// own flush (0x0053D660) runs that buffer on the calling thread, so while the
// audio thread is stopped this wait does the same instead of sleeping.
// Names from the Xbox build (11.10.): the loop is MultiTrackStream::Streams::
// InitVoices (IsCommandComplete / ThreadSleep), 0x0053C5D0 is rw::audio::core::
// System::ExecuteCommands and 0x0053D660 System::Release. The INI key and the
// log prefix keep their old "render" names.
namespace renderstall {
constexpr uint32_t kSleepRva = 0x62340, kWaitReturnRva = 0x372D22, kRenderRunningRva = 0x1C2CA83,
                   kCommandBufferRva = 0x1C2EB70, kExecuteRva = 0x13C5D0;
safetyhook::InlineHook sleepHook;
bool installed = false;
volatile LONG flushed = 0, faults = 0;
// Evidence for the stall itself (24.09.: it comes without the necro pool too,
// dump 27536). Every StreamManager::Load (0x0074BDD0, thiscall(name, float,
// unsigned, const void*, const char*), ret 0x14) made while no level is up is
// logged with its caller, and when the flush runs, its caller chain and every
// stream not yet loaded are logged once per stall. That names who brings
// global_assets back between the two closes of a checkpoint load.
constexpr uint32_t kStreamLoadRva = 0x34BDD0, kStreamManagerRva = 0x1C1C098;
safetyhook::InlineHook loadHook;
volatile LONG loadsLogged = 0, stallsLogged = 0;
bool StreamNameOf(uint32_t stream, char* out, size_t size) {
    uint32_t descriptor = 0, identity = 0, name = 0;
    out[0] = 0;
    if (!Read(stream + 0x18, &descriptor, 4) || !descriptor || !Read(descriptor, &identity, 4) || !identity ||
        !Read(identity + 4, &name, 4) || !name) return false;
    char buffer[96]{};
    if (!Read(name, buffer, sizeof(buffer) - 1)) return false;
    std::snprintf(out, size, "%s", buffer);
    return true;
}
uint32_t __fastcall OnStreamLoad(void* manager, void*, const char* name, float priority, unsigned flags, const void* a4, const char* a5) {
    const uint32_t caller = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    const uint32_t handle = loadHook.thiscall<uint32_t>(manager, name, priority, flags, a4, a5);
    if ((!inLevel || !levelSettled) && InterlockedIncrement(&loadsLogged) <= 160) {
        char copy[96]{};
        if (name) Read(reinterpret_cast<uintptr_t>(name), copy, sizeof(copy) - 1);
        gate::Event("coop_renderstall load_probe: %s flags=%X handle=%08X caller=%08X in_level=%d settled=%d", copy, flags, handle, caller,
                    inLevel ? 1 : 0, levelSettled ? 1 : 0);
    }
    return handle;
}
void LogStall() {
    if (InterlockedIncrement(&stallsLogged) > 4) return;
    // Return addresses into the game on this stack, innermost first.
    uint32_t frame[16]{};
    unsigned count = 0;
    const uint32_t* stack = reinterpret_cast<const uint32_t*>(_AddressOfReturnAddress());
    for (unsigned i = 0; i < 0x800 && count < 16; ++i) {
        uint32_t value = 0;
        if (!Read(reinterpret_cast<uintptr_t>(stack + i), &value, 4)) break;
        if (value < base + 0x1000 || value >= base + 0x1200000) continue;
        unsigned char before[6]{};
        if (!Read(value - 6, before, 6)) continue;
        if (before[1] == 0xE8 || before[4] == 0xFF || before[0] == 0xFF || before[3] == 0xFF) frame[count++] = value;
    }
    char chain[16 * 9 + 1] = "";
    for (unsigned i = 0; i < count; ++i) std::snprintf(chain + i * 9, 10, " %08X", frame[i]);
    gate::Event("coop_renderstall stall callers:%s", chain);
    uint32_t manager = 0, table = 0, buckets = 0;
    if (!Read(base + kStreamManagerRva, &manager, 4) || !manager || !Read(manager + 0x5C, &table, 4) || !Read(manager + 0x60, &buckets, 4)) return;
    unsigned listed = 0;
    for (uint32_t b = 0; b < buckets && b < 4096 && listed < 24; ++b) {
        uint32_t node = 0;
        Read(table + 4 * b, &node, 4);
        for (unsigned hops = 0; node && hops < 64 && listed < 24; ++hops) {
            uint32_t handle = 0, stream = 0, status = 0, flags = 0;
            Read(node, &handle, 4);
            Read(node + 4, &stream, 4);
            if (stream && Read(stream + 0x24, &status, 4) && status != 3) {
                Read(stream + 0x54, &flags, 4);
                char name[96];
                StreamNameOf(stream, name, sizeof(name));
                gate::Event("coop_renderstall stall stream handle=%X status=%u flags=%04X %s", handle, status, flags, name);
                ++listed;
            }
            if (!Read(node + 8, &node, 4)) break;
        }
    }
}
void __cdecl OnSleep(void* handle) {
    if (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress())) == base + kWaitReturnRva) {
        unsigned char running = 1;
        uint32_t buffer = 0;
        if (Read(base + kRenderRunningRva, &running, 1) && !running && Read(base + kCommandBufferRva, &buffer, 4) && buffer) {
            __try {
                reinterpret_cast<void(__thiscall*)(void*)>(base + kExecuteRva)(reinterpret_cast<void*>(buffer));
                if (InterlockedIncrement(&flushed) == 1)
                    gate::Event("coop_renderstall: a load waited on the stopped render thread; its command buffer was run on the main thread");
                LogStall();
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                InterlockedIncrement(&faults);
            }
        }
    }
    sleepHook.ccall<void>(handle);
}
bool Install(FILE* log, const std::string& ini) {
    if (GetPrivateProfileIntA("Coop", "RenderStallFlush", 1, ini.c_str()) == 0) {
        std::fprintf(log, "coop_renderstall off\n");
        return false;
    }
    // mov eax,[esp+4] / mov ecx,[eax] / push 1 / push ecx / call [imp]
    static const unsigned char kSleep[] = {0x8B, 0x44, 0x24, 0x04, 0x8B, 0x08, 0x6A, 0x01, 0x51, 0xFF, 0x15};
    // push ecx / push ebx / push ebp / push esi / mov esi,ecx / mov eax,[esi+0x54]
    static const unsigned char kExecute[] = {0x51, 0x53, 0x55, 0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x54};
    // lea edx,[esp+0x14] / push edx / mov dword [esp+0x18],1 / call 0x00462340
    static const unsigned char kWaitCall[] = {0xE8};
    unsigned char execute[sizeof(kExecute)]{}, call[1]{};
    int32_t rel = 0;
    const bool siteOk = Read(base + kExecuteRva, execute, sizeof(execute)) && std::memcmp(execute, kExecute, sizeof(kExecute)) == 0 &&
                        Read(base + kWaitReturnRva - 5, call, 1) && std::memcmp(call, kWaitCall, 1) == 0 &&
                        Read(base + kWaitReturnRva - 4, &rel, 4) && base + kWaitReturnRva + rel == base + kSleepRva;
    if (!siteOk) {
        std::fprintf(log, "coop_renderstall signatures do not match; not installed\n");
        return false;
    }
    installed = hook_site::Install(log, "coop_renderstall sleep", base + kSleepRva, kSleep, sizeof(kSleep),
                                   reinterpret_cast<void*>(OnSleep), sleepHook);
    std::fprintf(log, "coop_renderstall installed=%d (a load waiting on a stopped render thread runs the buffer itself)\n", installed ? 1 : 0);
    if (GetPrivateProfileIntA("Coop", "StreamLoadProbe", 0, ini.c_str()) != 0) {
        // sub esp,0x124 / push ebx / push ebp / mov ebp,ecx
        static const unsigned char kLoad[] = {0x81, 0xEC, 0x24, 0x01, 0x00, 0x00, 0x53, 0x55, 0x8B, 0xE9};
        const bool probe = hook_site::Install(log, "coop_renderstall load_probe", base + kStreamLoadRva, kLoad, sizeof(kLoad),
                                              reinterpret_cast<void*>(OnStreamLoad), loadHook);
        std::fprintf(log, "coop_renderstall load_probe=%d (stream requests while no level is up are logged)\n", probe ? 1 : 0);
    }
    return installed;
}
void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    const LONG now = InterlockedCompareExchange(&flushed, 0, 0) + InterlockedCompareExchange(&faults, 0, 0);
    if (now == last) return;
    last = now;
    std::fprintf(log, "coop_renderstall flushed=%ld faults=%ld\n", InterlockedCompareExchange(&flushed, 0, 0), InterlockedCompareExchange(&faults, 0, 0));
}
}  // namespace renderstall
