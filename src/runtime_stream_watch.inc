// Read-only worker snapshots; native lists can change during sampling.
struct WatchedStream {
    ds2::native::StreamState state;
    DWORD since;
};

bool SameStreamState(const ds2::native::StreamState& a, const ds2::native::StreamState& b) {
    return a.pointer == b.pointer && a.descriptor == b.descriptor && a.handle == b.handle &&
        a.status == b.status && a.flags == b.flags && a.dependencyRefs == b.dependencyRefs &&
        a.notificationBits == b.notificationBits && a.waitHandleCount == b.waitHandleCount;
}

void LogStreamWatchEntry(FILE* log, const char* event, const WatchedStream& entry, DWORD now) {
    const auto& s = entry.state;
    uint32_t header = 0, fields[2]{};
    char name[160]{};
    bool named = s.descriptor && Read(s.descriptor, &header, 4) && header &&
        Read(header, fields, sizeof(fields)) && fields[1];
    if (named) {
        named = false;
        for (unsigned i = 0; i < sizeof(name) - 1; ++i) {
            if (!Read(static_cast<uintptr_t>(fields[1]) + i, &name[i], 1)) break;
            if (!name[i]) { named = true; break; }
        }
    }
    std::fprintf(log, "stream_watch event=%s tick=%lu unchanged_ms=%lu pointer=%08X descriptor=%08X handle=%08X identity=%08X status=%u flags=%08X refs=%u notification_bits=%08X wait_count=%u name=%s\n",
        event, now, now - entry.since, s.pointer, s.descriptor, s.handle, fields[0], s.status,
        s.flags, static_cast<unsigned>(s.dependencyRefs), s.notificationBits, s.waitHandleCount,
        named ? name : "<name_unavailable>");
}

void PollStreamWatch(FILE* log) {
    static DWORD lastSample = 0, lastHeartbeat = 0;
    static uint32_t previousManager = 0;
    static std::map<uint32_t, WatchedStream> previous;
    const DWORD now = GetTickCount();
    if (now - lastSample < 5000) return;
    lastSample = now;
    uint32_t manager = 0, service = 0;
    const bool managerRead = Read(base + 0x1C1C098, &manager, 4);
    const bool serviceRead = Read(base + 0x1C1C0C8, &service, 4);
    const auto snapshot = ds2::native::inspectStreamStates(manager, Read);
    uint32_t after = 0;
    const bool consistent = managerRead && Read(base + 0x1C1C098, &after, 4) && after == manager &&
        snapshot.result == ds2::native::StreamStates::Stable;
    const bool heartbeat = now - lastHeartbeat >= 30000;
    if (manager != previousManager) previous.clear();
    unsigned changed = 0, removed = 0, pending = 0, emitted = 0;
    std::map<uint32_t, WatchedStream> current;
    if (consistent) {
        for (const auto& s : snapshot.entries) {
            const auto old = previous.find(s.handle);
            const bool same = old != previous.end() && SameStreamState(old->second.state, s);
            WatchedStream entry{s, same ? old->second.since : now};
            current.emplace(s.handle, entry);
            const bool waiting = s.status <= 1 || s.waitHandleCount ||
                (s.status == 3 && s.notificationBits != 0x80000000u);
            if (waiting) ++pending;
            if (!same) ++changed;
            if ((!same || (heartbeat && waiting)) && emitted < 64) {
                LogStreamWatchEntry(log, same ? "pending_sample" : "state_sample", entry, now);
                ++emitted;
            }
        }
        for (const auto& [handle, entry] : previous) {
            if (current.count(handle)) continue;
            ++removed;
            if (emitted < 64) {
                std::fprintf(log, "stream_watch event=absent tick=%lu handle=%08X pointer=%08X previous_status=%u\n",
                    now, handle, entry.state.pointer, entry.state.status);
                ++emitted;
            }
        }
        previous = std::move(current);
    }
    std::fprintf(log, "stream_watch_sample tick=%lu manager=%08X manager_read=%d service=%08X service_read=%d result=%u consistent=%d entries=%zu changed=%u absent=%u pending=%u detail_rows=%u detail_limit=64 snapshot_atomic=0\n",
        now, manager, managerRead, service, serviceRead, static_cast<unsigned>(snapshot.result),
        consistent, snapshot.entries.size(), changed, removed, pending, emitted);
    previousManager = manager;
    if (heartbeat) lastHeartbeat = now;
}
