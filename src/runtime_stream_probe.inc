// Native stream ownership and bounded loading diagnostics.

ds2::GlobalStreamLifetime streamLifetime;
safetyhook::MidHook streamUpdateHook, streamDestroyHook;
safetyhook::MidHook streamLevelReleaseHook, streamServiceCreateHook;
safetyhook::MidHook streamServiceTeardownHook;
ds2::StreamTransitionGate streamTransition;
volatile LONG streamServiceEpoch = 0;
volatile LONG streamReleaseCalls = 0, streamReleaseUnlocks = 0, streamReleaseFailures = 0, streamResumes = 0;
volatile LONG streamReleaseHeld = 0, streamReleaseLate = 0, streamReleaseSwept = 0;
volatile LONG streamProbeCalls = 0, streamProbeResult = 0, streamProbeHandle = 0;
volatile LONG streamProbePrerequisite = -1;
DWORD streamProbeThread = 0, streamProbeTick = 0, streamResumeTick = 0;
unsigned streamProbeAttempts = 0;
// A request that never completes used to keep the pool locked for the rest of
// the run, because only the Waiting state was bounded. One sample per second,
// so 120 s.
unsigned streamLoadingAttempts = 0;
constexpr unsigned kStreamLoadingCeiling = 120;
// The private pool root is locked, so the native cleanup pass skips it. Its
// prerequisites are ordinary shared character streams and are not skipped, so a
// room change can free the state tables of NPCs built from the pool while
// those NPCs are still ticking. Pinning holds a reference on each verified
// prerequisite for as long as the root is held.
bool pinPrerequisites = false;
constexpr unsigned kMaxPinnedStreams = 64;
uint32_t pinnedStreams[kMaxPinnedStreams]{};
unsigned pinnedStreamCount = 0;
volatile LONG streamPinned = 0, streamPinFailures = 0, streamUnpinned = 0, streamUnpinFailures = 0;
bool streamProbeLocked = false;

// A skin's model stream (ds2_randomizer_<variant>) is a loose file like the
// pool and is requested by name the same way, right after the pool's own
// request, and given back with it. Listing it as a TOC dependency of the pool
// does not work: dependencies are resolved against the loaded TOCs only and
// the null came back as a crash (dump 45636).
struct ModelStreamState { uint32_t handle = 0; bool locked = false; int status = -1; };
// 32 slots: the 8 of 13.09. capped the request at eight streams, so the ninth
// skin onwards (GhostLeaper as the twelfth) attached the retail look (14.09.).
ModelStreamState modelStreams[32]{};
volatile LONG modelStreamsRequested = 0, modelStreamsLocked = 0, modelStreamsFailed = 0, modelStreamsReady = 0, modelStreamsReleased = 0;
constexpr unsigned ModelStreamCount() {
    unsigned n = 0;
    for (const char* name : ds2::kModelStreams) if (name) ++n;
    return n;
}
static_assert(ModelStreamCount() <= 32, "more model streams than request slots");

void RequestModelStreams(uint32_t manager) {
    using Load = uint32_t (__thiscall*)(void*, const char*, float, unsigned, const void*, const char*);
    using Lock = bool (__thiscall*)(void*, uint32_t);
    for (unsigned i = 0; i < ModelStreamCount() && i < std::size(modelStreams); ++i) {
        auto& state = modelStreams[i];
        if (state.handle) continue;
        state.handle = reinterpret_cast<Load>(base + 0x34BDD0)(reinterpret_cast<void*>(manager), ds2::kModelStreams[i], 0.0f, 2, nullptr, nullptr);
        InterlockedIncrement(&modelStreamsRequested);
        if (!state.handle) { InterlockedIncrement(&modelStreamsFailed); continue; }
        state.locked = reinterpret_cast<Lock>(base + 0x34AC80)(reinterpret_cast<void*>(manager), state.handle);
        if (state.locked) InterlockedIncrement(&modelStreamsLocked); else InterlockedIncrement(&modelStreamsFailed);
    }
}

void SampleModelStreams(uint32_t manager) {
    using Status = int (__thiscall*)(void*, uint32_t);
    for (unsigned i = 0; i < ModelStreamCount() && i < std::size(modelStreams); ++i) {
        auto& state = modelStreams[i];
        if (!state.handle) continue;
        const int status = reinterpret_cast<Status>(base + 0xDAA0)(reinterpret_cast<void*>(manager), state.handle);
        if (status == 3 && state.status != 3) InterlockedIncrement(&modelStreamsReady);
        state.status = status;
    }
}

void ReleaseModelStreams(uint32_t manager) {
    using Unlock = bool (__thiscall*)(void*, uint32_t);
    for (auto& state : modelStreams) {
        if (state.handle && state.locked && manager)
            reinterpret_cast<Unlock>(base + 0x34ACB0)(reinterpret_cast<void*>(manager), state.handle);
        if (state.handle) InterlockedIncrement(&modelStreamsReleased);
        state = ModelStreamState{};
    }
}
bool streamTargetsSampled = false;
bool streamGraphDispatched = false;
volatile LONG streamGraphMissing = -1;
volatile LONG streamSelectionAllowed = 0;
DWORD streamSelectionHoldoff = 0;

