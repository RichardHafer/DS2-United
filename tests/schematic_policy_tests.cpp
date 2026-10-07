#include "../src/generated_schematic_catalog.h"
#include "../src/generated_schematic_blueprints.h"
#include "../src/generated_schematic_store_levels.h"
#include <cassert>
#include <iterator>
#include <map>
#include <set>
#include <cstdio>

int main() {
    using namespace ds2::schematics;
    constexpr size_t count = sizeof(kCampaignSources) / sizeof(kCampaignSources[0]);
    static_assert(count == 23);
    size_t containers = 0;
    std::set<Guid> originals, entities;
    for (const auto& source : kCampaignSources) {
        originals.insert(source.reward);
        entities.insert(source.entity);
        containers += source.kind == SourceKind::Container;
    }
    assert(originals.size() == count && entities.size() == count && containers == 1);
    Guid sentinel{{1, 2, 3, 4}}, output = sentinel;
    Policy<count> disabled(kCampaignSources, 1);
    const auto& first = kCampaignSources[0];
    assert(disabled.Select(first.entity, first.kind, first.reward, output) == Result::Disabled);
    assert(output == sentinel);
    for (uint64_t seed = 0; seed < 1024; ++seed) {
        Policy<count> policy(kCampaignSources, seed, true), reload(kCampaignSources, seed, true);
        std::set<Guid> rewards;
        for (size_t i = count; i-- > 0;) {
            const auto& source = kCampaignSources[i];
            Guid repeated{};
            assert(policy.Select(source.entity, source.kind, source.reward, output) == Result::Selected);
            assert(reload.Select(source.entity, source.kind, source.reward, repeated) == Result::Selected);
            assert(repeated == output);
            rewards.insert(output);
        }
        assert(rewards == originals);
    }
    {
        // Pins: the first two story sources land on a weapon and an affordable
        // suit for every seed, and the result is still a permutation.
        const Guid firstSource{{0x31CAD112u, 0xC2487E3Bu, 0x43524F57u, 0x31313048u}};
        const Guid secondSource{{0x31CB29D8u, 0x8859C534u, 0x43524F57u, 0x31313048u}};
        const Guid weapons[] = {
            {{0x36C8BC4Fu, 0x67BB5F38u, 0x44455257u, 0x34303245u}}, {{0x45CB0685u, 0xDD95DCDAu, 0x43534147u, 0x4E47494Fu}},
            {{0x36C8BC40u, 0x0C67B69Au, 0x44455257u, 0x34303245u}}, {{0x58CA30C7u, 0x78E54CEBu, 0x3045454Cu, 0x58373634u}},
            {{0x36C8BC4Fu, 0x6F9078E2u, 0x44455257u, 0x34303245u}}, {{0x36C8BC48u, 0xED477CBAu, 0x44455257u, 0x34303245u}},
            {{0x36C8BC3Fu, 0xF55615C8u, 0x44455257u, 0x34303245u}}};
        const Guid suits[] = {{{0x39CA8FDFu, 0x97CF9234u, 0x574F5242u, 0x3130314Eu}}, {{0x39CB3355u, 0x740F0370u, 0x4E415548u, 0x38343147u}}};
        std::set<Guid> weaponSet(std::begin(weapons), std::end(weapons)), suitSet(std::begin(suits), std::end(suits));
        for (uint64_t seed = 0; seed < 512; ++seed) {
            Policy<count> pinned(kCampaignSources, seed, true);
            uint64_t pinSeed = seed ^ UINT64_C(0x50494E53);
            assert(pinned.Pin(firstSource, weapons, pinSeed));
            assert(pinned.Pin(secondSource, suits, pinSeed));
            std::set<Guid> rewards;
            Guid got{};
            for (const auto& source : kCampaignSources) {
                assert(pinned.Select(source.entity, source.kind, source.reward, got) == Result::Selected);
                rewards.insert(got);
                if (source.entity == firstSource) assert(weaponSet.count(got));
                if (source.entity == secondSource) assert(suitSet.count(got));
            }
            assert(rewards == originals);
            assert(!pinned.Pin(sentinel, weapons, pinSeed));
        }
    }
    Policy<count> policy(kCampaignSources, 19, true);
    output = sentinel;
    assert(policy.Select(sentinel, first.kind, first.reward, output) == Result::UnknownSource);
    assert(output == sentinel);
    assert(policy.Select(first.entity, first.kind, sentinel, output) == Result::UnexpectedReward);
    assert(output == sentinel);
    const auto wrong = first.kind == SourceKind::Placed ? SourceKind::Container : SourceKind::Placed;
    assert(policy.Select(first.entity, wrong, first.reward, output) == Result::WrongSourceKind);
    assert(output == sentinel);
    for (uint64_t seed = 0; seed < 128; ++seed) {
        Policy<count> policyWithVisuals(kCampaignSources, seed, true);
        for (size_t i = 0; i < count; ++i) {
            AttributeBytes original;
            original.fill(0xA5);
            WriteGuid(original, kSharedGuidOffset, kCampaignBlueprints[i].definition);
            WriteGuid(original, kRewardGuidOffset, kCampaignSources[i].reward);
            for (size_t v = 0; v < 4; ++v)
                WriteGuid(original, kVisualOffsets[v], kCampaignBlueprints[i].visuals[v]);
            auto bytes = original;
            size_t source = count, target = count;
            Guid selected{};
            const auto status = PrepareAttributes(kCampaignSources, kCampaignBlueprints,
                policyWithVisuals, kCampaignBlueprints[i].definition, kCampaignSources[i].reward,
                bytes, selected, source, target);
            assert(status == AttributeResult::Prepared || status == AttributeResult::AlreadySelected);
            assert(source == i && target < count && selected == kCampaignSources[target].reward);
            assert(ReadGuid(bytes, kSharedGuidOffset) == kCampaignBlueprints[i].definition);
            for (size_t at = 0; at < bytes.size(); ++at) {
                bool mutableByte = at >= kRewardGuidOffset && at < kRewardGuidOffset + 16;
                for (const auto offset : kVisualOffsets)
                    mutableByte = mutableByte || (at >= offset && at < offset + 16);
                if (!mutableByte) assert(bytes[at] == original[at]);
            }
            const auto once = bytes;
            assert(PrepareAttributes(kCampaignSources, kCampaignBlueprints, policyWithVisuals,
                kCampaignBlueprints[i].definition, selected, bytes, selected, source, target)
                == AttributeResult::AlreadySelected);
            assert(bytes == once);
            auto restored = original;
            Guid restoredReward{};
            PrepareAttributes(kCampaignSources, kCampaignBlueprints, policyWithVisuals,
                ReadGuid(bytes, kSharedGuidOffset), kCampaignSources[i].reward,
                restored, restoredReward, source, target);
            assert(restoredReward == selected && restored == once);
            auto bad = original;
            WriteGuid(bad, kVisualOffsets[0], sentinel);
            const auto before = bad;
            assert(PrepareAttributes(kCampaignSources, kCampaignBlueprints, policyWithVisuals,
                kCampaignBlueprints[i].definition, kCampaignSources[i].reward,
                bad, selected, source, target) == AttributeResult::UnexpectedVisual);
            assert(bad == before);
            bad = original;
            assert(PrepareAttributes(kCampaignSources, kCampaignBlueprints, policyWithVisuals,
                sentinel, kCampaignSources[i].reward, bad, selected, source, target)
                == AttributeResult::UnknownIdentity);
            assert(bad == original);
        }
    }
    // Every reward has an authored store chapter, and moving the chapters with
    // the schematics only reorders them: no chapter is lost or invented.
    {
        auto levelOf = [](const Guid& reward) {
            for (const auto& level : kStoreLevels) if (level.reward == reward) return level.minLevel;
            return -1;
        };
        std::multiset<int> authored;
        for (const auto& source : kCampaignSources) {
            assert(levelOf(source.reward) > 0);
            authored.insert(levelOf(source.reward));
        }
        for (uint64_t seed = 1; seed <= 256; ++seed) {
            Policy<std::size(kCampaignSources)> shuffled(kCampaignSources, seed, true);
            std::map<Guid, int> shifted;   // reward -> chapter of the place it lies at now
            for (const auto& source : kCampaignSources) {
                Guid selected{};
                assert(shuffled.Select(source.entity, source.kind, source.reward, selected) == Result::Selected);
                assert(shifted.emplace(selected, levelOf(source.reward)).second);
            }
            assert(shifted.size() == std::size(kCampaignSources));
            std::multiset<int> moved;
            for (const auto& entry : shifted) moved.insert(entry.second);
            assert(moved == authored);
        }
    }
    std::puts("Schematic store chapters: 23 rewards with authored chapters, 256 seeds keep the chapter set");
    std::puts("Schematic policy: 23 rewards, 1024 bijections, reload/order stability and fail-closed guards passed");
    std::puts("Schematic attributes: 128 seeds, presentation consistency, original identity, immutable bytes and idempotence passed");
}
