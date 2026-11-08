// Dead Space 2 United by Richard Hafer.
// DS2ModLoader.asi: loose-file mods for Dead Space 2, without touching the .DAT archives.
//
// How the game finds a file (read from the 2026-09-19 dump, rw::core::filesystem):
//
//   The filesystem Manager (instance at [0x0202F1DC]) keeps a SEARCH PATH, a
//   ';'-separated list at +0x2C (buffer, size at +0x34 = 0x400), parsed into a
//   table at +0x3C of {path, device} pairs (capacity at +0x38 = 4, null
//   terminated). At start-up (Manager init 0x0056C560) it is set to
//
//       "big:;<game directory>"
//
//   i.e. the DS2DATn.DAT archives ("big:" device) first, then the game folder.
//   File::Open (0x0056DAE0) walks that table in order and takes the first
//   device that opens the file, so a loose copy of an archived file in the
//   game folder is never seen. A NEW file (the randomizer's pool .str) is,
//   because "big:" has no such name and the walk continues to the folder.
//
//   The Manager has the API for this (identical to the one Greavesy uses in
//   GF2ASI for The Godfather II, same EA tech):
//     Manager::AddSearchLocation(const char* location, int flags)  0x0056C2E0, thiscall, ret 8
//       flags != 0: prepend ("location;" + list), else append; then
//     Manager::SetSearchPath(const char* list)                    0x0056C210, thiscall, ret 4
//       copies the list into +0x2C, splits at ';', fills the table at +0x3C
//       WITHOUT a bounds check against +0x38 (4 entries), so the table and
//       the buffer are enlarged first, through the game's own allocator
//       ([0x0202F1E0]->Alloc, the one the Manager frees them with).
//
// So this plugin, once the Manager exists, puts every enabled mod folder in
// front, so the search path reads
//
//       "<game>\mods\A;<game>\mods\B;big:;<game>"
//
// and a file at mods\A\<path as inside the archive> wins over the archive. Any
// file the game opens through the filesystem (.str, .toc, .sbk, .lua, ...).
//
// It runs on its own thread while the game may open files. AddSearchLocation
// edits the live buffer in place (the NULs back to ';') and SetSearchPath then
// zeroes buffer and table before refilling them, so a File::Open on the game or
// a streaming thread could walk a half-written table. SetSearchPath touches
// only this+0x2C/0x34/0x38/0x3C and the device lookup 0x0056C080 (28.09. dump),
// so it fills new storage through a stand-in Manager, and the finished table
// is swapped in by single pointer writes. The old storage stays allocated for
// a reader still walking it.
//
// Mods: every sub-folder of <game>\mods, alphabetical, unless mods\order.txt
// lists them (one per line, first = highest priority); a folder whose name
// starts with '-' is skipped. INI: plugins\DS2ModLoader.ini [Mods].
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <share.h>
#include <string>
#include <vector>

namespace {
uintptr_t base = 0;
FILE* logFile = nullptr;

constexpr uintptr_t kManagerPtrRva = 0x1C2F1DC;      // rw::core::filesystem::Manager*
constexpr uintptr_t kAllocatorPtrRva = 0x1C2F1E0;    // the allocator the Manager uses
constexpr uintptr_t kAddSearchLocationRva = 0x16C2E0;
constexpr uintptr_t kSetSearchPathRva = 0x16C210;
constexpr uintptr_t kAllocTagRva = 0x16F8D5C;        // "rw::core::filesystem::Manager::Allocate"
constexpr uint32_t kBuffer = 0x2C, kBufferSize = 0x34, kCapacity = 0x38, kTable = 0x3C;
constexpr uint32_t kNewBufferSize = 8192, kNewCapacity = 128;

bool Read(uintptr_t address, void* out, size_t bytes) {
    SIZE_T done = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), out, bytes, &done) && done == bytes;
}
uint32_t U32(uintptr_t address) { uint32_t v = 0; Read(address, &v, 4); return v; }

void Log(const char* format, ...) {
    if (!logFile) return;
    va_list args;
    va_start(args, format);
    std::vfprintf(logFile, format, args);
    va_end(args);
    std::fputc('\n', logFile);
    std::fflush(logFile);
}

