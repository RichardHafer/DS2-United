#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ds2::native {
// Relocated TOC record used by 0074C360 and 0074B0E5. Not a disk struct.
struct StreamDescriptor32 {
    uint32_t identity;
    uint32_t contentSize;
    uint8_t prerequisiteCount;
    uint8_t dependentCount;
    uint8_t flags;
    uint8_t unknown0B;
    uint32_t dlcIndex;
    uint32_t references;
};
static_assert(sizeof(StreamDescriptor32) == 0x14);
static_assert(offsetof(StreamDescriptor32, prerequisiteCount) == 8);
static_assert(offsetof(StreamDescriptor32, dependentCount) == 9);
static_assert(offsetof(StreamDescriptor32, references) == 0x10);

constexpr unsigned dependentReferenceIndex(const StreamDescriptor32& descriptor, unsigned index) {
    return descriptor.prerequisiteCount + (descriptor.flags & 1u) + index;
}

// PC fields written by 0084A530. Addresses are borrowed, not owned.
struct SpawnInfo32 {
    uint32_t record;
    uint32_t transform;
    uint32_t field08;
    uint32_t recordField0C;
    uint32_t attributes;
    uint32_t componentSet;
    uint32_t field18;
    uint32_t field1C;
    uint32_t flags;
};
static_assert(std::is_standard_layout_v<SpawnInfo32>);
static_assert(sizeof(SpawnInfo32) == 0x24);
static_assert(offsetof(SpawnInfo32, transform) == 0x04);
static_assert(offsetof(SpawnInfo32, attributes) == 0x10);
static_assert(offsetof(SpawnInfo32, componentSet) == 0x14);
static_assert(offsetof(SpawnInfo32, flags) == 0x20);

constexpr uint32_t kFuncSpawnLookupCallRva = 0x00ACB2B0;
constexpr uint32_t kFuncSpawnCreateCallRva = 0x00ACB2CB;
constexpr uint32_t kFuncSpawnFinalizeCallRva = 0x00ACB411;
constexpr uint32_t kFuncSpawnCompleteCallRva = 0x00ACB419;
// The 0x2000 creation option suppresses manager auto-finalization on this path.
constexpr uint32_t kDeferredFinalize = 0x2000;
}
