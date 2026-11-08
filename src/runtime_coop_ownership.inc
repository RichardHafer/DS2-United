// Dead Space 2 United by Hafer.
// Snapshot only; never repair ownership while entities are being initialized.
namespace ownership {
char report[8192]{};

void Capture() {
    size_t used = 0;
    report[0] = 0;
    uint32_t manager = 0, slots = 0, capacity = 0;
    if (!Read(base + kPlayerManagerRva, &manager, 4) || !manager ||
        !Read(manager + 4, &slots, 4) || !slots ||
        !Read(manager + 8, &capacity, 4) || !capacity || capacity > 8) {
        AppendReport(report, sizeof(report), used, "coop ownership manager_unavailable=%08X", manager);
        return;
    }
    AppendReport(report, sizeof(report), used, "coop ownership manager=%08X", manager);
    uint32_t buffers[32]{};
    unsigned bufferCount = 0, players = 0;
    for (uint32_t slot = 0; slot < capacity; ++slot) {
        uint32_t bp = 0, vt = 0;
        if (!Read(slots + slot * 4, &bp, 4)) {
            AppendReport(report, sizeof(report), used, " slot%u=unreadable", slot);
            continue;
        }
        if (!bp) continue;
        const uint32_t p = bp >= 0x103C0 ? bp - 0x3C0 : 0;
        if (!p || !Read(p, &vt, 4) || vt != base + placement::kPlayerPrimaryVtableRva) {
            AppendReport(report, sizeof(report), used, " slot%u=non_player:%08X", slot, bp);
            continue;
        }
        ++players;
        uint32_t id = ~0U, controller = ~0U, proxy = 0, flags = 0;
        uint32_t camera[3]{}, uiRef = 0, uiVt = 0, owner = 0;
        bool readable = Read(p + 0x420, &id, 4) && Read(p + 0x41C, &controller, 4) &&
            Read(p + 0x68, &proxy, 4) && (!proxy || Read(proxy + 0x30, &flags, 4));
        for (unsigned i = 0; i < 3; ++i)
            readable = Read(p + 0x548 + i * 8, &camera[i], 4) && readable;
        readable = Read(p + 0x1660, &uiRef, 4) && readable;
        const uint32_t ui = uiRef >= 0x10010 ? uiRef - 0x10 : 0;
        const bool uiValid = ui && Read(ui, &uiVt, 4) && uiVt == base + 0x1726D70 &&
            Read(ui + 0x184, &owner, 4);
        unsigned invalidInventories = 0, sharedBuffers = 0;
        uint32_t counts[4]{}, currency[4]{};
        bool currencyReadable = true;
        for (unsigned i = 0; i < 4; ++i) {
            currencyReadable = Read(p + 0xBD0 + i * 0x34 + 0x28, &currency[i], 4) &&
                currencyReadable;
            uint32_t inv[5]{};
            if (!Read(p + 0xBD0 + i * 0x34, inv, sizeof(inv)) ||
                inv[0] != base + 0x174D844 || inv[2] > inv[3] || inv[3] > 65536 ||
                (inv[3] && !inv[1])) {
                ++invalidInventories;
                continue;
            }
            counts[i] = inv[2];
            if (inv[1]) {
                for (unsigned j = 0; j < bufferCount; ++j)
                    if (buffers[j] == inv[1]) { ++sharedBuffers; break; }
                buffers[bufferCount++] = inv[1];
            }
        }
        const bool ghost = (flags & 2) != 0;
        uint32_t lifecycleFlags = 0, retiringId[2]{};
        const bool lifecycleReadable = Read(p + 0x3D8, &lifecycleFlags, 4) &&
            Read(p + 0x16E4, retiringId, sizeof(retiringId));
        uint32_t sm = 0, smVt = 0, smOwner = 0;
        const bool smReadable = Read(p + 0x24C, &sm, 4) && sm && Read(sm, &smVt, 4);
        const bool smLocal = smReadable && smVt == base + 0x1727598;
        const bool smGhost = smReadable && smVt == base + 0x1726FD0;
        const bool smKnown = (smLocal || smGhost) && Read(sm + 0x50, &smOwner, 4);
        AppendReport(report, sizeof(report), used,
            " [slot=%u player=%08X id=%u controller=%u proxy=%08X ghost=%d readable=%d"
            " slot_mismatch=%d role_mismatch=%d cameras=%08X,%08X,%08X"
            " ui=%08X ui_valid=%d ui_owner=%08X owner_mismatch=%d"
            " inventory_counts=%u,%u,%u,%u invalid_inventories=%u shared_buffers=%u"
            " currency_readable=%d currency=%u,%u,%u,%u"
            " lifecycle_readable=%d lifecycle_flags=%08X delete_wait=%d respawn_pending=%d"
            " observer_pending=%d retiring_netid=%08X:%08X"
            " sm=%08X sm_vt=%08X sm_known=%d sm_owner=%08X sm_mismatch=%d]",
            slot, p, id, controller, proxy, ghost, readable, id != slot,
            (slot == 0) == ghost, camera[0], camera[1], camera[2], ui, uiValid,
            owner, uiValid && owner != p, counts[0], counts[1], counts[2], counts[3],
            invalidInventories, sharedBuffers, currencyReadable,
            currency[0], currency[1], currency[2], currency[3],
            lifecycleReadable, lifecycleFlags, (lifecycleFlags & 1) != 0,
            (lifecycleFlags & 0x800) != 0, (lifecycleFlags & 0x1000) != 0,
            retiringId[0], retiringId[1], sm, smVt, smKnown, smOwner,
            smKnown && (smOwner != p || smGhost != ghost));
    }
    AppendReport(report, sizeof(report), used, " players=%u", players);
}
}  // namespace ownership