std::string PluginDirectory() {
    char path[MAX_PATH]{};
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&PluginDirectory), &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    std::string directory(path);
    const auto slash = directory.find_last_of('\\');
    return slash == std::string::npos ? std::string(".") : directory.substr(0, slash);
}
// The game gets UTF-8 paths, as before; the plugin's own file calls go through
// the wide API, so a folder outside the ANSI code page is still found.
std::wstring Wide(const std::string& utf8) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (n <= 1) return std::wstring();
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, out.data(), n);
    return out;
}
std::string Utf8(const wchar_t* text) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), n, nullptr, nullptr);
    return out;
}
// A NUL-terminated string in game memory, read in small pieces so one near the
// end of a region is still read.
bool ReadCString(uintptr_t address, std::string& out, size_t max) {
    out.clear();
    char chunk[64];
    while (out.size() < max) {
        if (!Read(address + out.size(), chunk, sizeof(chunk))) {
            char c = 0;
            if (!Read(address + out.size(), &c, 1)) return false;
            if (!c) return true;
            out += c;
            continue;
        }
        const void* nul = std::memchr(chunk, 0, sizeof(chunk));
        if (nul) { out.append(chunk, static_cast<size_t>(static_cast<const char*>(nul) - chunk)); return true; }
        out.append(chunk, sizeof(chunk));
    }
    return false;
}
std::string GameDirectoryUtf8() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring directory(path);
    const auto slash = directory.find_last_of(L'\\');
    if (slash != std::wstring::npos) directory.resize(slash);
    char utf8[MAX_PATH * 3]{};
    WideCharToMultiByte(CP_UTF8, 0, directory.c_str(), -1, utf8, sizeof(utf8), nullptr, nullptr);
    return utf8;
}

// The packed exe: retail bytes appear only once the packer has run (same
// probe as the co-op plugin, a well-known site).
bool UnpackedImage() {
    constexpr unsigned char expected[] = {
        0xE8,0x8B,0xB1,0x8A,0xFF,0x8B,0x35,0,0,0,0,
        0x68,0,0x20,0,0,0x8D,0x4C,0x24,0x34,0x51,0x8B,0xCB,
        0xC6,0x46,0x28,0,0xE8,0xB0,0xB1,0x8A,0xFF,
        0x8B,0xD8,0x89,0x5C,0x24,0x10,0xC6,0x46,0x28,1
    };
    unsigned char actual[sizeof(expected)]{};
    if (!Read(base + 0xACB2B0, actual, sizeof(actual))) return false;
    for (size_t i = 0; i < sizeof(actual); ++i) {
        if (i >= 7 && i < 11) continue;
        if (actual[i] != expected[i]) return false;
    }
    return true;
}

bool SignaturesMatch() {
    // push ebp / mov ebp,esp / push ecx / push ebx / push esi / push edi / mov edi,ecx / mov eax,[edi+2Ch]
    static const unsigned char kAdd[] = {0x55,0x8B,0xEC,0x51,0x53,0x56,0x57,0x8B,0xF9,0x8B,0x47,0x2C};
    // push ebx / push ebp / push esi / push edi / mov edi,ecx / mov eax,[edi+34h] / mov ecx,[edi+2Ch]
    static const unsigned char kSet[] = {0x53,0x55,0x56,0x57,0x8B,0xF9,0x8B,0x47,0x34,0x8B,0x4F,0x2C};
    unsigned char a[sizeof(kAdd)]{}, s[sizeof(kSet)]{};
    if (!Read(base + kAddSearchLocationRva, a, sizeof(a)) || std::memcmp(a, kAdd, sizeof(kAdd)) != 0) { Log("AddSearchLocation signature mismatch at %08X", static_cast<unsigned>(base + kAddSearchLocationRva)); return false; }
    if (!Read(base + kSetSearchPathRva, s, sizeof(s)) || std::memcmp(s, kSet, sizeof(kSet)) != 0) { Log("SetSearchPath signature mismatch at %08X", static_cast<unsigned>(base + kSetSearchPathRva)); return false; }
    return true;
}

// The Manager is ready once its table holds the "big:" entry the game put there.
bool ManagerReady(uintptr_t& manager) {
    manager = U32(base + kManagerPtrRva);
    if (!manager) return false;
    const uint32_t table = U32(manager + kTable), buffer = U32(manager + kBuffer);
    if (!table || !buffer) return false;
    char first[8]{};
    const uint32_t entry0 = U32(table);
    return entry0 && Read(entry0, first, 5) && std::strncmp(first, "big:", 4) == 0;
}

// The current entries, read off the table (the buffer holds them NUL-separated).
std::vector<std::string> SearchEntries(uintptr_t manager, bool& complete) {
    std::vector<std::string> out;
    complete = true;
    const uint32_t table = U32(manager + kTable), capacity = U32(manager + kCapacity);
    for (uint32_t i = 0; i < capacity && i < kNewCapacity; ++i) {
        const uint32_t entry = U32(table + i * 8);
        if (!entry) break;
        std::string text;
        if (!ReadCString(entry, text, 1024)) { complete = false; break; }
        out.push_back(text);
    }
    return out;
}
std::string SearchPath(uintptr_t manager) {
    bool complete = false;
    std::string out;
    for (const auto& entry : SearchEntries(manager, complete)) {
        if (!out.empty()) out += ";";
        out += entry;
    }
    if (!complete) out += " (unreadable entry)";
    return out;
}

