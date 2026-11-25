// Dead Space 2 United by Richard Hafer.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstring>

namespace ds2_vr {
inline bool HasMarkers(const unsigned char* bytes, size_t size) {
    const auto contains = [&](const char* text) {
        return std::search(bytes, bytes + size, text, text + std::strlen(text)) != bytes + size;
    };
    return size && contains("DS2PCVR") && contains("ds2stereo.ini") && contains("vr_settings Apt");
}
#ifdef _WIN32
enum class Detection { Missing, Unreadable, OtherProxy, DS2PCVR };
inline Detection Detect() {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return Detection::Unreadable;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash || static_cast<size_t>(slash - path) + 10 >= MAX_PATH) return Detection::Unreadable;
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"d3d9.dll");
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? Detection::Missing : Detection::Unreadable;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 64 * 1024 * 1024) {
        CloseHandle(file);
        return Detection::Unreadable;
    }
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    const auto* bytes = mapping ? static_cast<const unsigned char*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0)) : nullptr;
    Detection result = Detection::Unreadable;
    if (bytes) {
        result = HasMarkers(bytes, static_cast<size_t>(size.QuadPart)) ? Detection::DS2PCVR : Detection::OtherProxy;
        UnmapViewOfFile(bytes);
    }
    if (mapping) CloseHandle(mapping);
    CloseHandle(file);
    return result;
}
inline const char* Name(Detection d) {
    switch (d) {
    case Detection::Missing: return "no game-folder d3d9.dll";
    case Detection::Unreadable: return "game-folder d3d9.dll could not be inspected";
    case Detection::OtherProxy: return "another d3d9.dll";
    case Detection::DS2PCVR: return "DS2PCVR";
    }
    return "unknown";
}
#endif
}