void UpdateSelectionGate(bool ready, DWORD now) {
    if (!ready) {
        streamSelectionHoldoff = now;
        InterlockedExchange(&streamSelectionAllowed, 0);
        return;
    }
    streamSelectionHoldoff = 0;
    InterlockedExchange(&streamSelectionAllowed, 1);
}

bool GlobalDonorAllowed(uint32_t handle) {
    uint32_t manager = 0, service = 0;
    if (!globalTemplates || !streamGraphDispatched || streamTransition.paused || !streamProbeLocked ||
        streamProbeThread != GetCurrentThreadId() ||
        !Read(base + 0x1C1C098, &manager, 4) || !Read(base + 0x1C1C0C8, &service, 4) || !service ||
        !streamLifetime.usable(manager)) return false;
    if (handle && handle != streamLifetime.handle) return false;
    using Status = int (__thiscall*)(void*, uint32_t);
    return reinterpret_cast<Status>(base + 0xDAA0)(reinterpret_cast<void*>(manager), streamLifetime.handle) == 3;
}

bool CheckStreamGraph(uint32_t manager) {
    const auto states = ds2::native::inspectStreamStates(manager, Read);
    if (states.result != ds2::native::StreamStates::Stable) {
        InterlockedExchange(&streamGraphMissing, -2); return false;
    }
    for (unsigned i = 0; i < std::size(ds2::kStreamRequirements); ++i) {
        const auto& requirement = ds2::kStreamRequirements[i];
        if (!ds2::native::containsDispatchedStream(states, requirement.identity, requirement.name, Read)) {
            InterlockedExchange(&streamGraphMissing, static_cast<LONG>(i)); return false;
        }
    }
    const auto root = std::find_if(states.entries.begin(), states.entries.end(),
        [](const auto& stream) { return stream.handle == streamLifetime.handle; });
    if (root == states.entries.end() || !ds2::native::dispatchedNotification(*root)) {
        InterlockedExchange(&streamGraphMissing, -3); return false;
    }
    InterlockedExchange(&streamGraphMissing, -1);
    return true;
}
void PinPrerequisiteStreams(uint32_t manager) {
    if (!pinPrerequisites || pinnedStreamCount || !manager) return;
    const auto states = ds2::native::inspectStreamStates(manager, Read);
    if (states.result != ds2::native::StreamStates::Stable) return;
    using Lock = bool (__thiscall*)(void*, uint32_t);
    const auto lock = reinterpret_cast<Lock>(base + 0x34AC80);
    for (const auto& requirement : ds2::kStreamRequirements) {
        if (pinnedStreamCount >= kMaxPinnedStreams) break;
        const auto handle = ds2::native::findDispatchedStream(states, requirement.identity, requirement.name, Read);
        if (!handle || handle == streamLifetime.handle) continue;
        if (lock(reinterpret_cast<void*>(manager), handle)) {
            pinnedStreams[pinnedStreamCount++] = handle;
            InterlockedIncrement(&streamPinned);
        } else {
            InterlockedIncrement(&streamPinFailures);
        }
    }
}

// Reverse order, mirroring the native dependency release.
void UnpinPrerequisiteStreams(uint32_t manager) {
    if (!manager) { pinnedStreamCount = 0; return; }
    using Unlock = bool (__thiscall*)(void*, uint32_t);
    const auto unlock = reinterpret_cast<Unlock>(base + 0x34ACB0);
    while (pinnedStreamCount) {
        const auto handle = pinnedStreams[--pinnedStreamCount];
        pinnedStreams[pinnedStreamCount] = 0;
        if (unlock(reinterpret_cast<void*>(manager), handle))
            InterlockedIncrement(&streamUnpinned);
        else
            InterlockedIncrement(&streamUnpinFailures);
    }
}

struct StreamTargetProbe {
    LONG ready = 0;
    unsigned index = 0, manager = 0, attributes = 0, components = 0, handle = 0;
    int status = -1;
    bool guidMatches = false;
};
StreamTargetProbe streamTargetProbes[136]{};
unsigned streamTargetProbeCount = 0;

void ProbeStreamTargets(uint32_t manager) {
    uint32_t sim = 0;
    if (!Read(base + 0x1C1C0A0, &sim, 4) || !sim) return;
    using Lookup = void* (__thiscall*)(void*, ds2::native::SpawnInfo32*, const Guid*, unsigned);
    using Status = int (__thiscall*)(void*, uint32_t);
    for (unsigned i = 0; i < std::size(ds2::kGlobalTargets); ++i) {
        if (streamTargetProbeCount >= std::size(streamTargetProbes)) break;
        auto& result = streamTargetProbes[streamTargetProbeCount++];
        const auto& target = ds2::kGlobalTargets[i];
        ds2::native::SpawnInfo32 packet{};
        reinterpret_cast<Lookup>(base + 0x376440)(reinterpret_cast<void*>(sim), &packet, &target.guid, 0);
        Guid resolved{};
        result.index = i;
        result.manager = manager;
        result.attributes = packet.attributes;
        result.components = packet.componentSet;
        result.handle = packet.field08;
        result.guidMatches = packet.record && packet.recordField0C &&
            Read(packet.recordField0C, resolved.data(), sizeof(resolved)) && resolved == target.guid;
        if (packet.field08)
            result.status = reinterpret_cast<Status>(base + 0xDAA0)(reinterpret_cast<void*>(manager), packet.field08);
        InterlockedExchange(&result.ready, 1);
    }
    streamTargetsSampled = true;
}

