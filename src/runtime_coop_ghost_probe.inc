// Dead Space 2 United by Richard Hafer.
namespace ghost_probe {
constexpr uint32_t kCreate = 0x3D1F00;
safetyhook::InlineHook hook;
bool installed = false;
volatile LONG calls = 0, returned = 0, failed = 0, dropped = 0;
struct Event {
    uint32_t id, phase, hash, subkey, stream, lookup, node, classID;
    uint32_t guid[4], reflectable, entity, vtable, bodyInspected, view, mmi, bones;
};
std::mutex mutex;
Event events[64]{};
unsigned count = 0;

void Queue(const Event& event) {
    std::lock_guard<std::mutex> guard(mutex);
    if (count < std::size(events)) events[count++] = event;
    else InterlockedIncrement(&dropped);
}

void Lookup(Event& event) {
    uint32_t manager = 0, node = 0;
    if (!Read(base + 0x1C1C0A0, &manager, 4) || !manager ||
        !Read(manager + 0x70 + (event.hash & 4095) * 4, &node, 4)) return;
    event.lookup = 1;
    for (unsigned n = 0; node && n < 4096; ++n) {
        uint32_t hash = 0;
        if (!Read(node + 0x1C, &hash, 4)) { event.lookup = 3; return; }
        if (hash == event.hash) {
            event.lookup = 2;
            event.node = node;
            uint32_t handle = 0, packet = 0, descriptor = 0;
            if (!Read(node + 0xC, event.guid, sizeof(event.guid)) ||
                !Read(node + 8, &handle, 4) || !handle || !Read(handle, &packet, 4) || !packet ||
                !Read(packet, &descriptor, 4) || !descriptor || !Read(descriptor + 8, &event.classID, 4)) event.lookup = 3;
            return;
        }
        if (!Read(node + 0x34, &node, 4)) { event.lookup = 3; return; }
    }
    if (node) event.lookup = 4;
}

uint32_t __cdecl OnCreate(uint32_t hash, uint32_t subkey, uint32_t stream) {
    Event event{};
    event.id = static_cast<uint32_t>(InterlockedIncrement(&calls));
    event.hash = hash; event.subkey = subkey; event.stream = stream;
    Lookup(event);
    Queue(event);
    const uint32_t result = hook.ccall<uint32_t>(hash, subkey, stream);
    event.phase = 1;
    event.reflectable = result;
    if (result >= 0x1005C) {
        InterlockedIncrement(&returned);
        event.entity = result - 0x5C;
        Read(event.entity, &event.vtable, 4);
        // Only these concrete Animated layouts have been checked in retail code.
        if (event.vtable == base + 0x17294D0 || event.vtable == base + 0x1783948 || event.vtable == base + 0x1789018 || event.vtable == base + kNecroPrimaryVtableRva) {
            event.bodyInspected = 1;
            Read(event.entity + 0x194, &event.view, 4);
            Read(event.entity + 0x140, &event.mmi, 4);
            if (event.view) Read(event.view + 0xB8, &event.bones, 4);
        }
    } else InterlockedIncrement(&failed);
    Queue(event);
    return result;
}

void Install(FILE* log, const std::string& ini) {
    const bool requested = GetPrivateProfileIntA("Coop", "GhostFactoryProbe", 0, ini.c_str()) != 0;
    std::fprintf(log, "coop_ghost_probe config=%d\n", requested);
    if (!requested) return;
    const unsigned char expected[] = {0x83,0xEC,0x24,0x53,0x56,0x57,0x8B,0x3D,0xA0,0xC0,0x01,0x02,0x51,0x8B,0x4C,0x24};
    unsigned char actual[sizeof(expected)]{};
    if (!Read(base + kCreate, actual, sizeof(actual)) || std::memcmp(actual, expected, sizeof(actual))) {
        std::fprintf(log, "coop_ghost_probe signature_mismatch; disabled\n"); return;
    }
    hook = safetyhook::create_inline(reinterpret_cast<void*>(base + kCreate), reinterpret_cast<void*>(OnCreate));
    installed = static_cast<bool>(hook);
    std::fprintf(log, "coop_ghost_probe signature_ok hook=%d va=%08X\n", installed, static_cast<unsigned>(base + kCreate));
}

void Flush(FILE* log) {
    if (!installed) return;
    Event pending[64]{};
    unsigned size = 0;
    {
        std::lock_guard<std::mutex> guard(mutex);
        size = count;
        std::copy_n(events, size, pending);
        count = 0;
    }
    for (unsigned i = 0; i < size; ++i) {
        const auto& e = pending[i];
        std::fprintf(log, "coop_ghost_factory id=%u phase=%s netid=%08X:%08X stream=%08X lookup=%u template=%08X class=%08X guid=%08X-%08X-%08X-%08X reflectable=%08X entity=%08X vt=%08X body_inspected=%u view=%08X mmi=%08X bones=%08X\n",
            e.id, e.phase ? "return" : "begin", e.hash, e.subkey, e.stream, e.lookup, e.node, e.classID,
            e.guid[0], e.guid[1], e.guid[2], e.guid[3], e.reflectable, e.entity, e.vtable, e.bodyInspected, e.view, e.mmi, e.bones);
    }
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (now - last < 1000) return;
    last = now;
    std::fprintf(log, "coop_ghost_factory calls=%ld returned=%ld null=%ld queue_dropped=%ld\n",
        InterlockedCompareExchange(&calls, 0, 0), InterlockedCompareExchange(&returned, 0, 0),
        InterlockedCompareExchange(&failed, 0, 0), InterlockedCompareExchange(&dropped, 0, 0));
}
} // namespace ghost_probe
