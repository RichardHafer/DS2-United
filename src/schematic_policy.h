// Dead Space 2 United by Hafer.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace ds2::schematics {
using Guid = std::array<uint32_t, 4>;
enum class SourceKind { Placed, Container };
struct Source { Guid entity; Guid reward; SourceKind kind; };
enum class Result { Disabled, UnknownSource, WrongSourceKind, UnexpectedReward, Selected };

inline uint64_t Next(uint64_t& state) {
    uint64_t value = (state += UINT64_C(0x9E3779B97F4A7C15));
    value = (value ^ (value >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94D049BB133111EB);
    return value ^ (value >> 31);
}

template<size_t N>
class Policy {
public:
    explicit Policy(const Source (&sources)[N], uint64_t seed, bool enabled = false)
        : sources_(sources), enabled_(enabled) {
        for (size_t i = 0; i < N; ++i) order_[i] = i;
        for (size_t count = N; count > 1; --count) {
            const uint64_t bound = static_cast<uint64_t>(count);
            const uint64_t threshold = (uint64_t{0} - bound) % bound;
            uint64_t value;
            do { value = Next(seed); } while (value < threshold);
            const size_t selected = static_cast<size_t>(value % bound);
            const size_t saved = order_[count - 1];
            order_[count - 1] = order_[selected];
            order_[selected] = saved;
        }
    }

    // Pin one source to a reward out of `allowed`: the seed picks which of
    // the allowed rewards, and the source that held it takes this source's
    // reward in exchange, so the permutation stays a permutation. False when
    // the entity is unknown or none of the allowed rewards is in the pool.
    // Wanted 15.09. so the first schematic of the story is always a weapon
    // and the second always an affordable suit; a run that opens with two
    // ammo blueprints is a slow start on a randomized game.
    template<size_t M>
    bool Pin(const Guid& entity, const Guid (&allowed)[M], uint64_t& seed) {
        size_t target = N;
        for (size_t i = 0; i < N; ++i) if (sources_[i].entity == entity) target = i;
        if (target == N) return false;
        size_t candidates[N]{};
        size_t found = 0;
        for (size_t j = 0; j < N; ++j)
            for (const auto& reward : allowed)
                if (sources_[order_[j]].reward == reward) { candidates[found++] = j; break; }
        if (!found) return false;
        for (size_t c = 0; c < found; ++c)
            if (candidates[c] == target) return true;   // already holds one
        const size_t pick = candidates[static_cast<size_t>(Next(seed) % found)];
        const size_t saved = order_[target];
        order_[target] = order_[pick];
        order_[pick] = saved;
        return true;
    }

    Result Select(const Guid& entity, SourceKind kind, const Guid& original, Guid& output) const {
        if (!enabled_) return Result::Disabled;
        for (size_t i = 0; i < N; ++i) {
            if (sources_[i].entity != entity) continue;
            if (sources_[i].kind != kind) return Result::WrongSourceKind;
            if (sources_[i].reward != original) return Result::UnexpectedReward;
            output = sources_[order_[i]].reward;
            return Result::Selected;
        }
        return Result::UnknownSource;
    }

private:
    const Source (&sources_)[N];
    std::array<size_t, N> order_{};
    bool enabled_;
};
}  // namespace ds2::schematics