// The evening of 15.09.: sessions loaded the pool root without its TOC
// (registry stayed at 23, the root had no descriptor, no child was dispatched,
// every selection said global_pool_unavailable). The cause was a TOC rebuilt
// with a root name lacking the randomizer\ folder: the engine registers the
// TOC, fails the exact name lookup (0x748670), and drops the TOC again when
// its reference count reaches zero at the end of the load (0x74BFBD). The
// note below stays: it says at once whether the descriptor was found.
uint32_t FileSlotsUsed(uint32_t& slots) {
    uint32_t service = 0, slotArray = 0, used = 0;
    slots = 0;
    if (!Read(base + 0x1C1C090, &service, 4) || !service || !Read(service + 0x54, &slots, 4) ||
        !Read(service + 0x4C, &slotArray, 4) || slots > 64) { slots = 0; return 0; }
    for (uint32_t i = 0; i < slots; ++i) {
        unsigned char flag = 0;
        if (Read(slotArray + 16 * i + 4, &flag, 1) && flag) ++used;
    }
    return used;
}
volatile LONG fileSlotWaits = 0;
bool FileSlotsBusy() {
    uint32_t slots = 0;
    const uint32_t used = FileSlotsUsed(slots);
    // slot 0 is never handed out, so the real capacity is one less than `slots`
    if (slots && used + 2 >= slots) { InterlockedIncrement(&fileSlotWaits); return true; }
    return false;
}
char poolRequestNote[512]{};
volatile LONG poolRequestNoteReady = 0;
void NotePoolRequest(uint32_t manager, uint32_t handle, uint32_t tocsBefore) {
    const auto after = ds2::native::inspectRegistry(manager, Read);
    const auto found = ds2::native::findDescriptor(after, 0, ds2::kPoolStreamName, Read);
    char cwd[MAX_PATH]{};
    GetCurrentDirectoryA(MAX_PATH, cwd);
    const std::string toc = std::string(ds2::kPoolStreamName) + ".toc";
    const HANDLE probe = CreateFileA(toc.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, 0, nullptr);
    const DWORD openError = probe == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    const DWORD size = probe == INVALID_HANDLE_VALUE ? 0 : GetFileSize(probe, nullptr);
    if (probe != INVALID_HANDLE_VALUE) CloseHandle(probe);
    // The engine's TOC loader (0x749320) opens the file through its own file
    // service (0x746010, a table of 20 slots, first free slot wins, none free
    // means no TOC and no error), asks its size (0x7463E0) and closes it
    // (0x746120). Run the same three steps here on the same thread, so the
    // log says which of them the engine lost.
    uint32_t service = 0, slots = 0, engineOpen = 0, engineSize = 0, engineRead = 0, engineAlloc = 0, engineAllocator = 0;
    int engineRelocated = -1;
    unsigned char engineHead[16]{};
    const uint32_t slotsUsed = FileSlotsUsed(slots);
    if (found != ds2::native::DescriptorStatus::Found && Read(base + 0x1C1C090, &service, 4) && service && slots) {
        using Open = uint32_t (__thiscall*)(void*, const char*, int);
        using Size = uint32_t (__thiscall*)(void*, uint32_t);
        using Close = void (__thiscall*)(void*, uint32_t);
        engineOpen = reinterpret_cast<Open>(base + 0x346010)(reinterpret_cast<void*>(service), toc.c_str(), 1);
        if (engineOpen) {
            engineSize = reinterpret_cast<Size>(base + 0x3463E0)(reinterpret_cast<void*>(service), engineOpen);
            // 18:02: open and size succeed and the TOC still never registers.
            // The loader's next two steps are the read (0x7461C0, must return
            // the whole size) and the relocation (0x848CB0, must return true).
            // Both run here on a private page, nothing of the engine's changes.
            using ReadFile = uint32_t (__thiscall*)(void*, uint32_t, void*, uint32_t);
            using Relocate = bool (__thiscall*)(void*);
            if (engineSize >= 16 && engineSize <= 0x10000) {
                void* page = VirtualAlloc(nullptr, engineSize + 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (page) {
                    engineRead = reinterpret_cast<ReadFile>(base + 0x3461C0)(reinterpret_cast<void*>(service), engineOpen, page, engineSize);
                    if (engineRead == engineSize) engineRelocated = reinterpret_cast<Relocate>(base + 0x448CB0)(page) ? 1 : 0;
                    std::memcpy(engineHead, page, sizeof(engineHead));
                    VirtualFree(page, 0, MEM_RELEASE);
                }
            }
            reinterpret_cast<Close>(base + 0x346120)(reinterpret_cast<void*>(service), engineOpen);
            // The step between size and read is the loader's own allocation:
            // allocator 0x402667(), vtable slot 0 (size, params {2, 0x800, 0}),
            // freed through slot 1 (pointer, 0). A null there is the one
            // silent exit left in the loader.
            using Allocator = void* (__cdecl*)();
            using Allocate = void* (__thiscall*)(void*, uint32_t, void*);
            using Release = void (__thiscall*)(void*, void*, int);
            void* allocator = reinterpret_cast<Allocator>(base + 0x2667)();
            engineAllocator = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(allocator));
            if (allocator) {
                uint32_t params[3] = {2, 0x800, 0};
                void** vtable = *reinterpret_cast<void***>(allocator);
                void* block = reinterpret_cast<Allocate>(vtable[0])(allocator, engineSize, params);
                engineAlloc = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(block));
                if (block) reinterpret_cast<Release>(vtable[1])(allocator, block, 0);
            }
        }
    }
    std::snprintf(poolRequestNote, sizeof(poolRequestNote),
                  "pool_request handle=%08X tocs_before=%u tocs_after=%u registry=%s descriptor=%s cwd=%s toc_open_error=%lu toc_size=%lu"
                  " file_service=%08X slots_used=%u/%u engine_open=%u engine_size=%u engine_read=%u engine_relocated=%d engine_alloc=%08X allocator=%08X thread=%lu head=%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                  handle, tocsBefore, after.count, ds2::native::registryStatusName(after.status),
                  ds2::native::descriptorStatusName(found), cwd, openError, size, service, slotsUsed, slots, engineOpen, engineSize,
                  engineRead, engineRelocated, engineAlloc, engineAllocator, GetCurrentThreadId(), engineHead[0], engineHead[1], engineHead[2], engineHead[3], engineHead[4], engineHead[5], engineHead[6], engineHead[7],
                  engineHead[8], engineHead[9], engineHead[10], engineHead[11]);
    InterlockedExchange(&poolRequestNoteReady, 1);
}

