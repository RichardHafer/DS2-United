// Dead Space 2 United by Hafer.
#pragma once
#include <cstdint>

namespace coop_weapon {
enum class Status { Invalid, Unarmed, Missing, Found };
struct Result {
    Status status = Status::Invalid;
    uint32_t item = 0, owner = 0, count = 0;
};
enum class Kind { Unknown, Gun, GunProxy };
struct Identity {
    uint32_t vtable = 0, readable = 0;
    Kind kind = Kind::Unknown;
    uint32_t templateGuid[4]{}, proxyGuid[4]{};
};
template<class Reader>
Identity InspectIdentity(uint32_t item, uint32_t imageBase, Reader read) {
    Identity result;
    if (item < 0x10000 || item > 0xFFFFFBE0 ||
        !read(item, &result.vtable, 4)) return result;
    result.readable |= 1;
    // Every weapon type is its own Gun subclass with its own primary vtable, and
    // the first two known (PulseRifle 0x1751A00, PlasmaCutter 0x17515D8) are only the
    // default starting pair. A weapon of any other class (Contact Beam, Force Gun,
    // Line Gun, ...) came back Unknown, so its proxy GUID never crossed the wire
    // and the partner's ghost was left empty-handed. All gun subclasses keep the
    // proxy GUID at Gun+0x404, so recognising the whole set is enough (offsets
    // and vtables from ds2_classmap_live_2026-09-17.json, 2026-09-21).
    switch (result.vtable - imageBase) {
        case 0x1750660:  // Gun (base)
        case 0x17515D8:  // PlasmaCutter
        case 0x17511B8:  // LineGun
        case 0x1753948:  // ForceGun
        case 0x1751A00:  // PulseRifle
        case 0x1778A78:  // RivetGun
        case 0x1773780:  // SeekerRifle
        case 0x1750D98:  // JavelinGun
        case 0x1753100:  // DetonatorGun
        case 0x1752CE0:  // ContactBeam
        case 0x1752580:  // Yoyo (Ripper)
        case 0x1753520:  // Flamethrower
        case 0x17696F0:  // StasisCaster
        case 0x175A3E0:  // MDSCannon
            result.kind = Kind::Gun; break;
        case 0x1751E20:  // GunProxy (primary)
        case 0x1751D40:  // GunProxy (secondary)
            result.kind = Kind::GunProxy; break;
        default: break;
    }
    if (result.kind == Kind::Unknown) return result;
    auto guid = [&](uint32_t address, uint32_t (&out)[4]) {
        uint32_t words[4]{};
        for (unsigned i = 0; i < 4; ++i)
            if (!read(address + i * 4, &words[i], 4)) return false;
        for (unsigned i = 0; i < 4; ++i) out[i] = words[i];
        return true;
    };
    if (guid(item + 0x24, result.templateGuid)) result.readable |= 2;
    uint32_t address = item + 0x404;
    if (result.kind == Kind::GunProxy) {
        uint32_t attributes = 0;
        if (!read(item + 0xC, &attributes, 4) ||
            attributes < 0x10000 || attributes > 0xFFFFFEB8) return result;
        address = attributes + 0x138;
    }
    if (guid(address, result.proxyGuid)) result.readable |= 4;
    return result;
}
template<class Reader>
Result Find(uint32_t player, uint32_t hash, uint32_t playerVtable, Reader read) {
    Result result;
    uint32_t vt = 0, entries = 0, allocated = 0;
    if (player < 0x10000 || player > 0xFFFFF000 ||
        !read(player, &vt, 4) || vt != playerVtable) return result;
    if (!hash) { result.status = Status::Unarmed; return result; }
    if (!read(player + 0xC08, &entries, 4) ||
        !read(player + 0xC0C, &result.count, 4) ||
        !read(player + 0xC10, &allocated, 4) || result.count > allocated ||
        allocated > 16 || (result.count && (!entries || entries > 0xFFFFFF3F))) return result;
    for (uint32_t i = 0; i < result.count; ++i) {
        uint32_t ref = 0, candidateHash = 0;
        if (!read(entries + i * 12 + 4, &ref, 4)) return result;
        if (!ref) continue;
        if (ref < 0x10010 || ref > 0xFFFFFD00 ||
            !read(ref - 0x10 + 0x25C, &candidateHash, 4)) return result;
        if (candidateHash != hash) continue;
        result.item = ref - 0x10;
        uint32_t ownerRef = 0;
        if (!read(result.item + 0x240, &ownerRef, 4)) return result;
        result.owner = ownerRef >= 0x10010 ? ownerRef - 0x10 : 0;
        result.status = Status::Found;
        return result;
    }
    result.status = Status::Missing;
    return result;
}
}  // namespace coop_weapon
