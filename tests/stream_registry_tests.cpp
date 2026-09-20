#include "../src/stream_registry.h"
#include <cassert>
#include <cstring>
#include <iostream>

int main() {
    unsigned char memory[4096]{};
    auto put = [&](unsigned offset, uint32_t value) { std::memcpy(memory + offset, &value, 4); };
    auto read = [&](uint32_t address, void* out, size_t length) {
        if (address > sizeof(memory) || length > sizeof(memory) - address) return false;
        std::memcpy(out, memory + address, length);
        return true;
    };
    using ds2::native::RegistryStatus;
    using ds2::native::inspectRegistry;
    constexpr unsigned manager = 0x100, toc = 0x400;
    assert(inspectRegistry(0, read).status == RegistryStatus::Missing);
    put(manager + 0x130, 33);
    assert(inspectRegistry(manager, read).status == RegistryStatus::InvalidCount);
    put(manager + 0x130, 1);
    put(manager + 0xB0, toc);
    put(toc, 0x9CBA7B28);
    put(toc + 4, 10);
    put(toc + 12, 0x80010000);
    auto result = inspectRegistry(manager, read);
    assert(result.status == RegistryStatus::Stable && result.references[0] == 0x8001);
    put(toc + 8, 1);
    put(toc + 16, 0x500);
    put(0x500, 99);
    put(0x504, 0x600);
    std::memcpy(memory + 0x600, "package", 8);
    using ds2::native::DescriptorStatus;
    using ds2::native::findDescriptor;
    assert(findDescriptor(result, 99, "package", read) == DescriptorStatus::Found);
    assert(findDescriptor(result, 99, "other", read) == DescriptorStatus::Collision);
    assert(findDescriptor(result, 100, "package", read) == DescriptorStatus::Missing);
    put(0x500, 0);
    assert(findDescriptor(result, 0, "package", read) == DescriptorStatus::Found);
    assert(findDescriptor(result, 0, "other", read) == DescriptorStatus::Missing);
    put(0x504, 0xFFFFF000);
    assert(findDescriptor(result, 0, "package", read) == DescriptorStatus::Invalid);
    put(manager + 0x130, 2);
    put(manager + 0xB4, toc);
    assert(inspectRegistry(manager, read).status == RegistryStatus::InvalidToc);
    put(manager + 0x130, 1);
    unsigned countReads = 0;
    auto changing = [&](uint32_t address, void* out, size_t length) {
        if (address == manager + 0x130 && ++countReads == 2) put(address, 0);
        return read(address, out, length);
    };
    assert(inspectRegistry(manager, changing).status == RegistryStatus::Changed);
    std::cout << "Registry snapshot tests passed; no native calls.\n";
}