bool VerifyStreamProbeFiles(FILE* log) {
    char executable[MAX_PATH]{};
    const DWORD executableLength = GetModuleFileNameA(nullptr, executable, MAX_PATH);
    if (!executableLength || executableLength >= MAX_PATH) return false;
    std::string directory(executable);
    directory.resize(directory.find_last_of("\\/") + 1);
    char current[MAX_PATH]{};
    const DWORD directoryLength = GetCurrentDirectoryA(MAX_PATH, current);
    if (!directoryLength || directoryLength >= MAX_PATH) return false;
    std::string currentDirectory(current);
    if (currentDirectory.back() != '\\') currentDirectory += '\\';
    if (_stricmp(currentDirectory.c_str(), directory.c_str())) {
        std::fprintf(log, "stream_probe working_directory_mismatch; relative native path rejected\n"); return false;
    }
    for (const auto& expected : ds2::kStreamProbeFiles) {
        std::ifstream file(directory + expected.name, std::ios::binary);
        uint64_t hash = 14695981039346656037ull, size = 0;
        char buffer[8192];
        while (file.read(buffer, sizeof(buffer)) || file.gcount()) {
            for (std::streamsize i = 0; i < file.gcount(); ++i)
                hash = (hash ^ static_cast<unsigned char>(buffer[i])) * 1099511628211ull;
            size += static_cast<uint64_t>(file.gcount());
            if (size > expected.size) break;
        }
        if (!file.eof() || size != expected.size || hash != expected.fingerprint) {
            std::fprintf(log, "stream_probe file_missing_or_mismatched name=%s bytes=%llu\n", expected.name, size);
            return false;
        }
    }
    return true;
}

void OnStreamServiceRetired(SafetyHookContext&) {
    InterlockedExchange(&streamSelectionAllowed, 0);
    streamSelectionHoldoff = 0;
    ClearHordeSilence();   // the enemies those counts belonged to are gone
    ResetExtraAreas();     // and the area rules may fire again in the next level
    uint32_t service = 0;
    if (Read(base + 0x1C1C0C8, &service, 4) && !service)
        InterlockedIncrement(&streamServiceEpoch);
    else
        InterlockedExchange(&streamProbeResult, 16);
}

// Release the pool root at the level's own release point. On.
//
// Three evenings' worth of dumps were spent moving this point around: level
// release, stream-manager destruction, and just before the streaming service is
// destroyed. Every one of them crashed somewhere else. None of it was the
// timing. The pool carried retail entity identities, so a level entity ended up
// bound to the pool stream and outlived the teardown that should have taken it.
//
// With unique identities in the pool this point works, which is also the point
// the engine uses for every stream of its own.
bool releasePoolAtLevelRelease = true;

