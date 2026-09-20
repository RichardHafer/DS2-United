#pragma once
// A hook site two plugins both want.
//
// The randomizer's frame cap and another plugin's frame tick wrap the same
// per-frame call at 0x3701C0. Each waits for the unpacked image, then installs
// at the same moment as the other. On 2026-09-17 that race went both ways in
// one four-instance run: in two processes the other plugin won and the frame
// cap refused ("signature_mismatch"), in the other two the randomizer won and
// the other plugin's hook failed outright, which left those instances without
// its per-frame driver.
//
// Two rules fix it. The site is taken under a mutex, so the second plugin
// sees a finished hook and not a half-written one. And a finished hook is not
// a reason to refuse: a jmp rel32 is exactly what safetyhook relocates into
// its trampoline, so the second plugin chains behind the first and both run.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../third_party/safetyhook/safetyhook.hpp"

namespace hook_site {
// Held while a site is inspected and hooked. Both plugins live in one process,
// so the name only has to be unique per process and per site.
struct Lock {
    HANDLE handle = nullptr;
    bool held = false;
    explicit Lock(uint32_t rva) {
        char name[64];
        std::snprintf(name, sizeof(name), "Local\\DS2HookSite_%08X_%08X",
                      static_cast<unsigned>(GetCurrentProcessId()), static_cast<unsigned>(rva));
        handle = CreateMutexA(nullptr, FALSE, name);
        if (handle) held = WaitForSingleObject(handle, 10000) == WAIT_OBJECT_0;
    }
    ~Lock() {
        if (held) ReleaseMutex(handle);
        if (handle) CloseHandle(handle);
    }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};

enum class State { Pristine, Hooked, Foreign };

// What is at the site: the retail bytes, somebody's jmp rel32 followed by the
// retail tail (safe to chain behind), or something else.
inline State Inspect(const unsigned char* actual, const unsigned char* expected, size_t size) {
    if (std::memcmp(actual, expected, size) == 0) return State::Pristine;
    if (size > 5 && actual[0] == 0xE9 && std::memcmp(actual + 5, expected + 5, size - 5) == 0)
        return State::Hooked;
    return State::Foreign;
}

inline const char* StateName(State s) {
    switch (s) {
    case State::Pristine: return "pristine";
    case State::Hooked: return "hooked (chaining behind it)";
    default: return "foreign bytes";
    }
}

inline const char* ErrorName(const safetyhook::InlineHook::Error& e) {
    using E = safetyhook::InlineHook::Error;
    switch (e.type) {
    case E::BAD_ALLOCATION: return "bad_allocation";
    case E::FAILED_TO_DECODE_INSTRUCTION: return "failed_to_decode_instruction";
    case E::SHORT_JUMP_IN_TRAMPOLINE: return "short_jump_in_trampoline";
    case E::IP_RELATIVE_INSTRUCTION_OUT_OF_RANGE: return "ip_relative_out_of_range";
    case E::UNSUPPORTED_INSTRUCTION_IN_TRAMPOLINE: return "unsupported_instruction";
    case E::FAILED_TO_UNPROTECT: return "failed_to_unprotect";
    case E::NOT_ENOUGH_SPACE: return "not_enough_space";
    default: return "?";
    }
}

// Install with the site locked. Retries a handful of times, because the one
// failure seen so far was transient (the other plugin mid-install); every
// failure is logged with safetyhook's own reason rather than a bare "failed".
inline bool Install(FILE* log, const char* who, uintptr_t address, const unsigned char* expected,
                    size_t size, void* destination, safetyhook::InlineHook& hook) {
    Lock lock(static_cast<uint32_t>(address));
    unsigned char actual[64]{};
    if (size > sizeof(actual)) size = sizeof(actual);
    for (unsigned attempt = 0; attempt < 20; ++attempt) {
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), actual, size, &got) ||
            got != size) {
            std::fprintf(log, "%s site %08X unreadable\n", who, static_cast<unsigned>(address));
            return false;
        }
        const State state = Inspect(actual, expected, size);
        if (state == State::Foreign) {
            std::fprintf(log, "%s site %08X has foreign bytes %02X %02X %02X %02X %02X; not installed\n",
                         who, static_cast<unsigned>(address), actual[0], actual[1], actual[2], actual[3], actual[4]);
            return false;
        }
        auto result = safetyhook::InlineHook::create(reinterpret_cast<void*>(address), destination);
        if (result) {
            hook = std::move(*result);
            std::fprintf(log, "%s installed at %08X (site was %s%s)\n", who, static_cast<unsigned>(address),
                         StateName(state), attempt ? ", after retries" : "");
            return true;
        }
        std::fprintf(log, "%s hook attempt %u at %08X failed: %s\n", who, attempt + 1,
                     static_cast<unsigned>(address), ErrorName(result.error()));
        std::fflush(log);
        Sleep(50);
    }
    return false;
}
}  // namespace hook_site
