#pragma once
#include "native_layout.h"
#include "policy.h"
#include <array>

namespace ds2::native {
constexpr size_t kMaxRebindingComponents = 64;
enum class RebindResult {
    Ready, InvalidList, Unreadable, MissingSpawning, DuplicateSpawning,
    WrongMode, WrongVent, InvalidReferenceCount
};
struct ComponentListCopy {
    uint32_t layoutKey = 0, count = 0;
    std::array<uint32_t, kMaxRebindingComponents> references{};
};
static_assert(offsetof(ComponentListCopy, references) == 8);
struct SpawningEntry {
    uint32_t reference = 0, header = 0, index = 0;
};
struct VentHeaders { uint32_t source = 0, donor = 0; };

// References point to cells containing refcounted attribute headers.
template<class Reader>
RebindResult inspectComponents(uint32_t address, uint32_t spawningDescriptor,
                              Reader& read, ComponentListCopy& list, SpawningEntry& spawn) {
    if (!address || address > UINT32_MAX - sizeof(ComponentListCopy)) return RebindResult::InvalidList;
    if (!read(address, &list, 8)) return RebindResult::Unreadable;
    if (!list.count || list.count > kMaxRebindingComponents) return RebindResult::InvalidList;
    if (!read(address + 8, list.references.data(), list.count * 4)) return RebindResult::Unreadable;
    unsigned matches = 0;
    for (uint32_t i = 0; i < list.count; ++i) {
        uint32_t header = 0, descriptor = 0;
        if (!list.references[i] || !read(list.references[i], &header, 4) || !header ||
            !read(header, &descriptor, 4) || !descriptor) return RebindResult::Unreadable;
        if (descriptor == spawningDescriptor) {
            spawn = {list.references[i], header, i};
            ++matches;
        }
    }
    return matches == 1 ? RebindResult::Ready : matches ? RebindResult::DuplicateSpawning : RebindResult::MissingSpawning;
}

template<class Reader>
RebindResult prepareVentHeaders(uint32_t sourceList, uint32_t donorList,
                                 uint32_t spawningDescriptor, const Guid& expectedVent,
                                 Reader& read, VentHeaders& output, bool allowDirectDonor = false) {
    output = {};
    ComponentListCopy source{}, donor{};
    SpawningEntry original{}, replacement{};
    auto status = inspectComponents(sourceList, spawningDescriptor, read, source, original);
    if (status != RebindResult::Ready) return status;
    status = inspectComponents(donorList, spawningDescriptor, read, donor, replacement);
    if (status != RebindResult::Ready) return status;
    if (original.header > UINT32_MAX - 0x6C || replacement.header > UINT32_MAX - 0x6C)
        return RebindResult::Unreadable;
    uint32_t sourceMode = 0, donorMode = 0;
    Guid vent{};
    if (!read(original.header + 0x10, &sourceMode, 4) ||
        !read(replacement.header + 0x10, &donorMode, 4) ||
        !read(original.header + 0x18, vent.data(), sizeof(vent))) return RebindResult::Unreadable;
    if (sourceMode != 1 || (donorMode != 1 && !(allowDirectDonor && donorMode == 0))) return RebindResult::WrongMode;
    if (expectedVent == Guid{} || vent != expectedVent) return RebindResult::WrongVent;
    uint16_t sourceRefs = 0, donorRefs = 0;
    if (!read(original.header + 0x0C, &sourceRefs, 2) ||
        !read(replacement.header + 0x0C, &donorRefs, 2)) return RebindResult::Unreadable;
    if (!sourceRefs || sourceRefs == UINT16_MAX || !donorRefs)
        return RebindResult::InvalidReferenceCount;
    output = {original.header, replacement.header};
    return RebindResult::Ready;
}

template<class Reader>
RebindResult prepareVentRebinding(uint32_t sourceList, uint32_t donorList,
                                 uint32_t spawningDescriptor, const Guid& expectedVent,
                                 Reader& read, ComponentListCopy& output) {
    output = {};
    VentHeaders headers{};
    auto status = prepareVentHeaders(sourceList, donorList, spawningDescriptor, expectedVent, read, headers);
    if (status != RebindResult::Ready) return status;
    ComponentListCopy source{}, donor{};
    SpawningEntry original{}, replacement{};
    status = inspectComponents(sourceList, spawningDescriptor, read, source, original);
    if (status != RebindResult::Ready) return status;
    status = inspectComponents(donorList, spawningDescriptor, read, donor, replacement);
    if (status != RebindResult::Ready) return status;
    if (original.header != headers.source || replacement.header != headers.donor)
        return RebindResult::Unreadable;
    // Checked, no longer assumed: the donor's cached component layout is unchanged.
    donor.references[replacement.index] = original.reference;
    output = donor;
    return RebindResult::Ready;
}
}
