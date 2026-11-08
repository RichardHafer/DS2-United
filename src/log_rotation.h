// Dead Space 2 United by Richard Hafer.
#pragma once
// The last runs of a log: name.log -> name.prev.log -> name.prev2.log -> name.prev3.log
// (16.10.: two quick restarts after a crash rotated the crashed session's log away).
// A log another running copy of the game still writes is left alone.
#include <windows.h>
#include <string>

namespace log_rotation {
inline std::string Previous(const std::string& stem, unsigned n) {
    return stem + (n == 1 ? std::string(".prev.log") : ".prev" + std::to_string(n) + ".log");
}
// True when another process has the file open for writing (our logs are opened deny-write).
inline bool InUse(const std::string& path) {
    const HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_SHARING_VIOLATION;
    CloseHandle(h);
    return false;
}
// stem: the path without ".log". False when the log is in use (the caller tries the next copy number).
inline bool Rotate(const std::string& stem, unsigned keep = 3) {
    const std::string current = stem + ".log";
    if (GetFileAttributesA(current.c_str()) == INVALID_FILE_ATTRIBUTES) return true;
    if (InUse(current)) return false;
    DeleteFileA(Previous(stem, keep).c_str());
    for (unsigned n = keep; n-- > 1;) MoveFileA(Previous(stem, n).c_str(), Previous(stem, n + 1).c_str());
    MoveFileA(current.c_str(), Previous(stem, 1).c_str());
    return true;
}
}  // namespace log_rotation
