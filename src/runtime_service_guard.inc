// Dead Space 2 United by Richard Hafer.
// Returning a resource handle after the streaming service is gone.
//
// The level-to-menu teardown destroys the streaming service first and cleans up
// the simulation afterwards:
//
//   004138D6  mov ecx,[0201C0C8]     ; the streaming service
//   004138E6  call [service vtable]  ; destroyed here
//   00413904  call 00775EF0          ; SimManager cleanup runs after it
//
// Anything the cleanup destroys that still holds a resource handle hands it back
// through that service, and the service is a null pointer by then. The read
// faults at 0x00DB64F0 on address 0x30, which is `[ecx+0x18]` with ecx zero.
// Five dumps in this session end exactly there, the last one without a single
// enemy replacement in the whole run, so it is not the replacement machinery.
//
// 0x00DE7010 is the release itself, and it is small enough to reason about:
//
//   push esi / mov esi,ecx          ; the holder
//   mov eax,[esi+8]                 ; its handle
//   mov edx,[esi]
//   mov edi,[0201C0C8]              ; the service
//   push eax / mov eax,[edx+0x14] / call eax
//   push eax / mov ecx,edi / call 00DE5670   ; hand the handle back
//   mov dword ptr [esi+8], 0FFFFFFFFh        ; mark it released
//   ret
//
// With no service there is nothing to hand the handle back to, and the table it
// would be removed from has been destroyed with it. The guard therefore does
// what the tail of the function does and marks the holder released, which is the
// state the caller expects. It can only ever fire in the window between the
// service being destroyed and the process leaving that teardown, because the
// pointer is non-null at every other time.
#include <windows.h>
#include <cstdio>
#include "../third_party/safetyhook/safetyhook.hpp"

namespace ds2_service_guard {
constexpr uintptr_t kReleaseRva = 0x9E7010;
constexpr uintptr_t kServiceRva = 0x1C1C0C8;
const unsigned char kReleaseBytes[] = {0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x08, 0x8B, 0x16, 0x57,
                                       0x8B, 0x3D, 0xC8, 0xC0, 0x01, 0x02};

uintptr_t moduleBase = 0;
safetyhook::InlineHook releaseHook;
volatile LONG skipped = 0, passed = 0;

void __fastcall OnRelease(void* self, void*) {
    uint32_t service = 0;
    SIZE_T done = 0;
    const bool read = ReadProcessMemory(GetCurrentProcess(),
        reinterpret_cast<void*>(moduleBase + kServiceRva), &service, 4, &done) && done == 4;
    if (read && !service && self) {
        InterlockedIncrement(&skipped);
        *reinterpret_cast<volatile uint32_t*>(static_cast<char*>(self) + 8) = 0xFFFFFFFFu;
        return;
    }
    InterlockedIncrement(&passed);
    releaseHook.thiscall<void>(self);
}

bool Install(uintptr_t base, const char* ini, FILE* log) {
    if (releaseHook) return true;
    if (GetPrivateProfileIntA("DS2Randomizer", "GuardServiceRelease", 1, ini) == 0) {
        std::fprintf(log, "service_guard disabled by configuration\n");
        return true;
    }
    moduleBase = base;
    unsigned char actual[sizeof(kReleaseBytes)]{};
    SIZE_T done = 0;
    const bool matched = ReadProcessMemory(GetCurrentProcess(),
        reinterpret_cast<void*>(base + kReleaseRva), actual, sizeof(actual), &done) &&
        done == sizeof(actual) && std::memcmp(actual, kReleaseBytes, sizeof(actual)) == 0;
    std::fprintf(log, "service_guard signature: rva=%08X %s\n",
                 static_cast<unsigned>(kReleaseRva), matched ? "matched" : "MISMATCH");
    if (!matched) return false;
    releaseHook = safetyhook::create_inline(reinterpret_cast<void*>(base + kReleaseRva),
                                            reinterpret_cast<void*>(OnRelease));
    std::fprintf(log, "service_guard hook: %s; skips the handle return only while the service is null\n",
                 releaseHook ? "installed" : "FAILED");
    return static_cast<bool>(releaseHook);
}

void WriteDiagnostics(FILE* log) {
    static LONG last = -1;
    const LONG total = InterlockedCompareExchange(&skipped, 0, 0);
    if (!releaseHook || total == last) return;
    last = total;
    std::fprintf(log, "service_guard skipped=%ld passed=%ld\n", total,
                 InterlockedCompareExchange(&passed, 0, 0));
}
}  // namespace ds2_service_guard
