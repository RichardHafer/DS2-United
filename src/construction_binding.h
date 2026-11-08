// Dead Space 2 United by Hafer.
#pragma once
#include <cstdint>

namespace ds2::native {
enum class BindResult { Matched, Inactive, WrongPacket, WrongList, WrongCaller,
                        InvalidOwner, AlreadyBound, WrongOwner, WrongHeader, Consumed };

struct ConstructionBinding {
    uint32_t packet = 0, componentList = 0, sourceHeader = 0, donorHeader = 0;
    uint32_t owner = 0;
    bool consumed = false;

    bool arm(uint32_t packetAddress, uint32_t listAddress, uint32_t source, uint32_t donor) {
        *this = {};
        if (!packetAddress || !listAddress || !source || !donor || source == donor) return false;
        packet = packetAddress;
        componentList = listAddress;
        sourceHeader = source;
        donorHeader = donor;
        return true;
    }

    BindResult bindOwner(uint32_t callerRva, uint32_t activePacket, uint32_t activeList, uint32_t actor) {
        if (!packet) return BindResult::Inactive;
        if (callerRva != 0x3CF40D) return BindResult::WrongCaller;
        if (activePacket != packet) return BindResult::WrongPacket;
        if (activeList != componentList) return BindResult::WrongList;
        if (!actor) return BindResult::InvalidOwner;
        if (owner) return BindResult::AlreadyBound;
        owner = actor;
        return BindResult::Matched;
    }

    BindResult replacement(uint32_t callerRva, uint32_t actor, uint32_t header, uint32_t& output) {
        output = 0;
        if (!packet || !owner) return BindResult::Inactive;
        if (consumed) return BindResult::Consumed;
        if (callerRva != 0x1E41A && callerRva != 0x1E459) return BindResult::WrongCaller;
        if (actor != owner) return BindResult::WrongOwner;
        if (header != donorHeader) return BindResult::WrongHeader;
        output = sourceHeader;
        consumed = true;
        return BindResult::Matched;
    }
};
}
