// Dead Space 2 United by Richard Hafer.
#pragma once
#include <cstdint>

namespace coop_save {
enum class Role { Unknown, Local, Ghost };
struct Evidence {
    uint32_t vtable = 0, proxy = 0, flags = 0;
    Role role = Role::Unknown;
};

template<class Reader>
Evidence Inspect(uint32_t player, uint32_t expectedVtable, Reader read) {
    Evidence result;
    if (player < 0x10000 || player > 0xFFFFFF93 ||
        !read(player, &result.vtable, 4) || result.vtable != expectedVtable ||
        !read(player + 0x68, &result.proxy, 4)) return result;
    if (!result.proxy) {
        result.role = Role::Local;
        return result;
    }
    uint32_t owner = 0;
    if (result.proxy > 0xFFFFFFBB ||
        !read(result.proxy + 0x30, &result.flags, 4) ||
        !read(result.proxy + 0x40, &owner, 4) || owner != player + 0x5C) return result;
    result.role = (result.flags & 2) ? Role::Ghost : Role::Local;
    return result;
}
}  // namespace coop_save
