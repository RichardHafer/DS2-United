// Dead Space 2 United by Hafer.
#pragma once
#include "schematic_policy.h"
#include <cstring>

namespace ds2::schematics {
struct Blueprint { Guid definition; std::array<Guid, 4> visuals; const char* name; };
inline constexpr size_t kAttributeSize = 0x320;
inline constexpr size_t kSharedGuidOffset = 0x138;
inline constexpr size_t kRewardGuidOffset = 0x2F0;
inline constexpr size_t kVisualOffsets[] = {0xE4, 0x25C, 0x26C, 0x300};
using AttributeBytes = std::array<unsigned char, kAttributeSize>;
enum class AttributeResult { UnknownIdentity, WrongDefinition, UnexpectedReward,
    UnexpectedVisual, Prepared, AlreadySelected };

inline Guid ReadGuid(const AttributeBytes& bytes, size_t offset) {
    Guid result{};
    std::memcpy(result.data(), bytes.data() + offset, sizeof(result));
    return result;
}
inline void WriteGuid(AttributeBytes& bytes, size_t offset, const Guid& guid) {
    std::memcpy(bytes.data() + offset, guid.data(), sizeof(guid));
}

template<size_t N>
AttributeResult PrepareAttributes(const Source (&sources)[N], const Blueprint (&blueprints)[N],
    const Policy<N>& policy, const Guid& entity, const Guid& instanceReward,
    AttributeBytes& bytes, Guid& selectedReward, size_t& sourceIndex, size_t& targetIndex) {
    const Guid shared = ReadGuid(bytes, kSharedGuidOffset);
    size_t source = N;
    for (size_t i = 0; i < N; ++i) {
        if (entity == blueprints[i].definition ||
            (sources[i].kind == SourceKind::Placed && entity == sources[i].entity)) {
            source = i;
            break;
        }
    }
    if (source == N) return AttributeResult::UnknownIdentity;
    if (shared != blueprints[source].definition) return AttributeResult::WrongDefinition;
    Guid selected{};
    if (policy.Select(sources[source].entity, sources[source].kind, sources[source].reward, selected)
        != Result::Selected) return AttributeResult::UnknownIdentity;
    size_t target = N;
    for (size_t i = 0; i < N; ++i) if (sources[i].reward == selected) { target = i; break; }
    if (target == N) return AttributeResult::UnexpectedReward;
    const Guid current = ReadGuid(bytes, kRewardGuidOffset);
    if ((current != sources[source].reward && current != selected) ||
        (instanceReward != sources[source].reward && instanceReward != selected))
        return AttributeResult::UnexpectedReward;
    bool changed = current != selected || instanceReward != selected;
    for (size_t i = 0; i < 4; ++i) {
        const Guid visual = ReadGuid(bytes, kVisualOffsets[i]);
        if (visual != blueprints[source].visuals[i] && visual != blueprints[target].visuals[i])
            return AttributeResult::UnexpectedVisual;
        changed = changed || visual != blueprints[target].visuals[i];
    }
    WriteGuid(bytes, kRewardGuidOffset, selected);
    for (size_t i = 0; i < 4; ++i) WriteGuid(bytes, kVisualOffsets[i], blueprints[target].visuals[i]);
    selectedReward = selected;
    sourceIndex = source;
    targetIndex = target;
    return changed ? AttributeResult::Prepared : AttributeResult::AlreadySelected;
}
}  // namespace ds2::schematics
