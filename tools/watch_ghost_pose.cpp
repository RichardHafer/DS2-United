// x86 diagnostic for the retail DS2 window thread. Run with the client PID.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

static_assert(sizeof(void*) == 4, "Build with the x86 toolchain");
constexpr DWORD image = 0x00400000, attach = image + 0xAB4E10;
HANDLE process;
DWORD processId, windowThread;

bool read(DWORD address, void* out, size_t size) {
    SIZE_T done = 0;
    return ReadProcessMemory(process, reinterpret_cast<void*>(address), out, size, &done) && done == size;
}
DWORD word(DWORD address) {
    DWORD result = 0;
    read(address, &result, sizeof(result));
    return result;
}
BOOL CALLBACK findWindow(HWND window, LPARAM) {
    DWORD owner = 0;
    const DWORD thread = GetWindowThreadProcessId(window, &owner);
    if (owner != processId || !IsWindowVisible(window) || GetWindow(window, GW_OWNER)) return TRUE;
    windowThread = thread;
    return FALSE;
}
struct Watch { DWORD player, buffer, hits; bool sawFreeLink; };
Watch watches[3]{};
unsigned nextSlot = 0;

void onDetach(CONTEXT& c) {
    DWORD args[2]{};
    if (!read(c.Esp + 4, args, sizeof(args)) || !args[0] || args[1]) return;
    const DWORD player = args[0], proxy = word(player + 0x68);
    if (word(player) != image + 0x17294D0 || !proxy || !(word(proxy + 0x30) & 2)) return;
    const DWORD buffer = word(player + 0x1E0), count = word(player + 0x1E4);
    if (!buffer || (buffer & 15) || !count || count > 132) return;
    unsigned slot = 0;
    while (slot < 3 && watches[slot].buffer != buffer) ++slot;
    if (slot == 3) slot = nextSlot++ % 3;
    watches[slot] = {player, buffer, 0, false};
    if (slot == 0) c.Dr1 = buffer;
    if (slot == 1) c.Dr2 = buffer;
    if (slot == 2) c.Dr3 = buffer;
    const unsigned reg = slot + 1;
    c.Dr7 |= (1u << (2 * reg)) | (13u << (16 + 4 * reg)); // Four-byte write.
    std::printf("detach tick=%lu player=%08lX buffer=%08lX count=%lu slot=%u\n",
                GetTickCount(), player, buffer, count, reg);
}
bool onWrite(const CONTEXT& c, unsigned slot) {
    auto& w = watches[slot];
    ++w.hits;
    const DWORD value = word(w.buffer);
    // Preserve the allocator links and the following writes even when the buffer is reused.
    if (c.Eip >= 0x013FC000 && c.Eip < 0x013FE000 && value > 0x10000 && !(value & 3))
        w.sawFreeLink = true;
    const bool suspect = w.sawFreeLink && (value & 0x7fffffff) == 0x7fc00000;
    if (w.hits > 80 && !suspect) return false;
    DWORD stack[8]{};
    read(c.Esp, stack, sizeof(stack));
    MEMORY_BASIC_INFORMATION region{};
    VirtualQueryEx(process, reinterpret_cast<const void*>(c.Eip), &region, sizeof(region));
    unsigned char code[16]{};
    read(c.Eip - sizeof(code), code, sizeof(code));
    std::printf("write tick=%lu slot=%u hit=%lu buf=%08lX value=%08lX next_eip=%08lX esp=%08lX stack=",
                GetTickCount(), slot + 1, w.hits, w.buffer, value, c.Eip, c.Esp);
    for (DWORD v : stack) std::printf(" %08lX", v);
    std::printf(" base=%p preceding_bytes=", region.AllocationBase);
    for (unsigned char b : code) std::printf("%02X", b);
    std::puts("");
    if (suspect) std::puts("NaN write after allocator link observed; ending capture");
    return suspect;
}
int main(int argc, char** argv) {
    if (argc != 2 || !(processId = std::strtoul(argv[1], nullptr, 10))) return 2;
    process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, processId);
    wchar_t path[MAX_PATH]{};
    DWORD len = MAX_PATH;
    if (!process || !QueryFullProcessImageNameW(process, 0, path, &len)) return 2;
    const wchar_t* file = std::wcsrchr(path, L'\\');
    if (!file || _wcsicmp(file + 1, L"deadspace2_4gb.exe")) return 2;
    const unsigned char expected[] = {0x8b,0x74,0x24,0x40,0x33,0xdb,0x39,0x9e,0x88,0x16,0,0};
    unsigned char actual[sizeof(expected)]{};
    if (!read(attach + 5, actual, sizeof(actual)) || std::memcmp(actual, expected, sizeof(actual))) return 2;
    EnumWindows(findWindow, 0);
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, windowThread);
    if (!thread || !DebugActiveProcess(processId)) return 3;
    if (!DebugSetProcessKillOnExit(FALSE)) { DebugActiveProcessStop(processId); return 3; }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("watch pid=%lu window_thread=%lu attach=%08lX\n", processId, windowThread, attach);
    bool armed = false, attachBreak = true, stop = false;
    while (!stop) {
        DEBUG_EVENT e{};
        if (!WaitForDebugEvent(&e, 1000)) {
            if (GetLastError() == ERROR_SEM_TIMEOUT) continue;
            break;
        }
        DWORD status = DBG_CONTINUE;
        if (e.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
            if (e.u.CreateProcessInfo.hFile) CloseHandle(e.u.CreateProcessInfo.hFile);
            CONTEXT c{};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(thread, &c) && !(c.Dr7 & 0xff)) {
                c.Dr0 = attach; c.Dr7 = 1; c.Dr6 = 0;
                armed = SetThreadContext(thread, &c) != FALSE;
            }
            if (!armed) stop = true;
        } else if (e.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
            if (e.u.LoadDll.hFile) CloseHandle(e.u.LoadDll.hFile);
        } else if (e.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            const DWORD code = e.u.Exception.ExceptionRecord.ExceptionCode;
            status = DBG_EXCEPTION_NOT_HANDLED;
            if (code == EXCEPTION_BREAKPOINT && attachBreak) { attachBreak = false; status = DBG_CONTINUE; }
            if (code == EXCEPTION_SINGLE_STEP && e.dwThreadId == windowThread && armed) {
                CONTEXT c{};
                c.ContextFlags = CONTEXT_CONTROL | CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(thread, &c) && (c.Dr6 & 15)) {
                    if (c.Dr6 & 1) { onDetach(c); c.EFlags |= 0x10000; }
                    for (unsigned s = 0; s < 3; ++s) if (c.Dr6 & (2u << s)) {
                        if (onWrite(c, s)) stop = true;
                        if (watches[s].hits >= 160) c.Dr7 &= ~(1u << (2 * (s + 1)));
                    }
                    c.Dr6 = 0;
                    if (stop) c.Dr7 = 0;
                    if (SetThreadContext(thread, &c)) status = DBG_CONTINUE;
                }
            }
        } else if (e.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) stop = true;
        ContinueDebugEvent(e.dwProcessId, e.dwThreadId, status);
    }
    DebugActiveProcessStop(processId);
    CloseHandle(thread);
    CloseHandle(process);
    return armed ? 0 : 3;
}
