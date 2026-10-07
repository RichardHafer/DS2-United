// hook_site::Inspect: pristine bytes, a chained jmp with safetyhook's NOP
// padding, and foreign bytes, at the two sites the randomizer and DS2Coop share.
#include "../src/hook_site.h"
#include <cstdio>

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL %s\n", what); ++failures; }
}
}

int main() {
    using hook_site::Inspect;
    using hook_site::State;
    // 0x34BBC0: sub esp,8 / push ebx / mov ebx,ecx / call; the hook cuts mov ebx,ecx and pads one NOP
    const unsigned char update[] = {0x83, 0xEC, 0x08, 0x53, 0x8B, 0xD9, 0xE8, 0x45, 0xCF, 0x0C, 0x00};
    const unsigned char updateHooked[] = {0xE9, 0x11, 0x22, 0x33, 0x44, 0x90, 0xE8, 0x45, 0xCF, 0x0C, 0x00};
    Check(Inspect(update, update, sizeof(update)) == State::Pristine, "stream update pristine");
    Check(Inspect(updateHooked, update, sizeof(update)) == State::Hooked, "stream update hooked with one NOP");
    // 0x9C5260: push esi / mov esi,ecx / test byte [esi+4],1 / jne; the hook pads two NOPs
    const unsigned char damage[] = {0x56, 0x8B, 0xF1, 0xF6, 0x46, 0x04, 0x01, 0x0F, 0x85};
    const unsigned char damageHooked[] = {0xE9, 0x01, 0x02, 0x03, 0x04, 0x90, 0x90, 0x0F, 0x85};
    Check(Inspect(damageHooked, damage, sizeof(damage)) == State::Hooked, "process damage hooked with two NOPs");
    // a jmp whose tail does not match is not ours to chain behind
    const unsigned char damageWrong[] = {0xE9, 0x01, 0x02, 0x03, 0x04, 0x90, 0x90, 0x0F, 0x84};
    Check(Inspect(damageWrong, damage, sizeof(damage)) == State::Foreign, "wrong tail is foreign");
    const unsigned char patched[] = {0xC3, 0x8B, 0xF1, 0xF6, 0x46, 0x04, 0x01, 0x0F, 0x85};
    Check(Inspect(patched, damage, sizeof(damage)) == State::Foreign, "ret patch is foreign");
    // the old rule still holds: a 5 byte instruction hooked without padding
    const unsigned char tick[] = {0xA1, 0x14, 0xD4, 0x04, 0x02, 0x83, 0xEC, 0x0C};
    const unsigned char tickHooked[] = {0xE9, 0x00, 0x00, 0x00, 0x00, 0x83, 0xEC, 0x0C};
    Check(Inspect(tickHooked, tick, sizeof(tick)) == State::Hooked, "frame tick hooked without padding");
    if (failures) return 1;
    std::printf("Hook site tests passed: pristine, padded chains and foreign bytes.\n");
    return 0;
}