// Never release the pool at all. Off, and it should stay off.
//
// It was written when every release point still crashed in the teardown of the
// pool stream, and it does remove the crash, by hanging instead. The transition
// waits for the streams to go, the locked root never does, and the loading
// screen stays up forever with the game ticking behind it.
//
// The crashes were the identity collision, not the timing: once every entity in
// the pool carried its own GUID, releasing at the level's own release point
// worked on the first try (releases=1 unlocked=1 unlock_failures=0 resumes=1,
// clean return to the main menu). See analysis/POOL_IDENTITY_COLLISION.md.
bool poolHoldForever = false;


// Everything the release does, from either of the two points below.
void ReleasePoolRoot(uint32_t manager) {
    if (poolHoldForever) {
        InterlockedExchange(&streamSelectionAllowed, 0);
        streamSelectionHoldoff = 0;
        InterlockedIncrement(&streamReleaseHeld);
        return;
    }
    InterlockedIncrement(&streamReleaseCalls);
    UnpinPrerequisiteStreams(manager);
    ReleaseModelStreams(manager);
    streamGraphDispatched = false;
    streamTransition.pause(static_cast<uint32_t>(InterlockedCompareExchange(&streamServiceEpoch, 0, 0)));
    streamLifetime.beginTeardown(manager);
    bool unlocked = !streamProbeLocked;
    if (streamProbeLocked) {
        using Unlock = bool (__thiscall*)(void*, uint32_t);
        unlocked = reinterpret_cast<Unlock>(base + 0x34ACB0)(reinterpret_cast<void*>(manager), streamLifetime.handle);
    }
    if (unlocked) {
        streamProbeLocked = false;
        InterlockedIncrement(&streamReleaseUnlocks);
    } else {
        InterlockedIncrement(&streamReleaseFailures);
    }
    streamTargetsSampled = false;
    streamSelectionHoldoff = 0;
    InterlockedExchange(&streamSelectionAllowed, 0);
    InterlockedExchange(&streamProbeResult, unlocked ? 13 : 14);
}

void OnLevelStreamRelease(SafetyHookContext& ctx) {
    const std::lock_guard<std::recursive_mutex> guard(streamProbeMutex);
    if (ctx.ecx != streamLifetime.manager || !streamLifetime.handle || streamTransition.paused) return;
    if (!releasePoolAtLevelRelease) {
        InterlockedExchange(&streamSelectionAllowed, 0);
        streamSelectionHoldoff = 0;
        InterlockedIncrement(&streamReleaseHeld);
        return;
    }
    ReleasePoolRoot(ctx.ecx);
    // The displaced native call releases the root and its dependency references.
}

// The late release point. It sits in the same teardown routine that destroys
// the streaming service, immediately before it does:
//
//   004138D6  mov ecx,[0201C0C8]     <- here
//   004138E6  call [service vtable]  ; the service is gone after this
//   00413904  call 00775EF0          ; SimManager cleanup, still asks for it
//
// That is the window the pool has to disappear in. Releasing it earlier, at the
// level's own release point, frees collision shapes under rigid bodies the
// level has not destroyed yet (analysis/CRASH_39616_MENU_HAVOK.md). Releasing
// it later, at stream-manager destruction, leaves pool entities to be cleaned
// up after the service they ask for is gone
// (analysis/CRASH_41152_MENU_TEARDOWN.md). Here the level is already down and
// both the manager and the service are still alive.
void OnServiceTeardown(SafetyHookContext&) {
    const std::lock_guard<std::recursive_mutex> guard(streamProbeMutex);
    if (!streamLifetime.manager || !streamLifetime.handle) return;
    if (!streamProbeLocked || streamTransition.paused) return;
    InterlockedIncrement(&streamReleaseLate);
    const uint32_t manager = streamLifetime.manager;
    ReleasePoolRoot(manager);
    // Unlocking only clears the skip flag. The level's release point suppressed
    // the native sweep for the pool root, so nothing has actually torn the stream
    // down, and its consumers still hold resource references that they hand
    // back through the streaming service when SimManager cleanup destroys them
    // at 0x00413904, by which time the service is gone. Run the same sweep the
    // level release runs, now that the flag is clear:
    //
    //   0074B8D0  walks the manager's two stream lists and releases every
    //             record whose bit 7 of [record+0x54] is clear, through
    //             0074B060(manager, record, 0)
    //
    // At this point the level's own pass has already been through, so the only
    // record left for it to find is the pool's.
    using Sweep = void (__thiscall*)(void*);
    reinterpret_cast<Sweep>(base + 0x34B8D0)(reinterpret_cast<void*>(manager));
    InterlockedIncrement(&streamReleaseSwept);
}

void OnStreamDestroy(SafetyHookContext& ctx) {
    const std::lock_guard<std::recursive_mutex> guard(streamProbeMutex);
    if (ctx.ecx != streamLifetime.manager) return;
    UnpinPrerequisiteStreams(ctx.ecx);
    ReleaseModelStreams(poolHoldForever ? 0 : ctx.ecx);
    streamGraphDispatched = false;
    streamLifetime.beginTeardown(ctx.ecx);
    if (streamProbeLocked && streamLifetime.handle && !poolHoldForever) {
        using Unlock = bool (__thiscall*)(void*, uint32_t);
        reinterpret_cast<Unlock>(base + 0x34ACB0)(reinterpret_cast<void*>(ctx.ecx), streamLifetime.handle);
    }
    streamProbeLocked = false;
    streamLifetime.finishTeardown();
    streamTransition.reset();
    streamProbeThread = 0;
    streamProbeAttempts = 0;
    streamLoadingAttempts = 0;
    streamTargetsSampled = false;
    streamSelectionHoldoff = 0;
    InterlockedExchange(&streamSelectionAllowed, 0);
    InterlockedExchange(&streamProbeResult, 9);
    InterlockedExchange(&streamProbeHandle, 0);
}

