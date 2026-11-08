// Dead Space 2 United by Richard Hafer.
#pragma once
#include <string>
#include <windows.h>

// Dead Space 2 United keeps every setting in one DS2United.ini next to the plugin
// (written by DS2UnitedConfig.exe). Before the rename the merged file was
// DS2Coop.ini, told apart from the co-op half's own file of that name by the
// randomizer's section, and before that HaferSprawl.ini. Without any of them the
// old per-plugin files are read as before.
namespace ds2coop {
inline bool FileExists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
inline bool IsMergedFile(const std::string& path) {
    char keys[64];
    return FileExists(path) && GetPrivateProfileStringA("DS2Randomizer", nullptr, "", keys, sizeof(keys), path.c_str()) > 0;
}
// `folder` is empty or ends with a path separator.
inline std::string ConfigFile(const std::string& folder, const char* legacy) {
    const std::string united = folder + "DS2United.ini";
    if (IsMergedFile(united)) return united;
    const std::string merged = folder + "DS2Coop.ini";
    if (IsMergedFile(merged)) return merged;
    const std::string beforeRename = folder + "HaferSprawl.ini";
    if (FileExists(beforeRename)) return beforeRename;
    // a DS2United.ini without the randomizer's section still beats a missing file
    return FileExists(folder + legacy) || !FileExists(united) ? folder + legacy : united;
}
// The module this code lives in.
inline HMODULE SelfModule() {
    HMODULE module = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&SelfModule), &module);
    return module;
}
// The module that exports the other half's functions: DS2United.asi holds
// both halves (built with DS2COOP_MODULE), the separate builds look it up by name.
inline HMODULE PartnerModule(const char* legacyName) {
#ifdef DS2COOP_MODULE
    (void)legacyName;
    return SelfModule();
#else
    return GetModuleHandleA(legacyName);
#endif
}
}  // namespace ds2coop
