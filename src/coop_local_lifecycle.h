// Dead Space 2 United by Hafer.
#pragma once
#include <cstdint>

namespace coop_local {
struct SettleState {
    uint32_t player = 0, released = 0, since = 0;
    bool tracking = false;

    // settleMs: how long the same body must stand (4 s in the campaign; an MP map
    // is fully loaded once it runs, so a visit there asks for less)
    bool Update(uint32_t now, bool inLevel, bool holding, uint32_t releaseSerial,
                uint32_t currentPlayer, uint32_t settleMs = 4000) {
        const bool changed = releaseSerial != released;
        released = releaseSerial;
        if (!inLevel || holding || !currentPlayer) {
            player = 0;
            tracking = false;
            since = now;
            return false;
        }
        if (!tracking || changed || player != currentPlayer) {
            player = currentPlayer;
            since = now;
            tracking = true;
            return false;
        }
        return now - since > settleMs;
    }
    // A respawn on the map hands slot 0 a new body in a level that stays: the
    // level is as settled as it was (otherwise the bots let go of their bodies
    // and the session stops ticking for everybody for four seconds).
    bool Adopt(uint32_t newPlayer) {
        if (!tracking || !newPlayer || player == newPlayer) return false;
        player = newPlayer;
        return true;
    }
};
}  // namespace coop_local