void OnStreamUpdate(SafetyHookContext& ctx) {
    const std::lock_guard<std::recursive_mutex> guard(streamProbeMutex);
    InterlockedIncrement(&streamProbeCalls);
    uint32_t manager = 0;
    if (!Read(base + 0x1C1C098, &manager, 4) || !manager || manager != ctx.ecx) return;
    if (streamLifetime.manager && streamLifetime.manager != manager) {
        InterlockedExchange(&streamProbeResult, 12); return;
    }
    const DWORD thread = GetCurrentThreadId();
    if (streamProbeThread && streamProbeThread != thread) {
        InterlockedExchange(&streamProbeResult, 10);
        return;
    }
    streamProbeThread = thread;
    if (streamTransition.paused) {
        uint32_t service = 0;
        const auto epoch = static_cast<uint32_t>(InterlockedCompareExchange(&streamServiceEpoch, 0, 0));
        if (epoch == streamTransition.epoch) {
            InterlockedExchange(&streamProbeResult, 17); return;
        }
        if (!Read(base + 0x1C1C0C8, &service, 4) || !service) {
            InterlockedExchange(&streamProbeResult, 18); return;
        }
        const DWORD resumeNow = GetTickCount();
        if (resumeNow - streamResumeTick < 1000) return;
        streamResumeTick = resumeNow;
        const auto registry = ds2::native::inspectRegistry(manager, Read);
        const bool absent = registry.status == ds2::native::RegistryStatus::Stable &&
            ds2::native::findDescriptor(registry, 0, ds2::kPoolStreamName, Read) == ds2::native::DescriptorStatus::Missing;
        if (streamProbeLocked || !streamTransition.canResume(epoch, service != 0, absent)) {
            InterlockedExchange(&streamProbeResult, 15); return;
        }
        streamLifetime.finishTeardown();
        streamTransition.reset();
        streamProbeAttempts = 0;
        streamLoadingAttempts = 0;
        streamProbeTick = 0;
        streamSelectionHoldoff = 0;
        InterlockedExchange(&streamSelectionAllowed, 0);
        InterlockedExchange(&streamProbeHandle, 0);
        InterlockedIncrement(&streamResumes);
    }
    if (!streamLifetime.attach(manager)) return;
    const auto now = GetTickCount();
    if (now - streamProbeTick < 1000) return;
    streamProbeTick = now;
    using State = ds2::GlobalStreamLifetime::State;
    if (streamLifetime.state == State::Waiting) {
        UpdateSelectionGate(false, now);
        if (++streamProbeAttempts > 120) { InterlockedExchange(&streamProbeResult, 11); return; }
        const auto registry = ds2::native::inspectRegistry(manager, Read);
        if (registry.status != ds2::native::RegistryStatus::Stable || registry.count >= 32) {
            InterlockedExchange(&streamProbeResult, 1); return;
        }
        for (unsigned i = 0; i < std::size(ds2::kStreamRequirements); ++i) {
            const auto& prerequisite = ds2::kStreamRequirements[i];
            const auto result = ds2::native::findDescriptor(registry, prerequisite.identity, prerequisite.name, Read);
            if (result != ds2::native::DescriptorStatus::Found) {
                InterlockedExchange(&streamProbePrerequisite, static_cast<LONG>(i * 16 + static_cast<unsigned>(result)));
                InterlockedExchange(&streamProbeResult, 2); return;
            }
        }
        InterlockedExchange(&streamProbePrerequisite, -1);
        if (ds2::native::findDescriptor(registry, 0, ds2::kPoolStreamName, Read) !=
            ds2::native::DescriptorStatus::Missing) {
            InterlockedExchange(&streamProbeResult, 3); return;
        }
        const auto after = ds2::native::inspectRegistry(manager, Read);
        if (after.status != ds2::native::RegistryStatus::Stable || after.tocs != registry.tocs) return;
        if (!streamLifetime.beginRequest()) return;
        const auto generation = streamLifetime.generation;
        using Load = uint32_t (__thiscall*)(void*, const char*, float, unsigned, const void*, const char*);
        const auto handle = reinterpret_cast<Load>(base + 0x34BDD0)(reinterpret_cast<void*>(manager),
            ds2::kPoolStreamName, 0.0f, 2, nullptr, nullptr);
        if (!streamLifetime.acceptHandle(generation, handle)) {
            InterlockedExchange(&streamProbeResult, 4); return;
        }
        InterlockedExchange(&streamProbeHandle, static_cast<LONG>(handle));
        NotePoolRequest(manager, handle, registry.count);
        using Lock = bool (__thiscall*)(void*, uint32_t);
        streamProbeLocked = reinterpret_cast<Lock>(base + 0x34AC80)(reinterpret_cast<void*>(manager), handle);
        if (!streamProbeLocked) {
            streamLifetime.sample(manager, handle, -1, false);
            InterlockedExchange(&streamProbeResult, 5); return;
        }
        RequestModelStreams(manager);
        InterlockedExchange(&streamProbeResult, 6);
    }
    if (streamLifetime.state == State::Loading || streamLifetime.state == State::Ready) {
        using Status = int (__thiscall*)(void*, uint32_t);
        const int status = reinterpret_cast<Status>(base + 0xDAA0)(reinterpret_cast<void*>(manager), streamLifetime.handle);
        streamGraphDispatched = status == 3 && CheckStreamGraph(manager);
        if (streamGraphDispatched) PinPrerequisiteStreams(manager);
        SampleModelStreams(manager);
        UpdateSelectionGate(status == 3 && streamGraphDispatched, now);
        streamLifetime.sample(manager, streamLifetime.handle, status, streamGraphDispatched);
        if (status == 3 && !streamTargetsSampled) ProbeStreamTargets(manager);
        if (status == 3) {
            streamLoadingAttempts = 0;
        } else if (++streamLoadingAttempts > kStreamLoadingCeiling) {
            // Give the lock back rather than hold a level hostage to the pool request.
            ReleaseModelStreams(manager);
            if (streamProbeLocked && streamLifetime.handle) {
                using Unlock = bool (__thiscall*)(void*, uint32_t);
                if (reinterpret_cast<Unlock>(base + 0x34ACB0)(reinterpret_cast<void*>(manager), streamLifetime.handle))
                    InterlockedIncrement(&streamReleaseUnlocks);
                else
                    InterlockedIncrement(&streamReleaseFailures);
            }
            streamProbeLocked = false;
            UnpinPrerequisiteStreams(manager);
            streamGraphDispatched = false;
            UpdateSelectionGate(false, now);
            streamLifetime.sample(manager, streamLifetime.handle, -1, false);
            InterlockedExchange(&streamProbeResult, 20);
            return;
        }
        InterlockedExchange(&streamProbeResult, streamGraphDispatched ? 19 : status == 3 ? 7 :
            streamLifetime.state == State::Failed ? 8 : 6);
    }
}

