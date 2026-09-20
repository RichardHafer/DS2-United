#include "../src/construction_binding.h"
#include <cassert>
#include <cstdio>

int main() {
    using namespace ds2::native;
    ConstructionBinding binding;
    uint32_t output = 123;
    assert(binding.replacement(0x1E41A, 5, 4, output) == BindResult::Inactive && !output);
    assert(!binding.arm(0, 2, 3, 4));
    assert(!binding.arm(1, 2, 3, 3));
    assert(binding.arm(1, 2, 3, 4));
    assert(binding.bindOwner(0, 1, 2, 5) == BindResult::WrongCaller);
    assert(binding.bindOwner(0x3CF40D, 99, 2, 5) == BindResult::WrongPacket);
    assert(binding.bindOwner(0x3CF40D, 1, 99, 5) == BindResult::WrongList);
    assert(binding.bindOwner(0x3CF40D, 1, 2, 0) == BindResult::InvalidOwner);
    assert(binding.bindOwner(0x3CF40D, 1, 2, 5) == BindResult::Matched);
    assert(binding.bindOwner(0x3CF40D, 1, 2, 6) == BindResult::AlreadyBound);
    assert(binding.replacement(0, 5, 4, output) == BindResult::WrongCaller);
    assert(binding.replacement(0x1E41A, 6, 4, output) == BindResult::WrongOwner);
    assert(binding.replacement(0x1E41A, 5, 99, output) == BindResult::WrongHeader);
    assert(binding.replacement(0x1E41A, 5, 4, output) == BindResult::Matched && output == 3);
    assert(binding.replacement(0x1E459, 5, 4, output) == BindResult::Consumed && !output);
    assert(binding.arm(1, 2, 3, 4));
    assert(binding.bindOwner(0x3CF40D, 1, 2, 5) == BindResult::Matched);
    assert(binding.replacement(0x1E459, 5, 4, output) == BindResult::Matched && output == 3);
    std::puts("Construction binding tests passed.");
}
