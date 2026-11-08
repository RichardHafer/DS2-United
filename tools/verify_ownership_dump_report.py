"""Replay retail dump snapshots through the compiled plugin ownership reporter."""
import argparse
import json
import struct
import subprocess
from pathlib import Path

from inspect_player_ownership import Dump, inspect


def snapshot(dump, evidence):
    result = {}

    def add(address, size):
        raw = dump.read(address, size)
        if not raw or len(raw) != size:
            raise ValueError(f"missing dump range {address:08X}+{size:X}")
        result[address] = bytearray(raw)

    add(0x201C0D4, 4)
    manager = dump.u32(0x201C0D4)
    add(manager, 16)
    add(dump.u32(manager + 4), dump.u32(manager + 8) * 4)
    for player in evidence["players"]:
        add(player["player"], 0x1800)
        if player["proxy"]:
            add(player["proxy"], 0x54)
        if player["inventory_ui"]["entity"]:
            add(player["inventory_ui"]["entity"], 0x190)
        if player["state_machine"]["root"]:
            add(player["state_machine"]["root"], 0x60)
    return result


def run(probe, path, ranges):
    with path.open("wb") as output:
        for address, data in sorted(ranges.items()):
            output.write(struct.pack("<II", address, len(data)) + data)
    return subprocess.check_output([str(probe), str(path)], text=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--probe", type=Path, default=Path(__file__).resolve().parents[1] /
                        "build/ownership_dump_probe.exe")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    dump = Dump(args.dump)
    try:
        evidence = inspect(dump)
        ranges = snapshot(dump, evidence)
    finally:
        dump.close()
    if len(evidence["players"]) != 2:
        raise ValueError("This check requires a two-player network dump")
    report = run(args.probe, args.output / "ownership_snapshot.bin", ranges)
    (args.output / "ownership_report.txt").write_text(report)
    for field in ("slot_mismatch", "role_mismatch", "owner_mismatch",
                  "invalid_inventories", "shared_buffers", "sm_mismatch"):
        assert report.count(" " + field + "=0") == 2, (field, report)
    for field in ("readable", "ui_valid", "sm_known"):
        assert report.count(" " + field + "=1") == 2, (field, report)
    assert "players=2" in report
    assert report.count("currency_readable=1") == 2
    assert report.count("lifecycle_readable=1") == 2
    for player in evidence["players"]:
        raw = ranges[player["player"]]
        money = [struct.unpack_from("<I", raw, 0xBD0 + i * 0x34 + 0x28)[0]
                 for i in range(4)]
        assert "currency=" + ",".join(map(str, money)) in report
    local = next(p for p in evidence["players"] if p["slot"] == 0)
    ghost = next(p for p in evidence["players"] if p["ghost"])
    assert f"save_policy player={local['player']:08X} role=1" in report
    assert f"save_policy player={ghost['player']:08X} role=2" in report
    assert local["state_machine"]["vtable"] == 0x1B27598
    assert ghost["state_machine"]["vtable"] == 0x1B26FD0
    mutated = {address: bytearray(data) for address, data in ranges.items()}
    struct.pack_into("<I", mutated[local["player"]], 0x420, 1)
    struct.pack_into("<I", mutated[ghost["inventory_ui"]["entity"]], 0x184, local["player"])
    struct.pack_into("<I", mutated[ghost["state_machine"]["root"]], 0, 0x1B27598)
    broken = run(args.probe, args.output / "ownership_injected_faults.bin", mutated)
    (args.output / "ownership_injected_faults.txt").write_text(broken)
    for field in ("slot_mismatch", "owner_mismatch", "sm_mismatch"):
        assert field + "=1" in broken, (field, broken)
    lifecycle = {address: bytearray(data) for address, data in ranges.items()}
    player_data = lifecycle[local["player"]]
    struct.pack_into("<I", player_data, 0x3D8, 0x1801)
    struct.pack_into("<II", player_data, 0x16E4, 0x12345678, 0xAABBCCDD)
    for i, value in enumerate([111, 222, 333, 444]):
        struct.pack_into("<I", player_data, 0xBD0 + i * 0x34 + 0x28, value)
    lifecycle_report = run(args.probe, args.output / "ownership_lifecycle.bin", lifecycle)
    (args.output / "ownership_lifecycle.txt").write_text(lifecycle_report)
    for expected in ("currency=111,222,333,444", "delete_wait=1 respawn_pending=1",
                     "observer_pending=1 retiring_netid=12345678:AABBCCDD"):
        assert expected in lifecycle_report, (expected, lifecycle_report)
    # Keep the player header readable while removing the saved NetID fields.
    lifecycle[local["player"]] = player_data[:0x16E4]
    unavailable = run(args.probe, args.output / "ownership_lifecycle_unreadable.bin", lifecycle)
    assert "lifecycle_readable=0" in unavailable
    summary = {"dump": str(args.dump), "players": len(evidence["players"]),
               "baseline": "passed", "injected_id_ui_owner_state_machine": "detected",
               "lifecycle_and_currency": "passed", "unreadable_lifecycle": "detected",
               "scope": "offline reporter replay; no game functions executed"}
    (args.output / "result.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary))


if __name__ == "__main__":
    main()