// The graph verdict without waiting for the next stream update.
//
// OnStreamUpdate runs when the game updates its stream manager, and the game
// does that only while it has streaming work: 30 to 150 times a second on a
// loading screen, then once every 20 to 30 seconds during play (measured over
// three runs on 2026-09-15). With twelve forced packages the pool's own stream
// was ready 3.7 s into the level but the dependency check still wanted one
// more pass, and that pass came 140 s later; the loading hold ran its full 30
// seconds for nothing and 26 spawns went unreplaced. This runs the same check
// from the per-frame hooks (loading hold, game thread tick), on the probe's
// own thread only, and only while the verdict is "dispatched, unverified".
volatile LONG streamRecheckCalls = 0, streamRecheckHits = 0, streamRecheckThreadMismatch = 0;
DWORD streamRecheckTick = 0;

void RecheckStreamGraph() {
    if (InterlockedCompareExchange(&streamProbeResult, 0, 0) != 7) return;
    if (!streamProbeThread || streamProbeThread != GetCurrentThreadId()) {
        InterlockedIncrement(&streamRecheckThreadMismatch);
        return;
    }
    std::unique_lock<std::recursive_mutex> guard(streamProbeMutex, std::try_to_lock);
    if (!guard.owns_lock()) return;
    const DWORD now = GetTickCount();
    if (now - streamRecheckTick < 250) return;
    streamRecheckTick = now;
    InterlockedIncrement(&streamRecheckCalls);
    uint32_t manager = 0;
    if (!Read(base + 0x1C1C098, &manager, 4) || !manager || !streamLifetime.usable(manager) ||
        streamTransition.paused || !streamLifetime.handle) return;
    using State = ds2::GlobalStreamLifetime::State;
    if (streamLifetime.state != State::Loading && streamLifetime.state != State::Ready) return;
    using Status = int (__thiscall*)(void*, uint32_t);
    const int status = reinterpret_cast<Status>(base + 0xDAA0)(reinterpret_cast<void*>(manager), streamLifetime.handle);
    if (status != 3 || !CheckStreamGraph(manager)) return;
    streamGraphDispatched = true;
    PinPrerequisiteStreams(manager);
    SampleModelStreams(manager);
    UpdateSelectionGate(true, now);
    streamLifetime.sample(manager, streamLifetime.handle, status, true);
    if (!streamTargetsSampled) ProbeStreamTargets(manager);
    streamLoadingAttempts = 0;
    InterlockedIncrement(&streamRecheckHits);
    InterlockedExchange(&streamProbeResult, 19);
}

