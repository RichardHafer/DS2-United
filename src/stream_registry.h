// Dead Space 2 United by Richard Hafer.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>

namespace ds2::native {
enum class RegistryStatus { Missing, ReadFailed, InvalidCount, InvalidToc, Changed, Stable };
enum class DescriptorStatus { Missing, Found, Invalid, Collision, BudgetExceeded };
struct RegistrySnapshot {
    RegistryStatus status = RegistryStatus::Missing;
    uint32_t manager = 0, count = 0;
    std::array<uint32_t, 32> tocs{};
    std::array<uint16_t, 32> references{};
};

template<class Read>
DescriptorStatus findDescriptor(const RegistrySnapshot& snapshot, uint32_t hash, const char* name, Read read) {
    if (snapshot.status != RegistryStatus::Stable || snapshot.count > 32) return DescriptorStatus::Invalid;
    const size_t nameLength = std::strlen(name);
    if (!nameLength || nameLength >= 160) return DescriptorStatus::Invalid;
    for (unsigned t = 0; t < snapshot.count; ++t) {
        const auto toc = snapshot.tocs[t];
        uint16_t count = 0;
        if (toc > UINT32_MAX - 16 || !read(toc + 8, &count, 2) ||
            uint64_t(toc) + 16 + uint64_t(count) * 20 > UINT32_MAX) return DescriptorStatus::Invalid;
        auto identityAt = [&](unsigned index, std::array<uint32_t, 2>& identity) {
            uint32_t pointer = 0;
            return read(toc + 16 + index * 20, &pointer, 4) && pointer && !(pointer & 3) &&
                pointer <= UINT32_MAX - 8 && read(pointer, identity.data(), 8);
        };
        unsigned first = 0, end = count;
        if (hash) {
            while (first < end) {
                const unsigned middle = first + (end - first) / 2;
                std::array<uint32_t, 2> identity{};
                if (!identityAt(middle, identity)) return DescriptorStatus::Invalid;
                if (identity[0] < hash) first = middle + 1; else end = middle;
            }
        }
        unsigned examined = 0;
        for (unsigned i = first; i < count; ++i) {
            if (++examined > 128) return DescriptorStatus::BudgetExceeded;
            std::array<uint32_t, 2> identity{};
            if (!identityAt(i, identity)) return DescriptorStatus::Invalid;
            if (identity[0] != hash) break;
            if (!identity[1] || identity[1] > UINT32_MAX - 160) return DescriptorStatus::Invalid;
            bool same = true;
            for (size_t c = 0; c <= nameLength; ++c) {
                char actual = 0;
                if (!read(identity[1] + static_cast<uint32_t>(c), &actual, 1)) return DescriptorStatus::Invalid;
                if (actual != name[c]) { same = false; break; }
            }
            if (same) return DescriptorStatus::Found;
            if (hash) return DescriptorStatus::Collision;
        }
    }
    return DescriptorStatus::Missing;
}

inline const char* descriptorStatusName(DescriptorStatus status) {
    switch (status) {
    case DescriptorStatus::Missing: return "missing";
    case DescriptorStatus::Found: return "found";
    case DescriptorStatus::Invalid: return "invalid_read";
    case DescriptorStatus::Collision: return "identity_collision";
    case DescriptorStatus::BudgetExceeded: return "scan_budget";
    }
    return "unknown";
}

// Diagnostic snapshot only. It does not lock the native registry.
template<class Read>
RegistrySnapshot inspectRegistry(uint32_t manager, Read read) {
    RegistrySnapshot result;
    result.manager = manager;
    if (!manager) return result;
    result.status = RegistryStatus::ReadFailed;
    if (!read(manager + 0x130, &result.count, 4)) return result;
    if (result.count > result.tocs.size()) {
        result.status = RegistryStatus::InvalidCount;
        return result;
    }
    if (result.count && !read(manager + 0xB0, result.tocs.data(), result.count * 4)) return result;
    for (uint32_t i = 0; i < result.count; ++i) {
        const auto pointer = result.tocs[i];
        if (!pointer || (pointer & 3)) { result.status = RegistryStatus::InvalidToc; return result; }
        for (uint32_t j = 0; j < i; ++j)
            if (result.tocs[j] == pointer) { result.status = RegistryStatus::InvalidToc; return result; }
        std::array<uint32_t, 4> header{};
        if (!read(pointer, header.data(), sizeof(header))) return result;
        if (header[0] != 0x9CBA7B28 || header[1] != 10) {
            result.status = RegistryStatus::InvalidToc; return result;
        }
        result.references[i] = static_cast<uint16_t>(header[3] >> 16);
    }
    uint32_t finalCount = 0;
    std::array<uint32_t, 32> finalTocs{};
    if (!read(manager + 0x130, &finalCount, 4)) return result;
    if (finalCount != result.count) { result.status = RegistryStatus::Changed; return result; }
    if (finalCount && !read(manager + 0xB0, finalTocs.data(), finalCount * 4)) return result;
    result.status = finalTocs == result.tocs ? RegistryStatus::Stable : RegistryStatus::Changed;
    return result;
}

inline const char* registryStatusName(RegistryStatus status) {
    switch (status) {
    case RegistryStatus::Missing: return "manager_missing";
    case RegistryStatus::ReadFailed: return "read_failed";
    case RegistryStatus::InvalidCount: return "invalid_count";
    case RegistryStatus::InvalidToc: return "invalid_toc";
    case RegistryStatus::Changed: return "registry_changed";
    case RegistryStatus::Stable: return "snapshot_stable";
    }
    return "unknown";
}
}
