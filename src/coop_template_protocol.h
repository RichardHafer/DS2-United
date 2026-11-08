// Dead Space 2 United by Richard Hafer.
#pragma once
#include <cstdint>
namespace coop_template {
constexpr uint32_t kMagic = 0x32544443, kVersion = 1;
enum Result : uint32_t { Request, Match, Missing, Mismatch, Unreadable };
struct Identity { uint32_t hash, guid[4], classID, flags; };
struct Packet {
    uint32_t magic, version, query, ownerToken, ownerEpoch, responderToken, responderEpoch, result;
    Identity identity;
};
static_assert(sizeof(Packet) == 60, "Unexpected template wire layout");
inline bool Equal(const Identity& a, const Identity& b) {
    if (a.hash != b.hash || a.classID != b.classID || a.flags != b.flags) return false;
    for (unsigned i = 0; i < 4; ++i) if (a.guid[i] != b.guid[i]) return false;
    return true;
}
inline bool Valid(const Packet& p) {
    uint32_t hash = 0;
    for (auto word : p.identity.guid) hash = hash * 33 + word;
    return p.magic == kMagic && p.version == kVersion && p.query && p.ownerToken && p.ownerEpoch &&
           p.result <= Unreadable && hash == p.identity.hash && p.identity.classID;
}
inline bool ReplyMatches(const Packet& request, const Packet& reply, uint32_t peerToken, uint32_t peerEpoch) {
    return Valid(reply) && reply.result != Request && reply.query == request.query &&
           reply.ownerToken == request.ownerToken && reply.ownerEpoch == request.ownerEpoch &&
           reply.responderToken == peerToken && reply.responderEpoch == peerEpoch &&
           Equal(request.identity, reply.identity);
}
} // namespace coop_template