bool Store(uintptr_t address, uint32_t value) {
    __try {
        InterlockedExchange(reinterpret_cast<volatile LONG*>(address), static_cast<LONG>(value));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool FillStandIn(unsigned char* standIn, const char* list) {
    using SetPath = void(__thiscall*)(void*, const char*);
    __try {
        reinterpret_cast<SetPath>(base + kSetSearchPathRva)(standIn, list);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The whole list (front entries, then the game's own) in new storage from the
// game's allocator, so the Manager's shutdown frees it like its own.
bool Publish(uintptr_t manager, const std::vector<std::string>& front, const std::vector<std::string>& old) {
    std::string list;
    for (const auto& entry : front) { list += entry; list += ';'; }
    for (size_t i = 0; i < old.size(); ++i) { if (i) list += ';'; list += old[i]; }
    if (front.size() + old.size() >= kNewCapacity || list.size() + 1 > kNewBufferSize) {
        Log("search path too long (%u entries, %u bytes); nothing changed", static_cast<unsigned>(front.size() + old.size()),
            static_cast<unsigned>(list.size() + 1));
        return false;
    }
    const uint32_t allocator = U32(base + kAllocatorPtrRva);
    if (!allocator) { Log("no allocator"); return false; }
    using Alloc = void* (__thiscall*)(void*, uint32_t size, const char* tag, uint32_t, uint32_t align, uint32_t);
    const uint32_t vtable = U32(allocator);
    const auto alloc = reinterpret_cast<Alloc>(U32(vtable + 4));
    const char* tag = reinterpret_cast<const char*>(base + kAllocTagRva);
    void* buffer = alloc(reinterpret_cast<void*>(allocator), kNewBufferSize, tag, 0, 4, 0);
    void* table = alloc(reinterpret_cast<void*>(allocator), kNewCapacity * 8, tag, 0, 4, 0);
    if (!buffer || !table) { Log("allocation failed"); return false; }
    const uint32_t newBuffer = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(buffer));
    const uint32_t newTable = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(table));
    alignas(4) unsigned char standIn[0x40]{};
    std::memcpy(standIn + kBuffer, &newBuffer, 4);
    std::memcpy(standIn + kBufferSize, &kNewBufferSize, 4);
    std::memcpy(standIn + kCapacity, &kNewCapacity, 4);
    std::memcpy(standIn + kTable, &newTable, 4);
    if (!FillStandIn(standIn, list.c_str())) { Log("SetSearchPath faulted on the new storage; nothing changed"); return false; }
    unsigned filled = 0;
    const uint32_t* entries = static_cast<const uint32_t*>(table);
    while (filled < kNewCapacity && entries[filled * 2]) ++filled;
    if (filled != front.size() + old.size()) {
        Log("new table holds %u entries, %u expected; nothing changed", filled, static_cast<unsigned>(front.size() + old.size()));
        return false;
    }
    // A reader bounded by the capacity must never step past a table's end: the
    // old table, if it has a NUL entry within its capacity, takes the larger
    // capacity first; otherwise the larger table goes in first.
    const bool oldTerminated = old.size() < U32(manager + kCapacity);
    bool ok = true;
    if (oldTerminated) ok = Store(manager + kCapacity, kNewCapacity) && Store(manager + kTable, newTable);
    else ok = Store(manager + kTable, newTable) && Store(manager + kCapacity, kNewCapacity);
    ok = ok && Store(manager + kBuffer, newBuffer) && Store(manager + kBufferSize, kNewBufferSize);
    Log("search table replaced: %u of %u entries, %u byte buffer, %s (old storage left to the game)", filled, kNewCapacity,
        kNewBufferSize, ok ? "published" : "PUBLISH FAILED");
    return ok;
}

std::vector<std::string> ReadOrder(const std::string& modsDir) {
    std::vector<std::string> order;
    FILE* f = _wfsopen(Wide(modsDir + "\\order.txt").c_str(), L"r", _SH_DENYNO);
    if (!f) return order;
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
        size_t start = 0;
        while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) ++start;
        s = s.substr(start);
        if (s.empty() || s[0] == '#' || s[0] == ';') continue;
        order.push_back(s);
    }
    std::fclose(f);
    return order;
}

std::vector<std::string> ModFolders(const std::string& modsDir) {
    std::vector<std::string> found;
    WIN32_FIND_DATAW data{};
    HANDLE h = FindFirstFileW(Wide(modsDir + "\\*").c_str(), &data);
    if (h == INVALID_HANDLE_VALUE) return found;
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const std::string name = Utf8(data.cFileName);
        if (name == "." || name == ".." || name.empty() || name[0] == '-' || name[0] == '.') continue;
        found.push_back(name);
    } while (FindNextFileW(h, &data));
    FindClose(h);
    std::sort(found.begin(), found.end(), [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
    // order.txt entries first, in that order; the rest alphabetical behind them
    std::vector<std::string> ordered;
    for (const auto& wanted : ReadOrder(modsDir)) {
        for (auto it = found.begin(); it != found.end(); ++it)
            if (_stricmp(it->c_str(), wanted.c_str()) == 0) { ordered.push_back(*it); found.erase(it); break; }
    }
    ordered.insert(ordered.end(), found.begin(), found.end());
    return ordered;
}

unsigned CountFiles(const std::string& dir, unsigned limit) {
    unsigned count = 0;
    WIN32_FIND_DATAW data{};
    HANDLE h = FindFirstFileW(Wide(dir + "\\*").c_str(), &data);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        const std::string name = Utf8(data.cFileName);
        if (name == "." || name == "..") continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) count += CountFiles(dir + "\\" + name, limit - count);
        else ++count;
        if (count >= limit) break;
    } while (FindNextFileW(h, &data));
    FindClose(h);
    return count;
}

