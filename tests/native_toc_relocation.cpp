// Dead Space 2 United by Richard Hafer.
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

static std::vector<unsigned char> read(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open input");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

static uint32_t word(const std::vector<unsigned char>& data, size_t offset, size_t bytes) {
    if (offset > data.size() || bytes > data.size() - offset) throw std::runtime_error("Invalid table bounds");
    uint32_t value = 0;
    std::memcpy(&value, data.data() + offset, bytes);
    return value;
}

int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("Expected PC backup and TOC paths");
        auto exe = read(argv[1]);
        auto toc = read(argv[2]);
        constexpr size_t raw = 0x4480B0, length = 0xAC;
        if (exe.size() < raw + length) throw std::runtime_error("Short PC backup");
        uint64_t fingerprint = 14695981039346656037ull;
        for (size_t i = 0; i < length; ++i) fingerprint = (fingerprint ^ exe[raw + i]) * 1099511628211ull;
        if (fingerprint != 0x5142E5EB3193C3DAull) throw std::runtime_error("Unverified native routine");
        if (word(toc, 0, 4) != 0x9CBA7B28 || word(toc, 4, 4) != 10)
            throw std::runtime_error("Unsupported TOC");
        std::vector<size_t> slots;
        const auto files = word(toc, 8, 2), ids = word(toc, 10, 2), refs = word(toc, 12, 2);
        for (size_t i = 0; i < files; ++i) {
            slots.push_back(16 + i * 20);
            slots.push_back(32 + i * 20);
        }
        const size_t idStart = 16 + files * 20;
        for (size_t i = 0; i < ids; ++i) slots.push_back(idStart + i * 8 + 4);
        for (size_t i = 0; i < refs; ++i) slots.push_back(idStart + ids * 8 + i * 4);
        auto expected = toc;
        const auto base = reinterpret_cast<uint32_t>(toc.data());
        for (const auto slot : slots) {
            auto value = word(toc, slot, 4);
            if (value) {
                if (value >= toc.size()) throw std::runtime_error("Pointer outside TOC");
                value += base;
                std::memcpy(expected.data() + slot, &value, 4);
            }
        }
        // The verified leaf has no calls or absolute data references.
        void* code = VirtualAlloc(nullptr, length, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!code) throw std::runtime_error("Code allocation failed");
        std::memcpy(code, exe.data() + raw, length);
        DWORD previous = 0;
        if (!VirtualProtect(code, length, PAGE_EXECUTE_READ, &previous)) throw std::runtime_error("Code protection failed");
        FlushInstructionCache(GetCurrentProcess(), code, length);
        using Relocate = bool(__thiscall*)(void*);
        const bool result = reinterpret_cast<Relocate>(code)(toc.data());
        VirtualFree(code, 0, MEM_RELEASE);
        if (!result || toc != expected) throw std::runtime_error("Native relocation mismatch");
        std::cout << "Native PC relocation matched every byte; pointer slots: " << slots.size() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