constexpr uint32_t kStreamUpdateRva = 0x34BBC0, kStreamLoadRva = 0x34BDD0;
constexpr char kStreamUpdateBytes[] = "\x83\xEC\x08\x53\x8B\xD9\xE8\x45\xCF\x0C\x00";

bool InstallStreamProbe(FILE* log) {
    if (!VerifyStreamProbeFiles(log)) return false;
    // Held through the check and the install, so a co-op hook is never seen half written.
    hook_site::Lock siteLock(static_cast<uint32_t>(base + kStreamUpdateRva));
    hook_site::Lock loadLock(static_cast<uint32_t>(base + kStreamLoadRva));
    const struct { uint32_t rva; const char* bytes; size_t size; } signatures[] = {
        {0x10686, "\x8B\x0D\x98\xC0\x01\x02\xE8\x3F\xB2\x33\x00", 11},
        {0x138E8, "\x8B\x0D\x28\x13\x02\x02\xE8\xAD\xC1\x39\x00", 11},
        {0x138D6, "\x8B\x0D\xC8\xC0\x01\x02\x85\xC9\x74\x08", 10},
        {0x34B8D0, "\x56\x57\x8B\xF9\x8B\x47\x7C\x85\xC0\x74\x22", 11},
        {kStreamUpdateRva, kStreamUpdateBytes, sizeof(kStreamUpdateBytes) - 1},
        {0x34BC90, "\x83\xEC\x1C\x56\x8B\xF1\x33\xC9", 8},
        {kStreamLoadRva, "\x81\xEC\x24\x01\x00\x00\x53\x55\x8B\xE9", 10},
        {0xDAA0, "\x8B\x44\x24\x04\x50\xE8\x91\x68\xFF\xFF", 10},
        {0x34AC80, "\x8B\x44\x24\x04\x53\x50\x32\xDB\xE8\xAE\x96\xCB\xFF", 13},
        {0x34ACB0, "\x8B\x44\x24\x04\x53\x50\x32\xDB\xE8\x7E\x96\xCB\xFF", 13},
        {0x376440, "\x8B\x44\x24\x0C\x8B\x54\x24\x08\x56\x50\x52\xE8\xC0\xFE\xFF\xFF", 16}
    };
    // Function entries may carry another plugin's hook: the stream update is
    // hooked by the co-op leaper (this hook chains behind it), and Load, which
    // is only called here, by DS2Coop's render stall probe (the call runs
    // through it). Until 05.10. any such hook made the probe give up the pool.
    const uint32_t entries[] = {kStreamUpdateRva, kStreamLoadRva, 0x34B8D0, 0xDAA0, 0x34AC80, 0x34ACB0, 0x376440};
    for (const auto& signature : signatures) {
        unsigned char actual[16]{};
        const auto bytes = reinterpret_cast<const unsigned char*>(signature.bytes);
        const bool entry = std::find(std::begin(entries), std::end(entries), signature.rva) != std::end(entries);
        const auto state = Read(base + signature.rva, actual, signature.size)
            ? hook_site::Inspect(actual, bytes, signature.size) : hook_site::State::Foreign;
        if (state == hook_site::State::Foreign || (state == hook_site::State::Hooked && !entry)) {
            std::fprintf(log, "stream_probe signature_mismatch rva=%08X\n", signature.rva); return false;
        }
        if (state == hook_site::State::Hooked)
            std::fprintf(log, "stream_probe rva=%08X carries another plugin's hook; calling through it\n", signature.rva);
    }
    streamServiceCreateHook = safetyhook::create_mid(reinterpret_cast<void*>(base + 0x138E8), OnStreamServiceRetired);
    if (!streamServiceCreateHook) return false;
    streamLevelReleaseHook = safetyhook::create_mid(reinterpret_cast<void*>(base + 0x1068C), OnLevelStreamRelease);
    if (!streamLevelReleaseHook) { streamServiceCreateHook.reset(); return false; }
    streamDestroyHook = safetyhook::create_mid(reinterpret_cast<void*>(base + 0x34BC90), OnStreamDestroy);
    if (!streamDestroyHook) {
        streamLevelReleaseHook.reset(); streamServiceCreateHook.reset(); return false;
    }
    streamServiceTeardownHook = safetyhook::create_mid(reinterpret_cast<void*>(base + 0x138D6), OnServiceTeardown);
    if (!streamServiceTeardownHook) {
        streamDestroyHook.reset(); streamLevelReleaseHook.reset(); streamServiceCreateHook.reset(); return false;
    }
    if (!hook_site::InstallMid(log, "stream_probe stream update", base + kStreamUpdateRva,
                               reinterpret_cast<const unsigned char*>(kStreamUpdateBytes), sizeof(kStreamUpdateBytes) - 1,
                               OnStreamUpdate, streamUpdateHook)) {
        streamServiceTeardownHook.reset(); streamDestroyHook.reset();
        streamLevelReleaseHook.reset(); streamServiceCreateHook.reset(); return false;
    }
    std::fprintf(log, "stream_probe installed; global_selection=%d requires_owned_dispatched_graph=1 asset_behavior=unverified\n", globalTemplates);
    return true;
}