DWORD WINAPI Start(LPVOID) {
    base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    const std::string directory = PluginDirectory();
    const std::string ini = directory + "\\DS2ModLoader.ini";
    logFile = _fsopen((directory + "\\DS2ModLoader.log").c_str(), "w", _SH_DENYWR);
    Log("DS2ModLoader base=%08X ini=%s", static_cast<unsigned>(base), ini.c_str());
    if (GetPrivateProfileIntA("Mods", "Enabled", 1, ini.c_str()) == 0) { Log("disabled by ini"); return 0; }
    char folderName[128] = "mods";
    GetPrivateProfileStringA("Mods", "Folder", "mods", folderName, sizeof(folderName), ini.c_str());
    const bool prependGameDir = GetPrivateProfileIntA("Mods", "LooseFilesFirst", 0, ini.c_str()) != 0;

    unsigned waited = 0;
    for (; waited < 300 && !UnpackedImage(); ++waited) Sleep(1000);
    if (waited >= 300) { Log("unpacked image never appeared; nothing installed"); return 0; }
    Log("unpacked image ready after %u s", waited);
    if (!SignaturesMatch()) return 0;

    uintptr_t manager = 0;
    for (waited = 0; waited < 600 && !ManagerReady(manager); ++waited) Sleep(100);
    if (!ManagerReady(manager)) { Log("filesystem manager not ready after %u s; nothing installed", waited / 10); return 0; }
    Log("filesystem manager %08X ready after %u ms; search path before: %s", static_cast<unsigned>(manager), waited * 100, SearchPath(manager).c_str());

    const std::string game = GameDirectoryUtf8();
    const std::string modsDir = game + "\\" + folderName;
    CreateDirectoryW(Wide(modsDir).c_str(), nullptr);
    std::vector<std::string> mods = ModFolders(modsDir);
    if (mods.empty() && !prependGameDir) { Log("no mod folders in %s; search path unchanged", modsDir.c_str()); return 0; }
    bool complete = false;
    const std::vector<std::string> old = SearchEntries(manager, complete);
    if (!complete || old.empty()) { Log("current search path unreadable; nothing changed"); return 0; }

    // Highest priority first: the mods in order, then the game folder (LooseFilesFirst=1).
    // The table holds kNewCapacity entries including its NUL end, and the
    // buffer kNewBufferSize bytes: the lowest-priority mods give way.
    std::vector<std::string> front;
    const size_t fixed = old.size() + (prependGameDir ? 1 : 0);
    size_t bytes = 1 + (prependGameDir ? game.size() + 1 : 0);
    for (const auto& entry : old) bytes += entry.size() + 1;
    size_t kept = 0;
    for (; kept < mods.size(); ++kept) {
        const size_t need = modsDir.size() + 1 + mods[kept].size() + 1;
        if (front.size() + fixed + 1 >= kNewCapacity || bytes + need > kNewBufferSize) break;
        front.push_back(modsDir + "\\" + mods[kept]);
        bytes += need;
    }
    for (size_t i = kept; i < mods.size(); ++i) Log("mod '%s' left out: the search table is full", mods[i].c_str());
    mods.resize(kept);
    if (prependGameDir) { front.push_back(game); Log("game folder itself searched before the archives (LooseFilesFirst=1)"); }
    if (!Publish(manager, front, old)) return 0;
    for (const auto& mod : mods) {
        const std::string path = modsDir + "\\" + mod;
        Log("mod '%s': %u file(s)", mod.c_str(), CountFiles(path, 100000));
    }
    Log("search path now: %s", SearchPath(manager).c_str());
    Log("%u mod folder(s) active; a file under mods\\<mod>\\<path as inside the DAT> replaces the archived one", static_cast<unsigned>(mods.size()));
    return 0;
}
}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        CreateThread(nullptr, 0, Start, nullptr, 0, nullptr);
    }
    return TRUE;
}
