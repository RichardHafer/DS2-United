// Dead Space 2 United by Richard Hafer.
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
#include "../src/vr_compat.h"
int main(int argc, char** argv) {
    const auto check = [](const char* text) {
        return ds2_vr::HasMarkers(reinterpret_cast<const unsigned char*>(text), std::strlen(text));
    };
    assert(!check(""));
    assert(!check("ReShade d3d9 DS2PCVR"));
    assert(!check("DS2PCVR ds2stereo.ini"));
    assert(!check("ds2stereo.ini vr_settings Apt"));
    assert(check("DS2PCVR ds2stereo.ini vr_settings Apt loader/assets conflict"));
    for (int i = 1; i < argc; ++i) {
        std::ifstream file(argv[i], std::ios::binary);
        assert(file);
        std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(file), {}};
        assert(ds2_vr::HasMarkers(bytes.data(), bytes.size()));
    }
    std::printf("VR marker tests passed; local detection: %s\n", ds2_vr::Name(ds2_vr::Detect()));
}
