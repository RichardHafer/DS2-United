// Dead Space 2 United by Hafer.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace survival {
struct RewardQueue {
    std::array<uint32_t, 64> waves{};
    std::size_t first = 0, count = 0;

    bool Push(uint32_t wave) {
        if (count == waves.size()) return false;
        waves[(first + count) % waves.size()] = wave;
        ++count;
        return true;
    }
    bool Take(uint32_t& wave) {
        if (!count) return false;
        wave = waves[first];
        first = (first + 1) % waves.size();
        --count;
        return true;
    }
    void Clear() { first = count = 0; }
};

constexpr uint32_t kRewardMagic = 0x31525753u; // SWR1
enum class RewardKind : uint32_t { Hello = 1, Grant, Ack };
struct RewardWire {
    uint32_t magic = kRewardMagic;
    RewardKind kind = RewardKind::Hello;
    uint32_t sender = 0, target = 0, generation = 0, hostRun = 0, serial = 0, wave = 0;
};
static_assert(sizeof(RewardWire) == 32, "reward wire size");
inline uint32_t NextRewardSerial(uint32_t value) { return ++value ? value : 1; }

struct RewardPartner {
    uint32_t token = 0, generation = 0, serial = 1, sentAt = 0;
    RewardQueue pending;
    void Bind(uint32_t peer) {
        if (token != peer) { *this = {}; token = peer; }
    }
    bool Hello(uint32_t next) {
        if (!next || (generation && static_cast<int32_t>(next - generation) < 0)) return false;
        if (generation != next) {
            // Before the first hello, grants belong to the peer already present at wave end.
            if (generation) pending.Clear();
            generation = next;
            serial = 1;
            sentAt = 0;
        }
        return true;
    }
    void Restart() { pending.Clear(); serial = 1; sentAt = 0; }
    bool Grant(uint32_t sender, uint32_t run, uint32_t now, RewardWire& out) {
        if (!token || !generation || !pending.count || (sentAt && now - sentAt < 500)) return false;
        out = {kRewardMagic, RewardKind::Grant, sender, token, generation, run, serial, pending.waves[pending.first]};
        sentAt = now;
        return true;
    }
    bool Ack(const RewardWire& p, uint32_t run) {
        if (p.sender != token || p.generation != generation || p.hostRun != run || p.serial != serial) return false;
        uint32_t wave = 0;
        if (!pending.Take(wave)) return false;
        serial = NextRewardSerial(serial);
        sentAt = 0;
        return true;
    }
};

struct RewardReceiver {
    uint32_t generation = 1, host = 0, run = 0, paid = 0;
    void Restart() { generation = NextRewardSerial(generation); host = run = paid = 0; }
    bool ObserveRun(uint32_t owner, uint32_t next, RewardQueue& queue) {
        if (!owner || !next) return false;
        if (host == owner && run == next) return true;
        if (host == owner && run && static_cast<int32_t>(next - run) <= 0) return false;
        if (host) queue.Clear();
        host = owner; run = next; paid = 0;
        return true;
    }
    bool Accept(const RewardWire& p, uint32_t localToken, RewardQueue& queue, bool& added) {
        added = false;
        if (p.kind != RewardKind::Grant || !p.sender || p.target != localToken || p.generation != generation ||
            !p.hostRun || !p.serial || p.wave > INT32_MAX) return false;
        if (host != p.sender || run != p.hostRun) {
            if (p.serial != 1 || !ObserveRun(p.sender, p.hostRun, queue)) return false;
        }
        if (p.serial == paid) return true;
        if (p.serial != NextRewardSerial(paid) || !queue.Push(p.wave)) return false;
        paid = p.serial;
        added = true;
        return true;
    }
};
}
